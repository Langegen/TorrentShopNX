#include "image_downloader.h"
#include "http_client.h"
#include "../config/config.h"
#include "../utils/log.h"
#include "../utils/app_paths.h"
#include <borealis/core/cache_helper.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>

// Vendored stb headers emit a wall of "defined but not used" for their
// static API surface; the implementation is intentionally compiled into this
// TU. Silence the noise locally instead of patching vendored code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <borealis/extern/nanovg/stb_image.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma GCC diagnostic pop

namespace net {

namespace {

std::string getThumbnailCachePath(const std::string& url) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : url) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));

    return std::string(TSNX_CACHE_THUMBNAILS) + "/" + std::string(hex) + ".jpg";
}

bool readWholeFileLocal(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    std::streamsize size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        in.read(&out[0], size);
        if (!in && in.gcount() != size) {
            out.resize(static_cast<size_t>(in.gcount()));
        }
    }
    return true;
}

void ensureParentDirectory(const std::string& filePath) {
    std::filesystem::path p(filePath);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
}

std::vector<uint8_t> resizeImageAreaAverage(const uint8_t* src, int srcW, int srcH, int channels, int dstW, int dstH) {
    std::vector<uint8_t> dst(dstW * dstH * channels);
    float xRatio = static_cast<float>(srcW) / static_cast<float>(dstW);
    float yRatio = static_cast<float>(srcH) / static_cast<float>(dstH);

    for (int y = 0; y < dstH; ++y) {
        float srcYStart = y * yRatio;
        float srcYEnd = (y + 1) * yRatio;
        int y0 = static_cast<int>(srcYStart);
        int y1 = std::min(srcH, static_cast<int>(srcYEnd + 0.9999f));

        for (int x = 0; x < dstW; ++x) {
            float srcXStart = x * xRatio;
            float srcXEnd = (x + 1) * xRatio;
            int x0 = static_cast<int>(srcXStart);
            int x1 = std::min(srcW, static_cast<int>(srcXEnd + 0.9999f));

            float totalWeight = 0.0f;
            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};

            for (int sy = y0; sy < y1; ++sy) {
                float yWeight = 1.0f;
                if (sy < srcYStart) yWeight -= (srcYStart - sy);
                if (sy + 1 > srcYEnd) yWeight -= (sy + 1 - srcYEnd);
                if (yWeight < 0.0f) yWeight = 0.0f;

                for (int sx = x0; sx < x1; ++sx) {
                    float xWeight = 1.0f;
                    if (sx < srcXStart) xWeight -= (srcXStart - sx);
                    if (sx + 1 > srcXEnd) xWeight -= (sx + 1 - srcXEnd);
                    if (xWeight < 0.0f) xWeight = 0.0f;

                    float w = xWeight * yWeight;
                    totalWeight += w;
                    const uint8_t* p = src + (sy * srcW + sx) * channels;
                    for (int c = 0; c < channels; ++c) {
                        acc[c] += p[c] * w;
                    }
                }
            }

            uint8_t* out = dst.data() + (y * dstW + x) * channels;
            if (totalWeight > 0.0001f) {
                for (int c = 0; c < channels; ++c) {
                    float v = acc[c] / totalWeight;
                    out[c] = static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
                }
            }
        }
    }
    return dst;
}

void stbiWriteToVector(void* context, void* data, int size) {
    auto* vec = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    vec->insert(vec->end(), bytes, bytes + size);
}

} // namespace

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
            util::logLine("ImageDownloader: task skipped (token invalidated) url=" + task.url);
            continue; // Skip invalidated task (card scrolled offscreen)
        }

        processTask(task);
    }
}

