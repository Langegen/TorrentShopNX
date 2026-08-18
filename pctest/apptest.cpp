// apptest — PC-реплика полного потока TorrentShopNX поверх custom engine:
//
//   FileSelectView (CustomEngineClient::probeFiles)
//     -> DownloadManager (tsnx_engine_set_file_wanted, priorities)
//     -> CustomEngineBackend::open / prebuffer / read
//     -> HybridNspInstaller: потоки Collector (RingBuffer, chunk 4MB) и
//        Installer (prebuffer 32MB, чтение 4MB, starvation>=500ms)
//     -> DownloadManager: опрос tsnx_engine_get_torrents каждые 800мс,
//        статистика collector/installer каждые 5с (формат как в log.txt)
//
// Использует РЕАЛЬНЫЕ классы приложения (CustomEngineBackend,
// CustomEngineScheduler, CustomEngineHealth, CustomEngineClient, RingBuffer),
// собранные вместе с движком. Отличие от приложения только одно: вместо
// NCM-инсталляции Thread B просто пожирает байты из RingBuffer.

#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>

#include <engine/engine.h>

extern "C" void engine_log_set_level(int level);
enum { ENGINE_LOG_DEBUG = 3 };
#include "../source/datasource/custom_engine_client.h"
#include "../source/datasource/custom_engine_backend.h"
#include "../source/buffer/ring_buffer.h"
#include "../source/utils/log.h"

// extern из utils/log.h
std::atomic<bool> g_appExiting{false};

// Та же функция, что в content_backend_factory.cpp (который тянет за собой
// внешние бэкенды — для PC-теста не нужен).
namespace datasource {
const char* streamStateName(StreamState state) {
    switch (state) {
        case StreamState::Idle: return "Idle";
        case StreamState::FetchingMetadata: return "FetchingMetadata";
        case StreamState::FileSelected: return "FileSelected";
        case StreamState::PrebufferInstallInfo: return "PrebufferInstallInfo";
        case StreamState::InstallInfoParsed: return "InstallInfoParsed";
        case StreamState::MainBuffering: return "MainBuffering";
        case StreamState::StreamingOrInstalling: return "StreamingOrInstalling";
        case StreamState::Stalled: return "Stalled";
        case StreamState::Completed: return "Completed";
        case StreamState::Stopping: return "Stopping";
        case StreamState::Error: return "Error";
    }
    return "Unknown";
}
} // namespace datasource

// Константы как в hybrid_nsp_installer.cpp
static constexpr size_t CHUNK_SIZE   = 4 * 1024 * 1024;      // LOCAL_STREAM_CHUNK_SIZE
static constexpr size_t PREBUFFER    = 32 * 1024 * 1024;     // LOCAL_PREBUFFER_TARGET_SIZE
static constexpr size_t RING_CAP     = 128 * 1024 * 1024;    // autoBufferSize(), title mode

struct Stats {
    std::atomic<uint64_t> downloaded{0};   // bytes_downloaded_
    std::atomic<uint64_t> installed{0};    // bytes_installed_
    std::atomic<uint64_t> starvation{0};   // starvation_count_
    std::atomic<uint64_t> collector_retries{0};
};

