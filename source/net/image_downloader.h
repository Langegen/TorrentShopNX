#pragma once

#include <borealis.hpp>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <unordered_set>

namespace net {

struct ImageTask {
    brls::Image* img = nullptr;
    std::string url;
    std::string cacheKey;
    std::shared_ptr<bool> token;
    std::string placeholder;
    bool bypassCache = false;
    std::string fallbackUrl;
    int row = -1;
    int col = -1;
    int priorityOverride = 0;
};

/// Синглтон очереди скачивания изображений с ограничением параллелизма.
class ImageDownloader {
public:
    static ImageDownloader& instance() {
        static ImageDownloader inst;
        return inst;
    }

    /// Инициализация пула потоков (по умолчанию 2 параллельных загрузки)
    void init(size_t maxConcurrent = 2);

    /// Остановка и очистка очереди
    void stop();

    /// Приостановить приём новых задач (текущие докачиваются)
    void pause();

    /// Возобновить приём задач
    void resume();

    /// Обновить текущую позицию фокуса для приоритизации очереди обложек
    void setFocusedPosition(int row, int col);

    /// Добавить задачу на скачивание изображения в очередь
    void enqueue(brls::Image* img,
                 const std::string& url,
                 const std::string& cacheKey,
                 std::shared_ptr<bool> token = nullptr,
                 const std::string& placeholder = "romfs:/img/borealis_96.png",
                 bool bypassCache = false,
                 const std::string& fallbackUrl = "",
                 int row = -1,
                 int col = -1,
                 int priorityOverride = 0);

private:
    ImageDownloader() = default;
    ~ImageDownloader();

    ImageDownloader(const ImageDownloader&) = delete;
    ImageDownloader& operator=(const ImageDownloader&) = delete;

    void workerLoop();
    void processTask(const ImageTask& task);
    int calculatePriority(const ImageTask& task) const;
    void reprioritizeQueueLocked();

    std::vector<std::thread> workers_;
    std::deque<ImageTask> tasks_;
    std::mutex queueMutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    int currentFocusedRow_ = -1;
    int currentFocusedCol_ = -1;
};

} // namespace net
