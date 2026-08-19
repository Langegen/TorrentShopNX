#include "image_downloader.h"
#include "http_client.h"
#include "../utils/log.h"
#include <borealis/core/cache_helper.hpp>
#include <algorithm>
#include <cmath>

namespace net {

ImageDownloader::~ImageDownloader() {
    stop();
}

void ImageDownloader::init(size_t maxConcurrent) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (running_.load()) return;

    running_.store(true);
    for (size_t i = 0; i < maxConcurrent; ++i) {
        workers_.emplace_back(&ImageDownloader::workerLoop, this);
    }
    util::logLine("ImageDownloader: initialized with " + std::to_string(maxConcurrent) + " workers");
}

void ImageDownloader::stop() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!running_.load()) return;
        running_.store(false);
        paused_.store(false);
        tasks_.clear();
    }
    cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    util::logLine("ImageDownloader: stopped");
}

void ImageDownloader::pause() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!running_.load() || paused_.load()) return;
        paused_.store(true);
    }
    cv_.notify_all();
    util::logLine("ImageDownloader: paused");
}

void ImageDownloader::resume() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!running_.load() || !paused_.load()) return;
        paused_.store(false);
    }
    cv_.notify_all();
    util::logLine("ImageDownloader: resumed");
}

int ImageDownloader::calculatePriority(const ImageTask& task) const {
    if (task.priorityOverride > 0) {
        return task.priorityOverride;
    }

    if (task.row < 0 || currentFocusedRow_ < 0) return 100000;

    int d_row = std::abs(task.row - currentFocusedRow_);
    int d_col = (task.col >= 0 && currentFocusedCol_ >= 0) ? std::abs(task.col - currentFocusedCol_) : 0;

    if (d_row == 0) {
        if (d_col == 0) {
            return 1000000;
        }
        return 900000 - d_col * 1000;
    }
    return 800000 - d_row * 10000 - d_col * 10;
}

void ImageDownloader::reprioritizeQueueLocked() {
    if (tasks_.empty()) return;

    std::stable_sort(tasks_.begin(), tasks_.end(), [this](const ImageTask& a, const ImageTask& b) {
        return calculatePriority(a) > calculatePriority(b);
    });
}

void ImageDownloader::setFocusedPosition(int row, int col) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (currentFocusedRow_ == row && currentFocusedCol_ == col) return;
    currentFocusedRow_ = row;
    currentFocusedCol_ = col;
    reprioritizeQueueLocked();
    cv_.notify_all();
}

void ImageDownloader::enqueue(brls::Image* img,
                               const std::string& url,
                               const std::string& cacheKey,
                               std::shared_ptr<bool> token,
                               const std::string& placeholder,
                               bool bypassCache,
                               const std::string& fallbackUrl,
                               int row,
                               int col,
                               int priorityOverride) {
    if (url.empty() || !img) {
        if (img) img->setImageFromFile(placeholder);
        return;
    }

    if (token && !*token) return;

    // Set placeholder on UI thread immediately
    if (bypassCache) {
        img->setImageFromFile(placeholder);
        img->setFreeTexture(true);
    } else {
        if (brls::TextureCache::instance().getCache(cacheKey) > 0) {
            img->setImageFromFile(cacheKey);
            return;
        }
        img->setImageFromFile(placeholder);
    }

    // Lazy init if workers were not explicitly started
    if (!running_.load()) {
        init(2);
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        // Prune invalidated tasks if queue grows
        if (tasks_.size() > 30) {
            for (auto it = tasks_.begin(); it != tasks_.end(); ) {
                if (it->token && !*it->token) {
                    it = tasks_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        ImageTask newTask{img, url, cacheKey, token, placeholder, bypassCache, fallbackUrl, row, col, priorityOverride};
        int newPriority = calculatePriority(newTask);
        auto insertPos = std::lower_bound(tasks_.begin(), tasks_.end(), newPriority, [this](const ImageTask& t, int targetPriority) {
            return calculatePriority(t) > targetPriority;
        });
        tasks_.insert(insertPos, std::move(newTask));
    }
    cv_.notify_one();
}

void ImageDownloader::workerLoop() {
    while (running_.load() && !g_appExiting.load()) {
        ImageTask task;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            cv_.wait(lock, [this]() {
                return !running_.load() || g_appExiting.load() ||
                       (!paused_.load() && !tasks_.empty());
            });

            if (!running_.load() || g_appExiting.load()) {
                break;
            }

            if (paused_.load() || tasks_.empty()) continue;

            task = tasks_.front();
            tasks_.pop_front();
        }

        if (task.token && !*task.token) {
            continue; // Skip invalidated task (card scrolled offscreen)
        }

        processTask(task);
    }
}

void ImageDownloader::processTask(const ImageTask& task) {
    if (g_appExiting.load()) return;
    if (task.token && !*task.token) return;

    net::HttpClient http;
    http.setTimeout(5);
    auto res = http.httpGet(task.url);

    if (g_appExiting.load()) return;

    if (res.status_code == 200 && !res.body.empty()) {
        util::logLine("ImageDownloader: HTTP 200 OK for url=" + task.url + " (size=" + std::to_string(res.body.size()) + ")");
        brls::sync([img = task.img, cacheKey = task.cacheKey, body = std::move(res.body), token = task.token, bypassCache = task.bypassCache, url = task.url]() {
            if (token && !*token) return;
            if (g_appExiting.load()) return;

            if (bypassCache) {
                int tex = nvgCreateImageMem(
                    brls::Application::getNVGContext(),
                    NVG_IMAGE_GENERATE_MIPMAPS,
                    const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(body.data())),
                    body.size()
                );
                if (tex > 0) {
                    if (token && !*token) {
                        nvgDeleteImage(brls::Application::getNVGContext(), tex);
                        return;
                    }
                    img->innerSetImage(tex);
                } else {
                    util::logLine("ImageDownloader: nvgCreateImageMem failed for " + url + " (size=" + std::to_string(body.size()) + ")");
                }
            } else {
                int tex = brls::TextureCache::instance().getCache(cacheKey);
                if (tex == 0) {
                    tex = nvgCreateImageMem(
                        brls::Application::getNVGContext(),
                        NVG_IMAGE_GENERATE_MIPMAPS,
                        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(body.data())),
                        body.size()
                    );
                    if (tex > 0) {
                        brls::TextureCache::instance().addCache(cacheKey, tex);
                    } else {
                        util::logLine("ImageDownloader: nvgCreateImageMem failed for " + url + " (size=" + std::to_string(body.size()) + ")");
                    }
                }
                if (token && !*token) return;
                if (tex > 0) {
                    img->innerSetImage(tex);
                }
            }
        });
    } else {
        util::logLine("ImageDownloader: HTTP fetch failed, code=" + std::to_string(res.status_code) + " url=" + task.url);
        if (!task.fallbackUrl.empty()) {
            if (task.token && !*task.token) return;
            enqueue(task.img, task.fallbackUrl, task.cacheKey, task.token, task.placeholder, task.bypassCache, "", task.row, task.col, task.priorityOverride);
        }
    }
}

} // namespace net