// Копия HybridNspInstaller::collectorThreadFunc (без парсинга NSP-заголовка,
// данные уже готовы): backend->read -> RingBuffer.write, partial/retry как в
// оригинале, лог статистики каждые 5с в формате log.txt.
static void collectorThread(datasource::CustomEngineBackend* backend,
                            buffer::RingBuffer* rb,
                            std::atomic<bool>* cancel,
                            Stats* st,
                            uint64_t data_end) {
    uint64_t data_offset = 0;
    uint64_t current     = 0;
    std::vector<uint8_t> chunk_buf(CHUNK_SIZE);

    int retry_count = 0;
    const int max_retries = 12;
    auto last_stats_at = std::chrono::steady_clock::now();
    uint64_t last_stats_bytes = 0;

    while (current < data_end && !cancel->load() && !g_appExiting.load()) {
        size_t to_read = static_cast<size_t>(
            std::min<uint64_t>(CHUNK_SIZE, data_end - current));
        int64_t got =
            backend->read(static_cast<int64_t>(data_offset + current),
                          chunk_buf.data(), static_cast<int64_t>(to_read));
        if (cancel->load() || g_appExiting.load()) break;
        size_t read = got > 0 ? static_cast<size_t>(got) : 0;

        if (read == 0 || read < to_read) {
            if (read > 0) {
                retry_count = 0;
                rb->write(chunk_buf.data(), read);
                current += read;
                st->downloaded = current;
            } else {
                ++retry_count;
                if (retry_count >= max_retries) {
                    std::printf("collector: retry limit reached at offset=%llu\n",
                                (unsigned long long)current);
                    break;
                }
                st->collector_retries++;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        } else {
            retry_count = 0;
            rb->write(chunk_buf.data(), read);
            current += read;
            st->downloaded = current;
        }

        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration<double>(now - last_stats_at).count();
        if (elapsed >= 5.0) {
            const uint64_t downloaded = st->downloaded.load();
            const uint64_t delta = downloaded >= last_stats_bytes
                ? downloaded - last_stats_bytes : 0;
            const double speed_kbps = elapsed > 0.0
                ? static_cast<double>(delta) / 1024.0 / elapsed : 0.0;
            std::printf("collector: stats downloaded=%llu speed=%dKB/s rb_avail=%zu rb_free=%zu rb_cap=%zu\n",
                        (unsigned long long)downloaded, (int)speed_kbps,
                        rb->available(), rb->freeSpace(), rb->capacity());
            last_stats_at = now;
            last_stats_bytes = downloaded;
        }
    }
    rb->setEof();
    std::printf("collector: finished, downloaded %llu bytes\n",
                (unsigned long long)st->downloaded.load());
}

// Копия HybridNspInstaller::installerThreadFunc: ожидание local prebuffer
// (32MB), затем чтение по CHUNK_SIZE; ожидание >= 500ms = starvation.
static void installerThread(buffer::RingBuffer* rb,
                            std::atomic<bool>* cancel,
                            Stats* st) {
    std::vector<uint8_t> chunk_buf(CHUNK_SIZE);
    std::printf("installer: waiting local prebuffer target=%zu\n", PREBUFFER);
    while (!cancel->load() && rb->available() < PREBUFFER)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("installer: local prebuffer ready available=%zu\n", rb->available());

    auto last_stats_at = std::chrono::steady_clock::now();
    uint64_t last_installed = 0;
    uint64_t last_downloaded = 0;

    while (!cancel->load()) {
        const auto t0 = std::chrono::steady_clock::now();
        size_t read = rb->read(chunk_buf.data(), CHUNK_SIZE);
        const auto t1 = std::chrono::steady_clock::now();
        const auto wait_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (wait_ms >= 500 && read > 0) {
            st->starvation++;
            std::printf("installer: buffer wait wait_ms=%lld got=%zu rb_avail=%zu downloaded=%llu installed=%llu starvation_total=%llu\n",
                        (long long)wait_ms, read, rb->available(),
                        (unsigned long long)st->downloaded.load(),
                        (unsigned long long)st->installed.load(),
                        (unsigned long long)st->starvation.load());
        }
        if (read == 0) break;
        st->installed += read;

        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration<double>(now - last_stats_at).count();
        if (elapsed >= 5.0) {
            const uint64_t installed = st->installed.load();
            const uint64_t downloaded = st->downloaded.load();
            const uint64_t installed_delta = installed >= last_installed
                ? installed - last_installed : 0;
            const uint64_t downloaded_delta = downloaded >= last_downloaded
                ? downloaded - last_downloaded : 0;
            const double install_kbps = elapsed > 0.0
                ? static_cast<double>(installed_delta) / 1024.0 / elapsed : 0.0;
            const double source_kbps = elapsed > 0.0
                ? static_cast<double>(downloaded_delta) / 1024.0 / elapsed : 0.0;
            std::printf("installer: stats installed=%llu install_speed=%dKB/s downloaded=%llu source_speed=%dKB/s rb_avail=%zu rb_free=%zu rb_cap=%zu\n",
                        (unsigned long long)installed, (int)install_kbps,
                        (unsigned long long)downloaded, (int)source_kbps,
                        rb->available(), rb->freeSpace(), rb->capacity());
            last_stats_at = now;
            last_installed = installed;
            last_downloaded = downloaded;
        }
    }
}

int main(int argc, char** argv) {
    const char* magnet =
        "magnet:?xt=urn:btih:838E7B98569A3C00C8B868B6E362468F5F78AA7B"
        "&tr=http%3A%2F%2Fbt2.t-ru.org%2Fann%3Fmagnet"
        "&dn=%5BNintendo%20Switch%5D%20The%20Legend%20of%20Zelda%3A%20Skyward%20Sword%20HD%20%5BNSZ%5D%5BRUS%2FMulti9%5D";
    int duration_sec = 90;
    int file_index = -1;

    if (argc > 1) {
        if (strncmp(argv[1], "magnet:", 7) == 0) {
            magnet = argv[1];
            if (argc > 2) file_index = atoi(argv[2]);
            if (argc > 3) duration_sec = atoi(argv[3]);
        } else {
            duration_sec = atoi(argv[1]);
            if (argc > 2 && strncmp(argv[2], "magnet:", 7) == 0) {
                magnet = argv[2];
                if (argc > 3) file_index = atoi(argv[3]);
            }
        }
    }

    util::logInit();
    setvbuf(stdout, NULL, _IONBF, 0);
    engine_log_set_level(ENGINE_LOG_DEBUG);
    std::printf("AppTest: duration=%ds magnet=%.60s...\n", duration_sec, magnet);
    fflush(stdout);

    // --- 1. Пробинг файлов как в FileSelectView ---------------------------
    datasource::CustomEngineClient& client = datasource::CustomEngineClient::instance();
    std::vector<datasource::CustomEngineFileInfo> files;
    std::string err;
    if (!client.probeFiles("", magnet, "", files, &err)) {
        std::printf("AppTest: probe failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("AppTest: probe ok, %zu files\n", files.size());
    if (file_index < 0) {
        for (size_t i = 1; i < files.size(); i++)
            if (files[i].size > files[file_index < 0 ? 0 : (size_t)file_index].size)
                file_index = (int)i;
        if (file_index < 0) file_index = 0;
    }
    std::printf("AppTest: selected file index=%d size=%lld path=%s\n",
                file_index, (long long)files[file_index].size,
                files[file_index].path.c_str());
    fflush(stdout);

    std::string hash;
    {
        // Хеш из probeStatus/engine — как DownloadManager ищет торрент.
        tsnx_torrent_item items[8];
        int n = tsnx_engine_get_torrents(client.sharedEngine(), items, 8);
        for (int i = 0; i < n; i++) {
            if (files.size() && i == 0) hash = items[i].hash;
        }
    }
    std::printf("AppTest: hash=%s\n", hash.c_str());

    // --- 2. Приоритеты файлов как в DownloadManager (priorities_set) -----
    for (size_t i = 0; i < files.size(); i++)
        tsnx_engine_set_file_wanted(nullptr, hash.c_str(), (int)i,
                                    (int)i == file_index);

    // --- 3. Бэкенд как в CustomEngineBackend ------------------------------
    datasource::BackendConfig cfg;
    datasource::CustomEngineBackend backend(cfg);
    std::atomic<bool> cancel{false};
    backend.setCancelFlag(&cancel);

    datasource::ContentRequest req;
    req.info_hash  = hash;
    req.magnet_link = magnet;
    req.file_index = file_index;
    if (!backend.open(req)) {
        std::printf("AppTest: backend open failed\n");
        return 1;
    }
    std::printf("AppTest: backend open ok, total=%llu piece=%d\n",
                (unsigned long long)backend.status().total_size,
                backend.pieceSize());

    // --- 4. Чтение заголовка как в parseNspHeaderPhase --------------------
    {
        uint8_t probe[4096];
        std::printf("AppTest: reading NSP header probe (4096 bytes)...\n");
        fflush(stdout);
        auto t0 = std::chrono::steady_clock::now();
        int64_t got = backend.read(0, probe, sizeof(probe));
        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::printf("AppTest: header read got=%lld waited_ms=%lld\n",
                    (long long)got, (long long)ms);
        // notifyInstallInfoParsed(offset=336, chunk) -> prebuffer
        backend.prebuffer(336, CHUNK_SIZE);
        std::printf("AppTest: prebuffer notified\n");
    }

    // --- 5. Collector + Installer потоки как в startStreamingPhase --------
    buffer::RingBuffer ring_buffer(RING_CAP);
    Stats st;
    std::thread collector(collectorThread, &backend, &ring_buffer, &cancel, &st,
                          static_cast<uint64_t>(files[file_index].size));
    std::thread installer(installerThread, &ring_buffer, &cancel, &st);

    // --- 6. Опрос как DownloadManager (800мс) + статус бэкенда -------------
    const auto start = std::chrono::steady_clock::now();
    double dl_sum = 0.0, dl_peak = 0.0;
    int dl_samples = 0;
    double rb_sum = 0.0;
    int rb_samples = 0;
    uint64_t bytes_recv_start = 0;
    {
        tsnx_engine_diag d0;
        if (tsnx_engine_get_diag(client.sharedEngine(), hash.c_str(), &d0))
            bytes_recv_start = (uint64_t)d0.bytes_recv;
    }
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        tsnx_torrent_item items[8];
        int n = tsnx_engine_get_torrents(client.sharedEngine(), items, 8);
        const tsnx_torrent_item* it = nullptr;
        for (int i = 0; i < n; i++)
            if (hash == items[i].hash) { it = &items[i]; break; }

        datasource::BackendStatus bs = backend.status();
        const char* stname = streamStateName(bs.state);

        if (it) {
            dl_sum += it->download_kbps;
            if (it->download_kbps > dl_peak) dl_peak = it->download_kbps;
            dl_samples++;
        }
        rb_sum += static_cast<double>(ring_buffer.available());
        rb_samples++;

        tsnx_engine_diag dg;
        char diagbuf[128] = {0};
        if (tsnx_engine_get_diag(client.sharedEngine(), hash.c_str(), &dg)) {
            std::snprintf(diagbuf, sizeof(diagbuf),
                          "claim=%d idle=%d empty_bf=%d calm=%d done=%lld",
                          dg.claiming, dg.idle, dg.empty_bitfield, dg.calm,
                          (long long)dg.pieces_done);
        }

        std::printf("[%6.1fs] st=%-22s dl=%.1fKB/s seeds=%d peers=%d known=%d dht=%d progress=%5.2f%% down=%5.1fMB inst=%5.1fMB rb=%3zuMB starv=%llu retries=%llu | %s | %s\n",
                    elapsed, stname,
                    it ? it->download_kbps : 0.0f,
                    it ? it->seeds : 0, it ? it->peers : 0,
                    it ? it->known_peers : 0, it ? it->dht_nodes : 0,
                    it ? (it->progress * 100.0) : 0.0,
                    st.downloaded.load() / (1024.0 * 1024.0),
                    st.installed.load() / (1024.0 * 1024.0),
                    ring_buffer.available() / (1024 * 1024),
                    (unsigned long long)st.starvation.load(),
                    (unsigned long long)st.collector_retries.load(),
                    bs.detail.c_str(), diagbuf);
        fflush(stdout);

        if (elapsed >= duration_sec) break;
    }

    // --- 7. Остановка как cancelDownload: cancel -> source->close() ------
    // (close() -> torrentfs_cancel разблокирует заблокированный read)
    std::printf("AppTest: stopping...\n");
    uint64_t bytes_recv_end = 0, dup_end = 0;
    {
        tsnx_engine_diag d1;
        if (tsnx_engine_get_diag(client.sharedEngine(), hash.c_str(), &d1)) {
            bytes_recv_end = (uint64_t)d1.bytes_recv;
            dup_end = (uint64_t)d1.dup_bytes;
        }
    }
    cancel = true;
    g_appExiting = true;
    backend.close();
    ring_buffer.setEof();
    if (collector.joinable()) collector.join();
    if (installer.joinable()) installer.join();

    tsnx_engine_remove_torrent(client.sharedEngine(), hash.c_str());
    client.releaseProbeTorrent();

    std::printf("AppTest: done. down=%lluMB inst=%lluMB starv=%llu\n",
                (unsigned long long)(st.downloaded.load() / (1024 * 1024)),
                (unsigned long long)(st.installed.load() / (1024 * 1024)),
                (unsigned long long)st.starvation.load());
    {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        const uint64_t down = st.downloaded.load();
        // Фактическая скорость: скачанные байты / время (как и просили), и
        // отдельно — принятый сетевой трафик движка за то же время.
        const uint64_t net = bytes_recv_end - bytes_recv_start;
        const uint64_t useful = net > dup_end ? net - dup_end : 0;
        std::printf("AppTest: SUMMARY elapsed=%.0fs\n"
                    "  read_bytes=%lluMB  FACT_avg=%.1fKB/s (read/elapsed)\n"
                    "  net_recv=%lluMB     NET_avg=%.1fKB/s (engine bytes_recv/elapsed)\n"
                    "  useful=%lluMB       USEFUL_avg=%.1fKB/s (net minus dups)\n"
                    "  dup_bytes=%lluMB (%.0f%%)  ewma_mean=%.1fKB/s ewma_peak=%.1fKB/s rb_avg=%.1fMB starv=%llu retries=%llu\n",
                    elapsed,
                    (unsigned long long)(down / (1024 * 1024)),
                    elapsed > 0 ? (double)down / 1024.0 / elapsed : 0.0,
                    (unsigned long long)(net / (1024 * 1024)),
                    elapsed > 0 ? (double)net / 1024.0 / elapsed : 0.0,
                    (unsigned long long)(useful / (1024 * 1024)),
                    elapsed > 0 ? (double)useful / 1024.0 / elapsed : 0.0,
                    (unsigned long long)(dup_end / (1024 * 1024)),
                    net > 0 ? 100.0 * (double)dup_end / (double)net : 0.0,
                    dl_samples > 0 ? dl_sum / dl_samples : 0.0, dl_peak,
                    rb_samples > 0 ? rb_sum / rb_samples / (1024 * 1024) : 0.0,
                    (unsigned long long)st.starvation.load(),
                    (unsigned long long)st.collector_retries.load());
    }
    return 0;
}
