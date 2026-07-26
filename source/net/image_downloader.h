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

namespace net {

struct ImageTask {
    brls::Image* img = nullptr;
    std::string url;
    std::string cacheKey;
    std::shared_ptr<bool> token;
    std::string placeholder;
    bool bypassCache = false;
    std::string fallbackUrl;
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

    /// Добавить задачу на скачивание изображения в очередь
    void enqueue(brls::Image* img,
                 const std::string& url,
                 const std::string& cacheKey,
                 std::shared_ptr<bool> token = nullptr,
                 const std::string& placeholder = "romfs:/img/borealis_96.png",
                 bool bypassCache = false,
                 const std::string& fallbackUrl = "");

private:
    ImageDownloader() = default;
    ~ImageDownloader();

    ImageDownloader(const ImageDownloader&) = delete;
    ImageDownloader& operator=(const ImageDownloader&) = delete;

    void workerLoop();
    void processTask(const ImageTask& task);

    std::vector<std::thread> workers_;
    std::deque<ImageTask> tasks_;
    std::mutex queueMutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
};

} // namespace net
