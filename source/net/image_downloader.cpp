#include "image_downloader.h"
#include "http_client.h"
#include "../utils/log.h"
#include <borealis/core/cache_helper.hpp>

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

void ImageDownloader::enqueue(brls::Image* img,
                               const std::string& url,
                               const std::string& cacheKey,
                               std::shared_ptr<bool> token,
                               const std::string& placeholder,
                               bool bypassCache,
                               const std::string& fallbackUrl) {
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
        // Push to FRONT so newly visible items load first (LIFO order)
        tasks_.push_front(ImageTask{img, url, cacheKey, token, placeholder, bypassCache, fallbackUrl});
    }
    cv_.notify_one();
}

void ImageDownloader::workerLoop() {
    while (running_.load() && !g_appExiting.load()) {
        ImageTask task;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            cv_.wait(lock, [this]() {
                return !tasks_.empty() || !running_.load() || g_appExiting.load();
            });

            if (!running_.load() || g_appExiting.load()) {
                break;
            }

            if (tasks_.empty()) continue;

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
    http.setTimeout(4);
    auto res = http.httpGet(task.url);

    if (g_appExiting.load()) return;

    if (res.status_code == 200 && !res.body.empty()) {
        brls::sync([img = task.img, cacheKey = task.cacheKey, body = std::move(res.body), token = task.token, bypassCache = task.bypassCache]() {
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
                }
            } else {
                if (brls::TextureCache::instance().getCache(cacheKey) == 0) {
                    int tex = nvgCreateImageMem(
                        brls::Application::getNVGContext(),
                        NVG_IMAGE_GENERATE_MIPMAPS,
                        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(body.data())),
                        body.size()
                    );
                    if (tex > 0) {
                        brls::TextureCache::instance().addCache(cacheKey, tex);
                    }
                }
                if (token && !*token) return;
                img->setImageFromFile(cacheKey);
            }
        });
    } else if (!task.fallbackUrl.empty()) {
        if (task.token && !*task.token) return;
        enqueue(task.img, task.fallbackUrl, task.cacheKey, task.token, task.placeholder, task.bypassCache, "");
    }
}

} // namespace net
