#include "ring_buffer.h"

#include <algorithm>
#include <cstring>

namespace buffer {

// =============================================================================
// Макросы для абстракции синхронизации (libnx vs std)
// =============================================================================
#ifdef __SWITCH__
  #define RB_LOCK()        mutexLock(&mutex_)
  #define RB_UNLOCK()      mutexUnlock(&mutex_)
  #define RB_WAIT_NE()     condvarWait(&cond_not_empty_, &mutex_)
  #define RB_WAIT_NF()     condvarWait(&cond_not_full_,  &mutex_)
  #define RB_SIGNAL_NE()   condvarWakeOne(&cond_not_empty_)
  #define RB_SIGNAL_NF()   condvarWakeOne(&cond_not_full_)
#else
  // std::unique_lock используется в wait(), но нам нужен manual lock/unlock
  // поэтому оборачиваем через unique_lock в месте ожидания
  #define RB_LOCK()        mutex_.lock()
  #define RB_UNLOCK()      mutex_.unlock()
  // Для host: используем lambdas ниже напрямую
  #define RB_SIGNAL_NE()   cond_not_empty_.notify_one()
  #define RB_SIGNAL_NF()   cond_not_full_.notify_one()
#endif

RingBuffer::RingBuffer(size_t capacity)
    : capacity_(capacity) {
    buffer_.resize(capacity_);
#ifdef __SWITCH__
    mutexInit(&mutex_);
    condvarInit(&cond_not_empty_);
    condvarInit(&cond_not_full_);
#endif
}

RingBuffer::~RingBuffer() = default;

size_t RingBuffer::write(const void* data, size_t size) {
    if (size == 0 || !data) return 0;

    const uint8_t* src = static_cast<const uint8_t*>(data);
    size_t total_written = 0;

    RB_LOCK();

    while (total_written < size) {
        // Ждём свободного места
        while (stored_ >= capacity_ && !eof_) {
#ifdef __SWITCH__
            RB_WAIT_NF();
#else
            std::unique_lock<std::mutex> lk(mutex_, std::adopt_lock);
            cond_not_full_.wait(lk, [this]{ return stored_ < capacity_ || eof_; });
            lk.release();
#endif
        }

        if (eof_) break;

        // Записываем столько, сколько можем
        size_t free = capacity_ - stored_;
        size_t to_write = std::min(size - total_written, free);

        // Копируем данные с учётом кольцевой структуры
        size_t first_chunk = std::min(to_write, capacity_ - write_pos_);
        std::memcpy(buffer_.data() + write_pos_, src + total_written, first_chunk);

        if (to_write > first_chunk) {
            // Оборот — продолжение с начала буфера
            std::memcpy(buffer_.data(), src + total_written + first_chunk,
                        to_write - first_chunk);
        }

        write_pos_ = (write_pos_ + to_write) % capacity_;
        stored_ += to_write;
        total_written += to_write;

        RB_SIGNAL_NE(); // Разбудить reader
    }

    RB_UNLOCK();
    return total_written;
}

size_t RingBuffer::read(void* buf, size_t size) {
    if (size == 0 || !buf) return 0;

    uint8_t* dst = static_cast<uint8_t*>(buf);
    size_t total_read = 0;

    RB_LOCK();

    // Ждём данных
    while (stored_ == 0 && !eof_) {
#ifdef __SWITCH__
        RB_WAIT_NE();
#else
        std::unique_lock<std::mutex> lk(mutex_, std::adopt_lock);
        cond_not_empty_.wait(lk, [this]{ return stored_ > 0 || eof_; });
        lk.release();
#endif
    }

    if (stored_ == 0 && eof_) {
        RB_UNLOCK();
        return 0; // EOF
    }

    // Читаем столько, сколько есть
    size_t to_read = std::min(size, stored_);

    size_t first_chunk = std::min(to_read, capacity_ - read_pos_);
    std::memcpy(dst, buffer_.data() + read_pos_, first_chunk);

    if (to_read > first_chunk) {
        std::memcpy(dst + first_chunk, buffer_.data(), to_read - first_chunk);
    }

    read_pos_ = (read_pos_ + to_read) % capacity_;
    stored_ -= to_read;
    total_read = to_read;

    RB_SIGNAL_NF(); // Разбудить writer

    RB_UNLOCK();
    return total_read;
}

size_t RingBuffer::available() const {
#ifdef __SWITCH__
    mutexLock(&mutex_);
    const size_t value = stored_;
    mutexUnlock(&mutex_);
    return value;
#else
    std::lock_guard<std::mutex> lk(mutex_);
    return stored_;
#endif
}

size_t RingBuffer::freeSpace() const {
#ifdef __SWITCH__
    mutexLock(&mutex_);
    const size_t value = capacity_ - stored_;
    mutexUnlock(&mutex_);
    return value;
#else
    std::lock_guard<std::mutex> lk(mutex_);
    return capacity_ - stored_;
#endif
}

bool RingBuffer::isEmpty() const {
    return available() == 0;
}

bool RingBuffer::isEof() const {
#if defined(__SWITCH__)
    mutexLock(&mutex_);
    const bool value = eof_;
    mutexUnlock(&mutex_);
#else
    std::lock_guard<std::mutex> lk(mutex_);
    const bool value = eof_;
#endif
    return value;
}

bool RingBuffer::isFull() const {
    return freeSpace() == 0;
}

void RingBuffer::setEof() {
    RB_LOCK();
    eof_ = true;
    RB_SIGNAL_NE(); // Разбудить reader, чтобы он заметил EOF
    RB_SIGNAL_NF(); // Разбудить writer, чтобы он остановился
    RB_UNLOCK();
}

void RingBuffer::reset() {
    RB_LOCK();
    read_pos_  = 0;
    write_pos_ = 0;
    stored_    = 0;
    eof_       = false;
    RB_UNLOCK();
}

void RingBuffer::reinit(size_t capacity) {
    RB_LOCK();
    capacity_ = capacity;
    buffer_.resize(capacity_);
    read_pos_  = 0;
    write_pos_ = 0;
    stored_    = 0;
    eof_       = false;
    RB_UNLOCK();
}

} // namespace buffer
