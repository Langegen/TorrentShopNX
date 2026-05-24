#pragma once
// =============================================================================
// PiecePool — пул предвыделенных буферов для кусков торрента.
//
// Назначение: Избежать heap allocation (new/delete) на горячем пути
//             MemoryStorage::writev. На Switch heap allocation занимает
//             ~10–50 мкс из-за фрагментации — это стоит ощутимо на 500+ KB/s.
//
// Использование:
//   auto pool = PiecePool::create(piece_size, kPoolCapacity);
//   char* buf = pool->acquire();  // nullptr если пул исчерпан → fallback new[]
//   pool->release(buf);
//
// Потокобезопасность: mutex защищает free_list (вызывается редко — 1 раз
// на кусок). На горячем пути (writev) acquire/release вызываются из одного
// потока libtorrent → mutex overhead минимален.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <cassert>

namespace buffer {

class PiecePool {
public:
    /// Создать пул с заданным размером буфера и числом слотов.
    /// @param piece_size  размер одного куска в байтах
    /// @param capacity    число буферов в пуле
    static std::unique_ptr<PiecePool> create(int piece_size, int capacity) {
        return std::unique_ptr<PiecePool>(new PiecePool(piece_size, capacity));
    }

    ~PiecePool() {
        // Все буферы в backing_store_, деструктор освободит их автоматически
    }

    // Запрет копирования
    PiecePool(const PiecePool&) = delete;
    PiecePool& operator=(const PiecePool&) = delete;

    // =========================================================================
    // Основной API
    // =========================================================================

    /// Получить буфер из пула.
    /// @return указатель на pre-allocated буфер или nullptr если пул исчерпан.
    ///         В случае nullptr вызывающий должен использовать new char[piece_size_].
    char* acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_list_.empty()) {
            ++misses_;
            return nullptr; // fallback: пусть вызывающий сделает new[]
        }
        char* ptr = free_list_.back();
        free_list_.pop_back();
        ++hits_;
        return ptr;
    }

    /// Вернуть буфер в пул.
    /// @return true если буфер принадлежал пулу и успешно возвращен, false если это был fallback буфер
    bool release(char* ptr) {
        if (!ptr) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& holder : backing_store_) {
            if (holder.get() == ptr) {
                free_list_.push_back(ptr);
                return true;
            }
        }
        return false;
    }

    int piece_size()    const { return piece_size_; }
    int capacity()      const { return static_cast<int>(backing_store_.size()); }
    int available()     const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(free_list_.size());
    }
    uint64_t hits()     const { return hits_; }
    uint64_t misses()   const { return misses_; }
    float    hit_rate() const {
        const uint64_t total = hits_ + misses_;
        return total > 0 ? static_cast<float>(hits_) / total : 0.0f;
    }

private:
    explicit PiecePool(int piece_size, int capacity)
        : piece_size_(piece_size) {
        assert(piece_size > 0 && capacity > 0);
        backing_store_.reserve(static_cast<size_t>(capacity));
        free_list_.reserve(static_cast<size_t>(capacity));

        for (int i = 0; i < capacity; ++i) {
            backing_store_.emplace_back(new char[piece_size]());
            free_list_.push_back(backing_store_.back().get());
        }
    }

    int piece_size_;

    // Владелец памяти — unique_ptr обеспечивает RAII без циклических проблем
    std::vector<std::unique_ptr<char[]>> backing_store_;

    // Список свободных буферов (raw pointer, не владеющий)
    mutable std::mutex mutex_;
    std::vector<char*> free_list_;

    uint64_t hits_   = 0;
    uint64_t misses_ = 0;
};

} // namespace buffer