void ImageDownloader::processTask(const ImageTask& task) {
    if (g_appExiting.load()) return;
    if (task.token && !*task.token) return;

    bool cacheEnabled = config::ConfigManager::instance().getCacheCoverThumbnails() && !task.bypassCache;
    std::string thumbPath;
    if (cacheEnabled && !task.url.empty()) {
        thumbPath = getThumbnailCachePath(task.url);
        std::string cachedBody;
        if (readWholeFileLocal(thumbPath, cachedBody) && !cachedBody.empty()) {
            util::logLine("ImageDownloader: loaded thumbnail from disk: " + thumbPath + " (" + std::to_string(cachedBody.size()) + " bytes)");
            brls::sync([img = task.img, cacheKey = task.cacheKey, body = std::move(cachedBody), token = task.token, bypassCache = task.bypassCache, url = task.url, row = task.row, col = task.col]() {
                if ((token && !*token) || g_appExiting.load()) return;

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
                        util::logLine("ImageDownloader: nvgCreateImageMem failed for disk cached " + url);
                    }
                }
                if (token && !*token) return;
                if (tex > 0) {
                    img->innerSetImage(tex);
                }
            });
            return;
        }
    }

    net::HttpClient http;
    http.setTimeout(5);
    auto res = http.httpGet(task.url);

    if (g_appExiting.load()) return;

    if (res.status_code == 200 && !res.body.empty()) {
        util::logLine("ImageDownloader: HTTP 200 OK for url=" + task.url + " (size=" + std::to_string(res.body.size()) + ")");
        
        std::string bodyToDisplay = std::move(res.body);

        if (cacheEnabled && !thumbPath.empty()) {
            int srcW = 0, srcH = 0, comp = 0;
            stbi_uc* decoded = stbi_load_from_memory(
                reinterpret_cast<const stbi_uc*>(bodyToDisplay.data()),
                static_cast<int>(bodyToDisplay.size()),
                &srcW, &srcH, &comp, 3
            );
            if (decoded) {
                const int maxW = 160;
                const int maxH = 245;
                float scale = std::min(static_cast<float>(maxW) / static_cast<float>(srcW),
                                       static_cast<float>(maxH) / static_cast<float>(srcH));
                int dstW = (scale < 1.0f) ? std::max(1, static_cast<int>(srcW * scale + 0.5f)) : srcW;
                int dstH = (scale < 1.0f) ? std::max(1, static_cast<int>(srcH * scale + 0.5f)) : srcH;

                std::vector<uint8_t> resizedData;
                const uint8_t* pixelData = nullptr;

                if (dstW < srcW || dstH < srcH) {
                    resizedData = resizeImageAreaAverage(decoded, srcW, srcH, 3, dstW, dstH);
                    pixelData = resizedData.data();
                } else {
                    pixelData = decoded;
                }

                std::vector<uint8_t> thumbJpeg;
                stbi_write_jpg_to_func(stbiWriteToVector, &thumbJpeg, dstW, dstH, 3, pixelData, 80);
                stbi_image_free(decoded);

                if (!thumbJpeg.empty()) {
                    ensureParentDirectory(thumbPath);
                    std::ofstream out(thumbPath, std::ios::binary);
                    if (out.is_open()) {
                        out.write(reinterpret_cast<const char*>(thumbJpeg.data()), thumbJpeg.size());
                        out.close();
                        util::logLine("ImageDownloader: saved thumbnail to " + thumbPath + " (" +
                                      std::to_string(thumbJpeg.size()) + " bytes, " +
                                      std::to_string(dstW) + "x" + std::to_string(dstH) + ")");
                    }
                    bodyToDisplay.assign(reinterpret_cast<const char*>(thumbJpeg.data()), thumbJpeg.size());
                }
            }
        }

        brls::sync([img = task.img, cacheKey = task.cacheKey, body = std::move(bodyToDisplay), token = task.token, bypassCache = task.bypassCache, url = task.url, row = task.row, col = task.col]() {
            if (token && !*token) {
                util::logLine("ImageDownloader: set skipped (token invalidated) row=" + std::to_string(row) +
                              " col=" + std::to_string(col) + " url=" + url);
                return;
            }
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
