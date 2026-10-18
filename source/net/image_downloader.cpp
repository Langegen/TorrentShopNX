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
    tsnx_ensure_parent_dirs(filePath.c_str());
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

static bool isKnownBlockedHost(const std::string& host) {
    if (host.empty()) return false;
    return (host.find(".nintendo.net") != std::string::npos ||
            host.find(".nintendo.com") != std::string::npos);
}

std::string ImageDownloader::extractHost(const std::string& url) {
    if (url.empty()) return "";
    size_t start = 0;
    if (url.rfind("http://", 0) == 0) start = 7;
    else if (url.rfind("https://", 0) == 0) start = 8;
    else return "";

    size_t end = url.find('/', start);
    size_t colon = url.find(':', start);
    if (colon != std::string::npos && (end == std::string::npos || colon < end)) {
        end = colon;
    }
    if (end == std::string::npos) return url.substr(start);
    return url.substr(start, end - start);
}

bool ImageDownloader::isHostInCooldown(const std::string& host, bool& isHardCooldown, std::chrono::steady_clock::time_point& until) {
    isHardCooldown = false;
    if (host.empty()) return false;
    if (isKnownBlockedHost(host)) {
        isHardCooldown = true;
        until = std::chrono::steady_clock::now() + std::chrono::hours(24);
        return true;
    }
    std::lock_guard<std::mutex> lock(failedHostsMutex_);
    auto it = failedHosts_.find(host);
    if (it == failedHosts_.end()) return false;
    auto now = std::chrono::steady_clock::now();
    if (now < it->second.until) {
        isHardCooldown = (it->second.consecutiveFails >= 3);
        until = it->second.until;
        return true;
    }
    // Cooldown expired
    if (it->second.consecutiveFails < 3) {
        failedHosts_.erase(it);
    }
    return false;
}

bool ImageDownloader::isHostInCooldown(const std::string& host) {
    bool isHard = false;
    std::chrono::steady_clock::time_point until;
    return isHostInCooldown(host, isHard, until);
}

void ImageDownloader::markHostFailed(const std::string& host) {
    if (host.empty()) return;
    std::lock_guard<std::mutex> lock(failedHostsMutex_);
    auto& entry = failedHosts_[host];
    entry.consecutiveFails++;
    // Transient failure: short cooldown 2s (or 3s on 2nd fail).
    // Hard failure (3+ consecutive fails): host is confirmed dead/blocked (e.g. 90DNS), cooldown 1 hour.
    int cooldownSec = 2;
    if (entry.consecutiveFails == 2) {
        cooldownSec = 3;
    } else if (entry.consecutiveFails >= 3) {
        cooldownSec = 3600;
    }
    entry.until = std::chrono::steady_clock::now() + std::chrono::seconds(cooldownSec);
    util::logLine("ImageDownloader: marked host " + host + " in cooldown for " +
                  std::to_string(cooldownSec) + "s (consecutive fails=" + std::to_string(entry.consecutiveFails) + ")");
}

void ImageDownloader::markHostSuccess(const std::string& host) {
    if (host.empty()) return;
    std::lock_guard<std::mutex> lock(failedHostsMutex_);
    failedHosts_.erase(host);
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
    if (currentFocusedRow_.load(std::memory_order_relaxed) == row &&
        currentFocusedCol_.load(std::memory_order_relaxed) == col) return;
    currentFocusedRow_.store(row, std::memory_order_relaxed);
    currentFocusedCol_.store(col, std::memory_order_relaxed);
    needsSort_.store(true, std::memory_order_release);
    cv_.notify_one();
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

    // Set placeholder on UI thread immediately without marking shared placeholder as freeable
    if (bypassCache) {
        img->setImageFromFile(placeholder);
        img->setFreeTexture(false);
    } else {
        if (brls::TextureCache::instance().getCache(cacheKey) > 0) {
            img->setImageFromFile(cacheKey);
            return;
        }
        img->setImageFromFile(placeholder);
    }

    enqueueInternal(img, url, cacheKey, token, placeholder, bypassCache, fallbackUrl, row, col, priorityOverride);
}

