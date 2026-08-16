#pragma once
// =============================================================================
// RingBuffer — потокобезопасный кольцевой буфер.
// Используется для связи потоков Collector (запись) и Installer (чтение).
// Синхронизация через libnx Mutex/CondVar.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#else
// Заглушки для компиляции на хосте (тестирование)
#include <mutex>
#include <condition_variable>
#endif

namespace buffer {

class RingBuffer {
public:
    /// @param capacity размер буфера в байтах (рекомендуется 32-64MB)
    explicit RingBuffer(size_t capacity = 64 * 1024 * 1024);
    ~RingBuffer();

    // Запрет копирования
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    /// Записать данные в буфер (вызывается из потока Collector).
    /// Блокируется, если буфер полон, до появления свободного места.
    /// @return количество записанных байт
    size_t write(const void* data, size_t size);

    /// Прочитать данные из буфера (вызывается из потока Installer).
    /// Блокируется, если буфер пуст, до появления данных.
    /// @return количество прочитанных байт
    size_t read(void* buf, size_t size);

    /// Количество байт, доступных для чтения
    size_t available() const;

    /// Количество свободных байт для записи
    size_t freeSpace() const;

    bool isEmpty() const;
    bool isFull() const;
    bool isEof() const;

    /// Сигнализировать о завершении записи (EOF).
    /// После этого read() вернёт 0 когда буфер опустеет.
    void setEof();

    /// Сбросить буфер в начальное состояние
    void reset();

    /// Переинициализировать буфер с новым размером
    void reinit(size_t capacity);

    size_t capacity() const { return capacity_; }

private:
    std::vector<uint8_t> buffer_;
    size_t capacity_;
    size_t read_pos_  = 0;
    size_t write_pos_ = 0;
    size_t stored_    = 0; ///< количество данных в буфере
    bool   eof_       = false;

#ifdef __SWITCH__
    mutable Mutex mutex_;
    CondVar cond_not_empty_;
    CondVar cond_not_full_;
#else
    mutable std::mutex mutex_;
    std::condition_variable cond_not_empty_;
    std::condition_variable cond_not_full_;
#endif
};

} // namespace buffer
