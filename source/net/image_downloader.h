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
#include <unordered_map>
#include <chrono>

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
    int retryCount = 0;
    // When the image was first requested, for the [perf] load-latency metric.
    std::chrono::steady_clock::time_point enqueuedAt{};
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

    using ActiveCheckFn = std::function<bool()>;
    void setActiveCheckFn(ActiveCheckFn fn) { activeCheck_ = fn; }
    bool isDownloadActive() const { return activeCheck_ ? activeCheck_() : false; }

    /// Добавить задачу на скачивание изображения в очередь
    void enqueue(brls::Image* img,
                 const std::string& url,
                 const std::string& cacheKey,
                 std::shared_ptr<bool> token = nullptr,
                 const std::string& placeholder = "romfs:/img/cover_blank.png",
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
    void processTask(const ImageTask& task, class HttpClient& http);
    int calculatePriority(const ImageTask& task) const;
    void reprioritizeQueueLocked();
    void enqueueInternal(brls::Image* img,
                         const std::string& url,
                         const std::string& cacheKey,
                         std::shared_ptr<bool> token,
                         const std::string& placeholder,
                         bool bypassCache,
                         const std::string& fallbackUrl,
                         int row,
                         int col,
                         int priorityOverride);

    struct HostCooldownInfo {
        std::chrono::steady_clock::time_point until;
        int consecutiveFails = 0;
    };

    bool isHostInCooldown(const std::string& host, bool& isHardCooldown, std::chrono::steady_clock::time_point& until);
    bool isHostInCooldown(const std::string& host);
    void markHostFailed(const std::string& host);
    void markHostSuccess(const std::string& host);
    static std::string extractHost(const std::string& url);

    std::vector<std::thread> workers_;
    std::deque<ImageTask> tasks_;
    std::mutex queueMutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<int> currentFocusedRow_{-1};
    std::atomic<int> currentFocusedCol_{-1};
    std::atomic<bool> needsSort_{false};
    ActiveCheckFn activeCheck_;

    std::mutex failedHostsMutex_;
    std::unordered_map<std::string, HostCooldownInfo> failedHosts_;
};

} // namespace net