void ImageDownloader::enqueueInternal(brls::Image* img,
                                       const std::string& url,
                                       const std::string& cacheKey,
                                       std::shared_ptr<bool> token,
                                       const std::string& placeholder,
                                       bool bypassCache,
                                       const std::string& fallbackUrl,
                                       int row,
                                       int col,
                                       int priorityOverride) {
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
    net::HttpClient http;
    http.setKeepAlive(true);

    while (running_.load() && !g_appExiting.load()) {
        ImageTask task;
        bool hasTask = false;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            while (running_.load() && !g_appExiting.load()) {
                if (paused_.load() || tasks_.empty()) {
                    cv_.wait(lock, [this]() {
                        return !running_.load() || g_appExiting.load() ||
                               (!paused_.load() && !tasks_.empty());
                    });
                    if (!running_.load() || g_appExiting.load()) break;
                    if (paused_.load() || tasks_.empty()) continue;
                }

                if (needsSort_.exchange(false, std::memory_order_acq_rel)) {
                    reprioritizeQueueLocked();
                }

                auto earliestCooldown = std::chrono::steady_clock::time_point::max();
                bool foundExecutableTask = false;

                for (auto it = tasks_.begin(); it != tasks_.end(); ) {
                    if (it->token && !*it->token) {
                        it = tasks_.erase(it);
                        continue;
                    }

                    std::string host = extractHost(it->url);
                    bool isHard = false;
                    std::chrono::steady_clock::time_point until;
                    if (!host.empty() && isHostInCooldown(host, isHard, until)) {
                        if (isHard) {
                            // Host is confirmed dead/blocked (3+ consecutive fails, e.g. 90DNS).
                            // Discard with placeholder immediately so we don't stall the queue.
                            util::logLine("ImageDownloader: host " + host + " in hard cooldown, discarding " + it->url);
                            ImageTask deadTask = std::move(*it);
                            it = tasks_.erase(it);
                            if (!deadTask.placeholder.empty() && deadTask.img) {
                                brls::sync([img = deadTask.img, token = deadTask.token, placeholder = deadTask.placeholder]() {
                                    if (g_appExiting.load()) return;
                                    if (token && !*token) return;
                                    if (img) {
                                        img->setImageFromFile(placeholder);
                                        img->setFreeTexture(false);
                                    }
                                });
                            }
                            continue;
                        }

                        // Soft cooldown (transient backoff, 2-3s).
                        // Keep this task in the queue and record when to wake up!
                        if (until < earliestCooldown) {
                            earliestCooldown = until;
                        }
                        ++it;
                        continue;
                    }

                    // Host is ready to fetch!
                    task = std::move(*it);
                    tasks_.erase(it);
                    foundExecutableTask = true;
                    break;
                }

                if (foundExecutableTask) {
                    hasTask = true;
                    break;
                }

                // If all remaining tasks in queue are in soft cooldown, wait until earliest cooldown expires or a new task arrives
                if (!tasks_.empty() && earliestCooldown != std::chrono::steady_clock::time_point::max()) {
                    cv_.wait_until(lock, earliestCooldown);
                } else if (tasks_.empty()) {
                    continue;
                }
            }
        }

        if (!hasTask || !running_.load() || g_appExiting.load()) {
            break;
        }

        if (task.token && !*task.token) {
            util::logLine("ImageDownloader: task skipped (token invalidated) url=" + task.url);
            continue; // Skip invalidated task (card scrolled offscreen)
        }

        processTask(task, http);
    }
}

void ImageDownloader::processTask(const ImageTask& task, net::HttpClient& http) {
    if (g_appExiting.load()) return;
    if (task.token && !*task.token) return;

    bool cacheEnabled = config::ConfigManager::instance().getCacheCoverThumbnails() && !task.bypassCache;
    std::string thumbPath = task.url.empty() ? "" : getThumbnailCachePath(task.url);
    if (cacheEnabled && !thumbPath.empty()) {
        std::string cachedBody;
        if (readWholeFileLocal(thumbPath, cachedBody) && !cachedBody.empty()) {
            util::logLine("ImageDownloader: loaded thumbnail from disk: " + thumbPath + " (" + std::to_string(cachedBody.size()) + " bytes)");
            // Pre-decode RGBA in worker thread to prevent main UI thread frame drops
            int dw = 0, dh = 0, dcomp = 0;
            stbi_uc* rgba = stbi_load_from_memory(
                reinterpret_cast<const stbi_uc*>(cachedBody.data()),
                static_cast<int>(cachedBody.size()),
                &dw, &dh, &dcomp, 4
            );
            brls::sync([img = task.img, cacheKey = task.cacheKey, rgba, dw, dh, token = task.token, bypassCache = task.bypassCache, url = task.url, row = task.row, col = task.col]() {
                if (g_appExiting.load()) {
                    if (rgba) stbi_image_free(rgba);
                    return;
                }

                int tex = brls::TextureCache::instance().getCache(cacheKey);
                if (tex == 0) {
                    if (rgba) {
                        tex = nvgCreateImageRGBA(
                            brls::Application::getNVGContext(),
                            dw, dh,
                            NVG_IMAGE_GENERATE_MIPMAPS,
                            rgba
                        );
                    }
                    if (tex > 0) {
                        brls::TextureCache::instance().addCache(cacheKey, tex);
                    } else {
                        util::logLine("ImageDownloader: nvgCreateImageRGBA failed for disk cached " + url);
                    }
                }
                if (rgba) {
                    stbi_image_free(rgba);
                }
                if (token && !*token) {
                    return;
                }
                if (tex > 0 && img) {
                    img->innerSetImage(tex);
                }
            });
            return;
        }
    }

    std::string host = extractHost(task.url);
    bool isHard = false;
    std::chrono::steady_clock::time_point until;
    if (!host.empty() && isHostInCooldown(host, isHard, until)) {
        if (!isHard) {
            // Soft cooldown active: re-queue task so it is preserved and retried when cooldown passes!
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                tasks_.push_back(task);
                needsSort_.store(true, std::memory_order_release);
            }
            cv_.notify_one();
            return;
        }
        util::logLine("ImageDownloader: host " + host + " in hard cooldown, skipping network fetch for " + task.url);
        std::string fbHost = extractHost(task.fallbackUrl);
        if (!task.fallbackUrl.empty() && fbHost != host && !isHostInCooldown(fbHost)) {
            if (task.token && !*task.token) return;
            enqueueInternal(task.img, task.fallbackUrl, task.cacheKey, task.token, task.placeholder, task.bypassCache, "", task.row, task.col, task.priorityOverride);
        } else if (!task.placeholder.empty()) {
            brls::sync([img = task.img, token = task.token, placeholder = task.placeholder]() {
                if (g_appExiting.load()) return;
                if (token && !*token) return;
                if (img) {
                    img->setImageFromFile(placeholder);
                    img->setFreeTexture(false);
                }
            });
        }
        return;
    }

    bool dlActive = isDownloadActive();
    int timeoutSec = dlActive ? 8 : 6;
    if (dlActive) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    http.setTimeout(timeoutSec);
    auto res = http.httpGet(task.url);

    if (g_appExiting.load()) return;

    if (res.status_code == 200 && !res.body.empty()) {
        util::logLine("ImageDownloader: HTTP 200 OK for url=" + task.url + " (size=" + std::to_string(res.body.size()) + ")");
        if (!host.empty()) {
            markHostSuccess(host);
        }
        
        std::string bodyToDisplay = std::move(res.body);

        if (cacheEnabled && !thumbPath.empty()) {
            int srcW = 0, srcH = 0, comp = 0;
            stbi_uc* decoded = stbi_load_from_memory(
                reinterpret_cast<const stbi_uc*>(bodyToDisplay.data()),
                static_cast<int>(bodyToDisplay.size()),
                &srcW, &srcH, &comp, 3
            );
            if (decoded) {
                // If it's a 3D box mock-up with side spine, crop the front artwork
                stbi_uc* sourcePixels = decoded;
                std::vector<uint8_t> croppedBuffer;
                int activeW = srcW;
                int activeH = srcH;

                if (srcW == 393 && srcH == 640) {
                    int cropX = 22;
                    int cropY = 24;
                    int cropW = 342;
                    int cropH = 594;

                    croppedBuffer.resize(cropW * cropH * 3);
                    for (int y = 0; y < cropH; ++y) {
                        const uint8_t* srcRow = decoded + ((cropY + y) * srcW + cropX) * 3;
                        uint8_t* dstRow = croppedBuffer.data() + (y * cropW) * 3;
                        std::memcpy(dstRow, srcRow, cropW * 3);
                    }
                    sourcePixels = croppedBuffer.data();
                    activeW = cropW;
                    activeH = cropH;
                }

                const int maxW = 160;
                const int maxH = 245;
                float scale = std::min(static_cast<float>(maxW) / static_cast<float>(activeW),
                                       static_cast<float>(maxH) / static_cast<float>(activeH));
                int dstW = (scale < 1.0f) ? std::max(1, static_cast<int>(activeW * scale + 0.5f)) : activeW;
                int dstH = (scale < 1.0f) ? std::max(1, static_cast<int>(activeH * scale + 0.5f)) : activeH;

                std::vector<uint8_t> resizedData;
                const uint8_t* pixelData = nullptr;

                if (dstW < activeW || dstH < activeH) {
                    resizedData = resizeImageAreaAverage(sourcePixels, activeW, activeH, 3, dstW, dstH);
                    pixelData = resizedData.data();
                } else {
                    pixelData = sourcePixels;
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

        // Pre-decode RGBA in worker thread before scheduling on main thread
        int dw = 0, dh = 0, dcomp = 0;
        stbi_uc* rgba = stbi_load_from_memory(
            reinterpret_cast<const stbi_uc*>(bodyToDisplay.data()),
            static_cast<int>(bodyToDisplay.size()),
            &dw, &dh, &dcomp, 4
        );

        brls::sync([img = task.img, cacheKey = task.cacheKey, rgba, dw, dh, token = task.token, bypassCache = task.bypassCache, url = task.url, row = task.row, col = task.col]() {
            if (g_appExiting.load()) {
                if (rgba) stbi_image_free(rgba);
                return;
            }

            if (bypassCache) {
                if (token && !*token) {
                    if (rgba) stbi_image_free(rgba);
                    util::logLine("ImageDownloader: set skipped (token invalidated) row=" + std::to_string(row) +
                                  " col=" + std::to_string(col) + " url=" + url);
                    return;
                }
                int tex = 0;
                if (rgba) {
                    tex = nvgCreateImageRGBA(
                        brls::Application::getNVGContext(),
                        dw, dh,
                        NVG_IMAGE_GENERATE_MIPMAPS,
                        rgba
                    );
                    stbi_image_free(rgba);
                }
                if (tex > 0) {
                    if (token && !*token) {
                        nvgDeleteImage(brls::Application::getNVGContext(), tex);
                        return;
                    }
                    if (img) {
                        img->innerSetImage(tex);
                        img->setFreeTexture(true); // Own dynamically created non-cached texture
                    }
                } else {
                    util::logLine("ImageDownloader: nvgCreateImageRGBA failed for " + url);
                }
            } else {
                int tex = brls::TextureCache::instance().getCache(cacheKey);
                if (tex == 0) {
                    if (rgba) {
                        tex = nvgCreateImageRGBA(
                            brls::Application::getNVGContext(),
                            dw, dh,
                            NVG_IMAGE_GENERATE_MIPMAPS,
                            rgba
                        );
                    }
                    if (tex > 0) {
                        brls::TextureCache::instance().addCache(cacheKey, tex);
                    } else {
                        util::logLine("ImageDownloader: nvgCreateImageRGBA failed for " + url);
                    }
                }
                if (rgba) {
                    stbi_image_free(rgba);
                }
                if (token && !*token) {
                    util::logLine("ImageDownloader: set skipped (token invalidated) row=" + std::to_string(row) +
                                  " col=" + std::to_string(col) + " url=" + url);
                    return;
                }
                if (tex > 0 && img) {
                    img->innerSetImage(tex);
                }
            }
        });
    } else {
        util::logLine("ImageDownloader: HTTP fetch failed, code=" + std::to_string(res.status_code) +
                      " (" + res.body + ") url=" + task.url);
        if (res.status_code == 0 && !host.empty()) {
            markHostFailed(host);
        }

        // If it was a network timeout / connection error (code == 0) and we haven't exhausted retries,
        // retry the task instead of giving up immediately and setting a placeholder (skip retrying known blocked hosts)
        if (res.status_code == 0 && !isKnownBlockedHost(host) && task.retryCount < 2) {
            ImageTask retryTask = task;
            retryTask.retryCount++;
            util::logLine("ImageDownloader: transient error for " + task.url + ", will retry (attempt " +
                          std::to_string(retryTask.retryCount) + "/2)");
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                tasks_.push_back(retryTask);
                needsSort_.store(true, std::memory_order_release);
            }
            cv_.notify_one();
            return;
        }

        std::string fbHost = extractHost(task.fallbackUrl);
        bool canTryFallback = !task.fallbackUrl.empty() &&
                              (res.status_code != 0 || fbHost != host) &&
                              !isHostInCooldown(fbHost);
        if (canTryFallback) {
            if (task.token && !*task.token) return;
            enqueueInternal(task.img, task.fallbackUrl, task.cacheKey, task.token, task.placeholder, task.bypassCache, "", task.row, task.col, task.priorityOverride);
        } else if (!task.placeholder.empty()) {
            brls::sync([img = task.img, token = task.token, placeholder = task.placeholder]() {
                if (g_appExiting.load()) return;
                if (token && !*token) return;
                if (img) {
                    img->setImageFromFile(placeholder);
                    img->setFreeTexture(false);
                }
            });
        }
    }

    if (dlActive) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

} // namespace net
