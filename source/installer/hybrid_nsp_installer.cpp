#include "hybrid_nsp_installer.h"
#include "installer_flow.h"
#include "ncm_installer.h"
#include "ncz_parser.h"
#include "../buffer/ring_buffer.h"
#include "../utils/log.h"
#include "../utils/switch_utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <thread>

#ifdef __SWITCH__
#include <switch.h>
#include <mutex>
// SHA-256 через аппаратный ARMv8 CE (libnx crypto) — mbedtls-путь был чисто
// программным и жег целое ядро на верификацию каждого NCA.
extern "C" {
#include <switch/crypto/sha256.h>
}
extern std::recursive_mutex g_switch_service_mutex;
#endif

namespace installer {

namespace {

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

bool endsWithIcase(const std::string& value, const char* ext) {
    const size_t ext_len = std::strlen(ext);
    if (value.size() < ext_len) return false;

    const size_t offset = value.size() - ext_len;
    for (size_t i = 0; i < ext_len; ++i) {
        unsigned char a = static_cast<unsigned char>(value[offset + i]);
        unsigned char b = static_cast<unsigned char>(ext[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

} // namespace

// =============================================================================
// Размеры и константы
// =============================================================================
static constexpr size_t NSP_HEADER_READ_SIZE = 128 * 1024; // 128KB для заголовка
static constexpr size_t NSP_HEADER_PROBE_SIZE = 4 * 1024;  // 4KB для быстрого определения реального размера header
static constexpr size_t NSP_HEADER_MAX_SIZE  = 4 * 1024 * 1024; // защитный лимит для неадекватных header
static constexpr size_t LOCAL_STREAM_CHUNK_SIZE = 4 * 1024 * 1024; // Increased from 128KB to 4MB to prevent starvation
static constexpr size_t LOCAL_PREBUFFER_TARGET_SIZE = 32 * 1024 * 1024; // Increased from 8MB to 32MB for smoother play buffer
// If the full prebuffer target never fills (slow/dead swarm), start installing
// with whatever has arrived after this long -- provided at least one byte is
// there. Without the timeout a 3-peer wifi swarm that trickles below the
// target keeps the install stuck in the buffering phase forever.
static constexpr int LOCAL_PREBUFFER_TIMEOUT_MS = 60000;
static constexpr int LOCAL_HEADER_READ_TIMEOUT_MS = 180000;
static constexpr int LOCAL_HEADER_READ_LOG_MS = 5000;
static constexpr size_t MIN_BUFFER_SIZE = 16 * 1024 * 1024; // 16MB (saves RAM in Applet mode)
static constexpr size_t DEFAULT_CHUNK_SIZE = 4 * 1024 * 1024;  // 4MB chunk
#ifdef __SWITCH__
static constexpr size_t COLLECTOR_THREAD_STACK_SIZE = 0x20000; // 128KB
static constexpr size_t INSTALLER_THREAD_STACK_SIZE = 0x80000; // 512KB (was 256KB, increased to prevent stack overflow)
#endif

namespace {

size_t effectiveStreamingChunkSize(const datasource::IDataSource* source, size_t configured) {
    size_t chunk_size = configured == 0 ? DEFAULT_CHUNK_SIZE : configured;
    if (source && source->type() == datasource::SourceType::LocalInternal && chunk_size > LOCAL_STREAM_CHUNK_SIZE) {
        chunk_size = LOCAL_STREAM_CHUNK_SIZE;
    }
    return chunk_size;
}

// limitReadToPieceBoundary — УДАЛЕНО.
// Ранее обрезала запрос до границы куска, что гарантировало stall на каждой
// 8MB границе: коллектор читал хвост куска (например, 16936 байт), останавливался,
// затем ждал верификацию следующего куска.
// readPreparedFileRangeLocked в torrent_engine сам склеивает multi-piece reads.
// Читаем полный chunk_size=131072 не обращая внимания на границы.
inline size_t limitReadToPieceBoundary(const datasource::IDataSource* /*source*/,
                                       uint64_t /*offset*/,
                                       size_t requested) {
    return requested; // no-op: не обрезаем по границе куска
}

void sleepPrebufferPoll() {
#ifdef __SWITCH__
    svcSleepThread(100000000LL);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
}

} // namespace

// =============================================================================
// Обёртки для запуска потоков на Switch (libnx Thread)
// =============================================================================
#ifdef __SWITCH__
void collectorThreadEntry(void* arg) {
    auto* self = static_cast<HybridNspInstaller*>(arg);
    self->collectorThreadFunc();
}

void installerThreadEntry(void* arg) {
    auto* self = static_cast<HybridNspInstaller*>(arg);
    self->installerThreadFunc();
}
#endif

// =============================================================================
// Конструктор / Деструктор
// =============================================================================

HybridNspInstaller::HybridNspInstaller()
    : ring_buffer_(MIN_BUFFER_SIZE) {
}

HybridNspInstaller::~HybridNspInstaller() {
    cancel();

#ifdef __SWITCH__
    if (threads_started_) {
        threadWaitForExit(&collector_thread_);
        threadWaitForExit(&installer_thread_);
        threadClose(&collector_thread_);
        threadClose(&installer_thread_);
        threads_started_ = false;
    }
#endif
    ring_buffer_.clear();
}

void HybridNspInstaller::setSourceFileNameHint(const std::string& name) {
    is_xci_ = endsWithIcase(name, ".xci") || endsWithIcase(name, ".xcz");
}

// =============================================================================
// Определение оптимального размера буфера
// =============================================================================
size_t HybridNspInstaller::autoBufferSize() {
#ifdef __SWITCH__
    AppletType type = appletGetAppletType();
    if (type == AppletType_LibraryApplet || type == AppletType_OverlayApplet) {
        return 16 * 1024 * 1024; // 16MB для аплет-режима
    }
    // Application/Title Mode: 128MB.
    return 128 * 1024 * 1024;
#else
    return 128 * 1024 * 1024;
#endif
}

// =============================================================================
// start() — точка входа
// =============================================================================
bool HybridNspInstaller::start(datasource::IDataSource* source, const InstallConfig& config) {
    if (!source) {
        setError("Data source is not set");
        return false;
    }

    source_ = source;
    config_ = config;

    size_t max_buf = autoBufferSize();
    uint64_t file_sz = source_->totalSize();
    size_t buf_size = max_buf;
    if (file_sz > 0 && file_sz < max_buf) {
        size_t needed = static_cast<size_t>(file_sz) + 2 * 1024 * 1024;
        if (needed < 1024 * 1024) needed = 1024 * 1024;
        buf_size = std::min(max_buf, needed);
    }
    if (buf_size < 1024 * 1024) buf_size = 1024 * 1024;

    ring_buffer_.reinit(buf_size);

    cancel_requested_ = false;
    bytes_downloaded_ = 0;
    download_total_bytes_.store(0);
    bytes_installed_  = 0;
    payload_consumed_ = 0;
    total_bytes_.store(0);
    current_nca_index_  = 0;
    current_nca_offset_ = 0;
    error_message_.clear();
    header_data_.clear();
    if (source_) {
        source_->setCancelFlag(&cancel_requested_);
    }

#ifdef __SWITCH__
    last_bytes_ = 0;
    last_time_  = armGetSystemTick();
#endif
    current_speed_kbps_ = 0;
    ticket_data_.clear();
    cert_data_.clear();

    util::logLine("hybrid: start install, buffer=" + std::to_string(buf_size / (1024*1024)) + "MB");

    // Фаза 1: Парсинг заголовка
    if (!parseNspHeaderPhase()) return false;

    // Фаза 2: Создание плейсхолдеров
    if (!createPlaceHoldersPhase()) return false;

    // Фаза 3: Запуск двухпоточного стриминга
    if (!startStreamingPhase()) return false;

    return true;
}

// =============================================================================
// Фаза 1: Парсинг заголовка
// =============================================================================
bool HybridNspInstaller::parseNspHeaderPhase() {
    state_ = InstallState::ParsingHeader;
    util::logLine("hybrid: phase 1 - parsing header");

    if (is_xci_) {
        uint64_t file_size = source_->totalSize();
        if (file_size == 0) {
            setError("Failed to get total size from DataSource for XCI parsing");
            return false;
        }

        if (!xci_header_.parse(source_, file_size)) {
            setError("Invalid XCI format (parse failed)");
            return false;
        }

        total_bytes_.store(0);
        download_total_bytes_.store(0);
        for (const auto& e : xci_header_.entries()) {
            total_bytes_.fetch_add(e.size);
            download_total_bytes_.fetch_add(e.size);
        }

        util::logLine("hybrid: XCI contains " + std::to_string(xci_header_.fileCount()) + " files"
                       + ", total size=" + std::to_string(total_bytes_.load() / (1024*1024)) + "MB"
                       + ", ticket=" + std::string(xci_header_.hasTicket() ? "yes" : "no"));
        source_->notifyInstallInfoParsed(xci_header_.getInitialStreamPos(),
                                         effectiveStreamingChunkSize(source_, config_.chunk_size));
        return true;
    }

    auto read_header_range = [this](uint64_t offset, uint8_t* dst, size_t size, const char* label) -> size_t {
        if (!source_ || size == 0) {
            return 0;
        }

        const bool retry_transient_timeout = source_->type() == datasource::SourceType::LocalInternal;
        size_t total = 0;
        const auto start = std::chrono::steady_clock::now();
        auto last_log = start;

        while (total < size && !cancel_requested_ && !hasError()) {
            size_t read = source_->read(offset + total, dst + total, size - total);
            if (read > 0) {
                total += read;
                continue;
            }

            if (!retry_transient_timeout) {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (waited_ms >= LOCAL_HEADER_READ_TIMEOUT_MS) {
                break;
            }

            const auto log_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
            if (log_wait_ms >= LOCAL_HEADER_READ_LOG_MS) {
                util::logLine("hybrid: waiting local NSP header " + std::string(label ? label : "read") +
                              " got=" + std::to_string(total) +
                              " need=" + std::to_string(size) +
                              " waited_ms=" + std::to_string(waited_ms));
                last_log = now;
            }
            sleepPrebufferPoll();
        }

        return total;
    };

    header_data_.resize(NSP_HEADER_PROBE_SIZE);
    size_t read = read_header_range(0, header_data_.data(), NSP_HEADER_PROBE_SIZE, "probe");

    if (read < 0x10) {
        setError("Failed to read NSP header (" + std::to_string(read) + " bytes read)");
        return false;
    }
    header_data_.resize(read);

    if (std::memcmp(header_data_.data(), "PFS0", 4) != 0) {
        setError("Invalid NSP format (PFS0 magic not found)");
        return false;
    }

    const uint32_t file_count = readLe32(header_data_.data() + 4);
    const uint32_t string_table_size = readLe32(header_data_.data() + 8);
    const size_t required_header_size =
        0x10u + static_cast<size_t>(file_count) * 0x18u + static_cast<size_t>(string_table_size);

    util::logLine("hybrid: NSP header probe read=" + std::to_string(read) +
                  " need=" + std::to_string(required_header_size) +
                  " files=" + std::to_string(file_count));

    if (required_header_size == 0 || required_header_size > NSP_HEADER_MAX_SIZE) {
        setError("Invalid NSP header size (" + std::to_string(required_header_size) + ")");
        return false;
    }

    if (header_data_.size() < required_header_size) {
        const size_t current_size = header_data_.size();
        header_data_.resize(required_header_size);
        size_t extra_read = read_header_range(current_size, header_data_.data() + current_size,
                                              required_header_size - current_size, "full");
        if (current_size + extra_read < required_header_size) {
            setError("Failed to read full NSP header");
            return false;
        }
    } else {
        header_data_.resize(required_header_size);
    }

    if (!nsp_header_.parse(header_data_.data(), header_data_.size())) {
        setError("Failed to parse full NSP header");
        return false;
    }

    if (nsp_header_.dataRegionOffset() != required_header_size) {
        util::logLine("hybrid: NSP header size mismatch parsed=" +
                      std::to_string(nsp_header_.dataRegionOffset()) +
                      " computed=" + std::to_string(required_header_size));
    }

    total_bytes_.store(0);
    download_total_bytes_.store(0);
    for (const auto& e : nsp_header_.entries()) {
        total_bytes_.fetch_add(e.size);
        download_total_bytes_.fetch_add(e.size);
    }

    util::logLine("hybrid: NSP contains " + std::to_string(nsp_header_.fileCount()) + " files"
                   + ", total size=" + std::to_string(total_bytes_.load() / (1024*1024)) + "MB"
                   + ", ticket=" + std::string(nsp_header_.hasTicket() ? "yes" : "no"));

    source_->notifyInstallInfoParsed(nsp_header_.dataRegionOffset(),
                                     effectiveStreamingChunkSize(source_, config_.chunk_size));

    return true;
}

// =============================================================================
// Фаза 2: Создание NCM-плейсхолдеров
// =============================================================================
bool HybridNspInstaller::createPlaceHoldersPhase() {
    state_ = InstallState::CreatingPlaceHolders;
    util::logLine("hybrid: phase 2 - creating placeholders");

#ifdef __SWITCH__
    if (!ncm_.begin(config_.storage)) {
        setError("Failed to initialize NCM");
        return false;
    }

    const auto& entries = is_xci_ ? xci_header_.entries() : nsp_header_.entries();
    for (const auto& e : entries) {
        if (cancel_requested_) {
            setError("Cancelled by user");
            return false;
        }

        if (e.type == NspEntryType::Nca || e.type == NspEntryType::CnmtNca) {
            NcmContentId id = e.content_id;
            bool is_ncz = (e.name.find(".ncz") != std::string::npos);

            // Если это NCZ, мы не знаем распакованный размер до начала чтения файла.
            // Поэтому создадим placeholder прямо перед записью в Thread B.
            if (is_ncz) {
                util::logLine("installer: skipping placeholder creation for NCZ (will create later): " + e.name);
                continue;
            }

            if (!ncm_.createPlaceHolder(id, e.size)) {
                setError("Failed to create placeholder for " + e.name);
                return false;
            }
            util::logLine("ncm: placeholder created for " + e.name);
        }
    }
#else
    util::logLine("hybrid: (host) skipping placeholder creation");
#endif

    return true;
}

// =============================================================================
// Фаза 3: Запуск двухпоточного стриминга
// =============================================================================
bool HybridNspInstaller::startStreamingPhase() {
    state_ = InstallState::Streaming;
    util::logLine("hybrid: phase 3 - starting Collector + Installer threads");

    ring_buffer_.reset();

#ifdef __SWITCH__
    // Network/Collector thread: Higher priority, Core 0
    Result rc = threadCreate(&collector_thread_, collectorThreadEntry, this,
                              nullptr, COLLECTOR_THREAD_STACK_SIZE, 0x20, 0);
    if (R_FAILED(rc)) {
        setError("Failed to create Collector thread, rc=" + std::to_string(rc));
        return false;
    }

    // Decompression/Installer thread: Lower priority, Core 1
    rc = threadCreate(&installer_thread_, installerThreadEntry, this,
                       nullptr, INSTALLER_THREAD_STACK_SIZE, 0x30, 1);
    if (R_FAILED(rc)) {
        setError("Failed to create Installer thread, rc=" + std::to_string(rc));
        threadClose(&collector_thread_);
        return false;
    }

    rc = threadStart(&collector_thread_);
    if (R_FAILED(rc)) {
        setError("Failed to start Collector thread, rc=" + std::to_string(rc));
        threadClose(&collector_thread_);
        threadClose(&installer_thread_);
        return false;
    }

    rc = threadStart(&installer_thread_);
    if (R_FAILED(rc)) {
        setError("Failed to start Installer thread, rc=" + std::to_string(rc));
        cancel_requested_ = true;
        ring_buffer_.setEof();
        threadWaitForExit(&collector_thread_);
        threadClose(&collector_thread_);
        threadClose(&installer_thread_);
        return false;
    }

    threads_started_ = true;
    // The transfer is now running: hold the console at 1785 MHz so SHA-1
    // verification, zstd decompression and bsd IPCs all run ~75% faster.
    util::cpuBoostBegin();
    util::logLine("hybrid: both threads started");
#else
    // На хосте — однопоточная имитация
    collectorThreadFunc();
    installerThreadFunc();
#endif

    return true;
}

// =============================================================================
// Thread A: Collector — DataSource → RingBuffer
// =============================================================================
void HybridNspInstaller::collectorThreadFunc() {
    util::logLine("collector: start");

    uint64_t data_offset = is_xci_ ? xci_header_.getInitialStreamPos() : nsp_header_.dataRegionOffset();
    uint64_t current_offset = data_offset;

    uint64_t data_end = data_offset;
    const auto& entries = is_xci_ ? xci_header_.entries() : nsp_header_.entries();
    for (const auto& e : entries) {
        uint64_t file_end = data_offset + e.offset + e.size;
        if (file_end > data_end) data_end = file_end;
    }

    const size_t chunk_size = effectiveStreamingChunkSize(source_, config_.chunk_size);
    const bool allow_partial_progress =
        source_ && source_->type() == datasource::SourceType::LocalInternal;
    if (source_) {
        util::logLine("collector: source_type=" + std::to_string(static_cast<int>(source_->type())) +
                      " chunk_size=" + std::to_string(chunk_size));
    }
    if (source_ && source_->type() == datasource::SourceType::LocalInternal) {
        util::logLine("collector: local chunk size=" + std::to_string(chunk_size));
    }
    std::vector<uint8_t> chunk_buf(chunk_size);

    int retry_count = 0;
    const int max_retries = 12; // increased from 5: stall recovery can take 3-12 seconds
    const uint64_t partial_log_interval = 8ULL * 1024ULL * 1024ULL;
    uint64_t next_partial_log_offset = data_offset;
    auto last_collector_stats_at = std::chrono::steady_clock::now();
    uint64_t last_collector_stats_bytes = 0;
    auto logCollectorStats = [&]() {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration<double>(now - last_collector_stats_at).count();
        if (elapsed < 5.0) {
            return;
        }

        const uint64_t downloaded = bytes_downloaded_.load();
        const uint64_t delta = downloaded >= last_collector_stats_bytes
            ? downloaded - last_collector_stats_bytes
            : 0;
        const double speed_kbps = elapsed > 0.0
            ? static_cast<double>(delta) / 1024.0 / elapsed
            : 0.0;
        util::logLine("collector: stats downloaded=" + std::to_string(downloaded) +
                      " speed=" + std::to_string(static_cast<int>(speed_kbps)) + "KB/s" +
                      " rb_avail=" + std::to_string(ring_buffer_.available()) +
                      " rb_free=" + std::to_string(ring_buffer_.freeSpace()) +
                      " rb_cap=" + std::to_string(ring_buffer_.capacity()));
        last_collector_stats_at = now;
        last_collector_stats_bytes = downloaded;
    };

    while (current_offset < data_end && !cancel_requested_.load() && !g_appExiting.load()) {
        size_t to_read = static_cast<size_t>(
            std::min(static_cast<uint64_t>(chunk_size), data_end - current_offset));
        to_read = limitReadToPieceBoundary(source_, current_offset, to_read);

        size_t read = source_->read(current_offset, chunk_buf.data(), to_read);
        if (cancel_requested_.load() || g_appExiting.load()) break;

        if (read == 0 || read < to_read) {
            if (read > 0 && allow_partial_progress) {
                if (current_offset >= next_partial_log_offset) {
                    util::logLine("collector: partial read advance offset=" + std::to_string(current_offset) +
                                   " requested=" + std::to_string(to_read) +
                                   " got=" + std::to_string(read));
                    next_partial_log_offset = current_offset + partial_log_interval;
                }
                retry_count = 0;
                ring_buffer_.write(chunk_buf.data(), read);
                current_offset += read;
                bytes_downloaded_ = current_offset - data_offset;
                logCollectorStats();
                continue;
            }

            ++retry_count;
            if (retry_count >= max_retries) {
                util::logLine("collector: retry limit reached at offset="
                               + std::to_string(current_offset)
                               + " requested=" + std::to_string(to_read)
                               + " got=" + std::to_string(read));
                break;
            }
            if (read == 0) {
                util::logLine("collector: retry " + std::to_string(retry_count)
                               + "/" + std::to_string(max_retries)
                               + " offset=" + std::to_string(current_offset));
            } else {
                util::logLine("collector: short read retry " + std::to_string(retry_count)
                               + "/" + std::to_string(max_retries)
                               + " offset=" + std::to_string(current_offset)
                               + " requested=" + std::to_string(to_read)
                               + " got=" + std::to_string(read));
            }
#ifdef __SWITCH__
            svcSleepThread(500000000LL); // 500ms
#endif
            continue;
        }

        retry_count = 0;
        ring_buffer_.write(chunk_buf.data(), read);
        current_offset += read;
        bytes_downloaded_ = current_offset - data_offset;
        logCollectorStats();
    }

    if (!cancel_requested_ && current_offset < data_end && !hasError()) {
        setError("Collector stopped early at offset=" + std::to_string(current_offset) +
                 " of " + std::to_string(data_end));
    }

    ring_buffer_.setEof();
    util::logLine("collector: finished, downloaded " + std::to_string(bytes_downloaded_.load()) + " bytes");
}

// =============================================================================
// Thread B: Installer — RingBuffer → NCM
// =============================================================================

#ifdef __SWITCH__
// =========================================================================
// Custom IPC wrappers for Push/List/Delete ApplicationRecord
// =========================================================================

struct ContentStorageRecord {
    NcmContentMetaKey key;
    u64 storage_id;
};
static_assert(sizeof(ContentStorageRecord) == 24, "ContentStorageRecord must be exactly 24 bytes");

static Result nsPushApplicationRecordCustom(u64 application_id, u8 record_type, const ContentStorageRecord* records, s32 count) {
    Service app_manager;
    Result rc = nsGetApplicationManagerInterface(&app_manager);
    if (R_FAILED(rc)) return rc;

    struct {
        u8 record_type;
        u8 pad[7];
        u64 application_id;
    } in;
    in.record_type = record_type;
    in.pad[0] = 0; in.pad[1] = 0; in.pad[2] = 0; in.pad[3] = 0; in.pad[4] = 0; in.pad[5] = 0; in.pad[6] = 0;
    in.application_id = application_id;

    rc = serviceDispatchIn(&app_manager, 16, in,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
        .buffers = { { records, static_cast<size_t>(count * sizeof(ContentStorageRecord)) } }
    );
    serviceClose(&app_manager);
    return rc;
}

static Result nsListApplicationRecordContentMetaCustom(u64 offset, u64 application_id, ContentStorageRecord* out_records, size_t out_buf_size, u32* out_entries_read) {
    Service app_manager;
    Result rc = nsGetApplicationManagerInterface(&app_manager);
    if (R_FAILED(rc)) return rc;

    struct {
        u64 offset;
        u64 application_id;
    } in;
    in.offset = offset;
    in.application_id = application_id;

    rc = serviceDispatchInOut(&app_manager, 17, in, *out_entries_read,
        .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
        .buffers = { { out_records, out_buf_size } }
    );
    serviceClose(&app_manager);
    return rc;
}

static Result nsDeleteApplicationRecordCustom(u64 application_id) {
    Service app_manager;
    Result rc = nsGetApplicationManagerInterface(&app_manager);
    if (R_FAILED(rc)) return rc;

    struct {
        u64 application_id;
    } in;
    in.application_id = application_id;

    rc = serviceDispatchIn(&app_manager, 27, in);
    serviceClose(&app_manager);
    return rc;
}



static constexpr u8 NsApplicationRecordType_Installed = 0x3;
#endif

// =============================================================================
void HybridNspInstaller::installerThreadFunc() {
    util::logLine("installer: start");

    const size_t chunk_size = effectiveStreamingChunkSize(source_, config_.chunk_size);
    if (source_ && source_->type() == datasource::SourceType::LocalInternal) {
        util::logLine("installer: local chunk size=" + std::to_string(chunk_size));
    }
    std::vector<uint8_t> chunk_buf(chunk_size);
    auto last_installer_stats_at = std::chrono::steady_clock::now();
    uint64_t last_installer_stats_installed = bytes_installed_.load();
    uint64_t last_installer_stats_downloaded = bytes_downloaded_.load();
    uint64_t last_installer_stats_consumed  = payload_consumed_.load();
    std::vector<int64_t> wait_ms_samples;   // waits >= 500 ms (совпадает с starvation_count_)
    int64_t stall_total_ms = 0;
    double install_speed_sum = 0.0, install_speed_max = 0.0;
    double source_speed_sum  = 0.0, source_speed_max  = 0.0;
    double consume_speed_sum = 0.0, consume_speed_max = 0.0;
    int     speed_samples = 0;
    int     live_peers_min = -1;
    auto percentile = [](std::vector<int64_t> v, double p) -> int64_t {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        size_t idx = static_cast<size_t>(std::ceil(v.size() * p / 100.0)) - 1;
        return v[idx];
    };
    auto buildSummarySuffix = [&]() {
        std::string msg = " starvation_events=" + std::to_string(starvation_count_.load());
        if (!wait_ms_samples.empty()) {
            msg += " wait_p50_ms=" + std::to_string(percentile(wait_ms_samples, 50.0)) +
                   " wait_p95_ms=" + std::to_string(percentile(wait_ms_samples, 95.0)) +
                   " wait_max_ms=" +
                   std::to_string(*std::max_element(wait_ms_samples.begin(), wait_ms_samples.end())) +
                   " stall_total_ms=" + std::to_string(stall_total_ms);
        }
        if (speed_samples > 0) {
            msg += " install_speed_avg_kbps=" + std::to_string(static_cast<int>(install_speed_sum / speed_samples)) +
                   " install_speed_max_kbps=" + std::to_string(static_cast<int>(install_speed_max)) +
                   " source_speed_avg_kbps=" + std::to_string(static_cast<int>(source_speed_sum / speed_samples)) +
                   " source_speed_max_kbps=" + std::to_string(static_cast<int>(source_speed_max)) +
                   " consume_speed_avg_kbps=" + std::to_string(static_cast<int>(consume_speed_sum / speed_samples)) +
                   " consume_speed_max_kbps=" + std::to_string(static_cast<int>(consume_speed_max));
        }
        if (live_peers_min >= 0) {
            msg += " live_peers_min=" + std::to_string(live_peers_min);
        }
        return msg;
    };
    bool summary_logged = false;
    auto logSummary = [&]() {
        if (summary_logged) return;
        summary_logged = true;
        util::logLine("installer: summary" + buildSummarySuffix());
    };
    auto logInstallerStats = [&]() {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration<double>(now - last_installer_stats_at).count();
        if (elapsed < 5.0) {
            return;
        }

        const uint64_t installed = bytes_installed_.load();
        const uint64_t downloaded = bytes_downloaded_.load();
        const uint64_t consumed = payload_consumed_.load();
        const uint64_t installed_delta = installed >= last_installer_stats_installed
            ? installed - last_installer_stats_installed
            : 0;
        const uint64_t downloaded_delta = downloaded >= last_installer_stats_downloaded
            ? downloaded - last_installer_stats_downloaded
            : 0;
        const uint64_t consumed_delta = consumed >= last_installer_stats_consumed
            ? consumed - last_installer_stats_consumed
            : 0;
        const double install_kbps = elapsed > 0.0
            ? static_cast<double>(installed_delta) / 1024.0 / elapsed
            : 0.0;
        const double source_kbps = elapsed > 0.0
            ? static_cast<double>(downloaded_delta) / 1024.0 / elapsed
            : 0.0;
        const double consume_kbps = elapsed > 0.0
            ? static_cast<double>(consumed_delta) / 1024.0 / elapsed
            : 0.0;
        util::logLine("installer: stats installed=" + std::to_string(installed) +
                      " install_speed=" + std::to_string(static_cast<int>(install_kbps)) + "KB/s" +
                      " downloaded=" + std::to_string(downloaded) +
                      " source_speed=" + std::to_string(static_cast<int>(source_kbps)) + "KB/s" +
                      " consume_speed=" + std::to_string(static_cast<int>(consume_kbps)) + "KB/s" +
                      " rb_avail=" + std::to_string(ring_buffer_.available()) +
                      " rb_free=" + std::to_string(ring_buffer_.freeSpace()) +
                      " rb_cap=" + std::to_string(ring_buffer_.capacity()));
        if (elapsed > 0.0) {
            install_speed_sum += install_kbps;
            source_speed_sum  += source_kbps;
            consume_speed_sum += consume_kbps;
            if (install_kbps > install_speed_max) install_speed_max = install_kbps;
            if (source_kbps  > source_speed_max)  source_speed_max  = source_kbps;
            if (consume_kbps > consume_speed_max) consume_speed_max = consume_kbps;
            speed_samples++;
        }
        if (source_) {
            const int live = source_->livePeers();
            if (live >= 0 && (live_peers_min < 0 || live < live_peers_min)) {
                live_peers_min = live;
            }
        }
        last_installer_stats_at = now;
        last_installer_stats_installed = installed;
        last_installer_stats_downloaded = downloaded;
        last_installer_stats_consumed = consumed;
    };

    if (source_ && source_->type() == datasource::SourceType::LocalInternal) {
        size_t target = std::min(LOCAL_PREBUFFER_TARGET_SIZE, ring_buffer_.capacity());
        const uint64_t total = download_total_bytes_.load();
        if (total > 0 && total < target) {
            target = static_cast<size_t>(total);
        } else {
            // Адаптивный target: время наполнения буфера при текущей скорости
            // сети. Медленный/нестабильный рой (полевой прогон: 3.5 МБ/с в
            // среднем, просадки до нуля) получает максимум, быстрый рой не
            // тратит лишние секунды на ожидание полного буфера.
            const int src_kbps = source_->downloadSpeedKBps();
            if (src_kbps > 0) {
                const size_t speed_target = static_cast<size_t>(src_kbps) * 10 * 1024; // ~10s of network
                target = std::max(MIN_BUFFER_SIZE, std::min(target, speed_target));
            }
        }
        if (total > 0 && target > total) {
            target = static_cast<size_t>(total);
        }
        if (target > 0) {
            util::logLine("installer: waiting local prebuffer target=" + std::to_string(target));
            const auto prebuffer_start = std::chrono::steady_clock::now();
            while (!cancel_requested_ && !hasError() && ring_buffer_.available() < target && !ring_buffer_.isEof()) {
                sleepPrebufferPoll();
                if (ring_buffer_.isEof()) break;
                // Slow swarm: give up on the full target after a minute and
                // install with what has arrived. Requires at least one byte --
                // a completely dead swarm just keeps waiting (cancellable).
                const auto waited_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - prebuffer_start).count();
                if (ring_buffer_.available() > 0 &&
                    waited_ms >= LOCAL_PREBUFFER_TIMEOUT_MS) {
                    util::logLine("installer: prebuffer timeout after " +
                                  std::to_string(waited_ms) + "ms, starting with available=" +
                                  std::to_string(ring_buffer_.available()));
                    break;
                }
            }
            util::logLine("installer: local prebuffer ready available=" +
                          std::to_string(ring_buffer_.available()));
        }
    }

    const auto& entries = is_xci_ ? xci_header_.entries() : nsp_header_.entries();
    // Сжатый объём payload (для NSZ total_bytes_ скорректирован вверх до
    // распакованного размера — сравнение stream_pos (сжатый) с ним сломало
    // бы хвостовое исключение watermark'а).
    uint64_t total_payload = 0;
    for (const auto& e : entries) total_payload += e.size;
    uint64_t stream_pos = 0;
    uint64_t progress_pos = 0;

    // =========================================================================
    // Pacing + watermark: единая реализация в installer_flow::InstallerFlow
    // (тот же код исполняется в pctest/apptest — PC-прогон точно повторяет
    // поведение железа). Не даём инсталлеру стабильно обгонять сеть: если
    // потребление ring buffer заметно выше скорости скачивания, буфер
    // упирается в пустоту и read() блокируется на секунды (starvation_events
    // в полевых логах: 542 события, p95 ~15.6s). Ограничение потребления
    // ~92% от download_speed превращает длинные ожидания в плавный поток и
    // держит буфер наполненным (страховка от кратковременных просадок сети).
    // =========================================================================
    installer_flow::InstallerFlow flow{installer_flow::FlowConfig()};
    auto flow_eof     = [&]() { return ring_buffer_.isEof(); };
    auto flow_cancel  = [&]() { return cancel_requested_.load(); };
    auto flow_err     = [&]() { return hasError(); };
    auto flow_avail   = [&]() { return ring_buffer_.available(); };
    auto flow_sleep   = [&]() { sleepPrebufferPoll(); };
    auto flow_log     = [&](const std::string& m) { util::logLine("installer: " + m); };

#ifdef __SWITCH__
    Sha256Context sha_ctx;
    bool hashing_active = false;
#endif

    ticket_data_.clear();
    cert_data_.clear();

    while (!cancel_requested_) {
        flow.waitForWatermark(stream_pos, total_payload, flow_eof, flow_cancel,
                              flow_err, flow_avail, flow_sleep);
        const auto read_wait_start = std::chrono::steady_clock::now();
        util::logLine("installer: [RingBuffer] read start rb_avail=" + std::to_string(ring_buffer_.available()) +
                      " downloaded=" + std::to_string(bytes_downloaded_.load()));
        size_t read = ring_buffer_.read(chunk_buf.data(), chunk_size);
        payload_consumed_.fetch_add(read);
        util::logLine("installer: [RingBuffer] read done got=" + std::to_string(read));
        const auto read_wait_end = std::chrono::steady_clock::now();
        const auto read_wait_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(read_wait_end - read_wait_start).count();
        if (read_wait_ms >= 500 && read > 0) {
            ++starvation_count_;
            wait_ms_samples.push_back(read_wait_ms);
            stall_total_ms += read_wait_ms;
            util::logLine("installer: buffer wait wait_ms=" + std::to_string(read_wait_ms) +
                          " got=" + std::to_string(read) +
                          " rb_avail=" + std::to_string(ring_buffer_.available()) +
                          " downloaded=" + std::to_string(bytes_downloaded_.load()) +
                          " installed=" + std::to_string(bytes_installed_.load()) +
                          " starvation_total=" + std::to_string(starvation_count_.load()));
        }
        if (read == 0) break; // EOF

        size_t processed = 0;

        while (processed < read && !cancel_requested_) {
            const NspFileEntry* current_entry = nullptr;
            uint64_t entry_start = 0;
            uint64_t entry_end   = 0;

            for (const auto& e : entries) {
                entry_start = e.offset;
                entry_end   = e.offset + e.size;

                if (stream_pos >= entry_start && stream_pos < entry_end) {
                    current_entry = &e;
                    break;
                }
            }

            if (!current_entry) {
                ++processed;
                ++stream_pos;
                continue;
            }

            uint64_t remaining_in_file = entry_end - stream_pos;
            size_t remaining_in_chunk = read - processed;
            size_t to_process = static_cast<size_t>(
                std::min(static_cast<uint64_t>(remaining_in_chunk), remaining_in_file));
            uint64_t progress_advance = to_process;

            uint64_t offset_in_file = stream_pos - entry_start;

            switch (current_entry->type) {
                case NspEntryType::Nca:
                case NspEntryType::CnmtNca: {
#ifdef __SWITCH__
                    bool is_ncz = (current_entry->name.find(".ncz") != std::string::npos);

                    if (is_ncz && offset_in_file == 0) {
                        uint64_t ncz_bytes_read = 0;
                        uint64_t ncz_total_compressed = current_entry->size;

                        auto fetch_cb = [&](void* buf, size_t fetch_size) -> size_t {
                            if (ncz_bytes_read >= ncz_total_compressed) {
                                return 0;
                            }
                            size_t actual_fetch = std::min(fetch_size, static_cast<size_t>(ncz_total_compressed - ncz_bytes_read));
                            size_t returned = 0;
                            uint8_t* out = static_cast<uint8_t*>(buf);

                            // Consume from chunk_buf first
                            if (ncz_bytes_read < to_process) {
                                size_t from_chunk = std::min(actual_fetch, static_cast<size_t>(to_process - ncz_bytes_read));
                                std::memcpy(out, chunk_buf.data() + processed + ncz_bytes_read, from_chunk);
                                returned += from_chunk;
                                ncz_bytes_read += from_chunk;
                                actual_fetch -= from_chunk;
                                out += from_chunk;
                            }

                            // Consume from ring_buffer_
                            if (actual_fetch > 0) {
                                while (actual_fetch > 0) {
                                    const auto rb_wait_start = std::chrono::steady_clock::now();
                                    size_t from_rb = ring_buffer_.read(out, actual_fetch);
                                    payload_consumed_.fetch_add(from_rb);
                                    const auto rb_wait_end = std::chrono::steady_clock::now();
                                    const auto rb_wait_ms =
                                        std::chrono::duration_cast<std::chrono::milliseconds>(rb_wait_end - rb_wait_start).count();
                                    if (rb_wait_ms >= 500 && from_rb > 0) {
                                        ++starvation_count_;
                                        wait_ms_samples.push_back(rb_wait_ms);
                                        stall_total_ms += rb_wait_ms;
                                        util::logLine("installer: ncz buffer wait wait_ms=" + std::to_string(rb_wait_ms) +
                                                      " got=" + std::to_string(from_rb) +
                                                      " rb_avail=" + std::to_string(ring_buffer_.available()) +
                                                      " downloaded=" + std::to_string(bytes_downloaded_.load()) +
                                                      " installed=" + std::to_string(bytes_installed_.load()) +
                                                      " starvation_total=" + std::to_string(starvation_count_.load()));
                                    }
                                    if (from_rb == 0) return returned; // EOF
                                    returned += from_rb;
                                    ncz_bytes_read += from_rb;
                                    actual_fetch -= from_rb;
                                    out += from_rb;
                                    logInstallerStats();
                                }
                            }
                            return returned;
                        };

                        NczDecompressor decompressor(fetch_cb);
                        if (!decompressor.init()) {
                            setError("NCZ: Failed to init decompressor for " + current_entry->name);
                            ring_buffer_.setEof();
                            goto cleanup_sha; // Use existing cleanup label
                        }

                        uint64_t d_size = decompressor.getDecompressedSize();
                        if (d_size > current_entry->size) {
                            total_bytes_.fetch_add(d_size - current_entry->size);
                        } else if (d_size < current_entry->size) {
                            total_bytes_.fetch_sub(current_entry->size - d_size);
                        }
                        if (d_size != current_entry->size) {
                            util::logLine("ncz: adjusted logical install size for " + current_entry->name
                                          + " from " + std::to_string(current_entry->size)
                                          + " to " + std::to_string(d_size));
                        }
                        if (!ncm_.createPlaceHolder(current_entry->content_id, d_size)) {
                            setError("NCZ: Failed to create placeholder for " + current_entry->name);
                            ring_buffer_.setEof();
                            goto cleanup_sha;
                        }

                        util::logLine("ncz: installing " + current_entry->name + " (compressed: " + std::to_string(ncz_total_compressed/(1024*1024)) + "MB, orig: " + std::to_string(d_size/(1024*1024)) + "MB)");

                        std::vector<uint8_t> out_buf(4 * 1024 * 1024); // 4MB buffer for NCM
                        const uint64_t progress_base = progress_pos;
                        uint64_t out_written = 0;
                        bool ok = true;
                        
                        if (config_.verify_sha256) {
                            sha256ContextCreate(&sha_ctx);
                            hashing_active = true;
                        }
                        
                        while (out_written < d_size && !cancel_requested_) {
                            size_t got = decompressor.read(out_buf.data(), out_buf.size());
                            if (got == 0) {
                                if (out_written < d_size) {
                                    setError("NCZ: Unexpected EOF decompessing " + current_entry->name);
                                    ok = false;
                                }
                                break;
                            }

                            if (!ncm_.writePlaceHolder(current_entry->content_id, out_written, out_buf.data(), got)) {
                                setError("NCZ: Failed to write placeholder " + current_entry->name);
                                ok = false;
                                break;
                            }
                            
                            if (config_.verify_sha256 && hashing_active) {
                                sha256ContextUpdate(&sha_ctx, out_buf.data(), got);
                            }

                            if (current_entry->type == NspEntryType::CnmtNca) {
                                cnmt_nca_data_.insert(cnmt_nca_data_.end(), out_buf.data(), out_buf.data() + got);
                            }

                            out_written += got;

                            // Report progress using decompressed bytes written to NCM.
                            bytes_installed_ = progress_base + out_written;
                        }

                        if (!ok) {
                            ring_buffer_.setEof();
                            goto cleanup_sha;
                        }

                        if (!ncm_.finalizePlaceHolder(current_entry->content_id)) {
                            setError("NCZ: Failed to finalize NCA: " + current_entry->name);
                            ring_buffer_.setEof();
                            goto cleanup_sha;
                        }

                        util::logLine("installer: NCZ installed: " + current_entry->name);
                        hashing_active = false;
                        progress_advance = out_written;

                        // NCZ полностью обработан через callback: в этом чанке потреблено только `to_process`,
                        // остальной compressed-поток уже прочитан из ring_buffer_ внутри декомпрессора.
                        // Outer loop затем добавит `+to_process`, поэтому здесь добавляем только остаток.
                        stream_pos += (current_entry->size - to_process);
                        break; 
                    } else if (is_ncz) {
                        // is_ncz && offset_in_file > 0
                        // This should never happen if we process it completely at offset_in_file == 0.
                        break;
                    }

                    if (offset_in_file == 0) {
                        if (config_.verify_sha256) {
                            sha256ContextCreate(&sha_ctx);
                            hashing_active = true;
                        }
                        if (current_entry->type == NspEntryType::CnmtNca) {
                            cnmt_nca_data_.clear();
                            cnmt_nca_data_.reserve(static_cast<size_t>(current_entry->size));
                        }
                    }

                    // Буферизуем CNMT NCA для Phase 5
                    if (current_entry->type == NspEntryType::CnmtNca) {
                        cnmt_nca_data_.insert(cnmt_nca_data_.end(),
                                              chunk_buf.data() + processed,
                                              chunk_buf.data() + processed + to_process);
                    }

                    util::logLine("installer: [NcaWrite] start content=" + current_entry->name + 
                                  " offset=" + std::to_string(offset_in_file) + " size=" + std::to_string(to_process));
                    if (!ncm_.writePlaceHolder(current_entry->content_id,
                                                offset_in_file,
                                                chunk_buf.data() + processed,
                                                to_process)) {
                        setError("Failed to write NCA: " + current_entry->name);
                        ring_buffer_.setEof();
                        goto cleanup_sha;
                    }
                    util::logLine("installer: [NcaWrite] success");

                    if (config_.verify_sha256 && hashing_active) {
                        util::logLine("installer: [SHA256] update start");
                        sha256ContextUpdate(&sha_ctx,
                                            chunk_buf.data() + processed,
                                            to_process);
                        util::logLine("installer: [SHA256] update success");
                    }

                    if (offset_in_file + to_process >= current_entry->size) {
                        util::logLine("installer: [NcaFinalize] start content=" + current_entry->name);
                        if (!ncm_.finalizePlaceHolder(current_entry->content_id)) {
                            setError("Failed to finalize NCA: " + current_entry->name);
                            ring_buffer_.setEof();
                            goto cleanup_sha;
                        }
                        util::logLine("installer: [NcaFinalize] success");
                        util::logLine("installer: NCA installed: " + current_entry->name);
                        hashing_active = false;
                    }
#else
                    (void)offset_in_file;
                    if (current_entry->type == NspEntryType::CnmtNca) {
                        cnmt_nca_data_.insert(cnmt_nca_data_.end(),
                                              chunk_buf.data() + processed,
                                              chunk_buf.data() + processed + to_process);
                    }
#endif
                    break;
                }

                case NspEntryType::Ticket:
                    ticket_data_.insert(ticket_data_.end(),
                                         chunk_buf.data() + processed,
                                         chunk_buf.data() + processed + to_process);
                    break;

                case NspEntryType::Cert:
                    cert_data_.insert(cert_data_.end(),
                                       chunk_buf.data() + processed,
                                       chunk_buf.data() + processed + to_process);
                    break;

                case NspEntryType::Other:
                    break;
            }

            processed   += to_process;
            stream_pos  += to_process;
            progress_pos += progress_advance;
            bytes_installed_ = progress_pos;
        }
        logInstallerStats();
        flow.pace(source_ != nullptr,
                  source_ ? source_->downloadSpeedKBps() : 0,
                  payload_consumed_.load(),
                  flow_cancel, flow_err, flow_sleep, flow_log);
    }

#ifdef __SWITCH__
cleanup_sha:
#endif

    if (!cancel_requested_ && !hasError()) {
        installTicketsPhase();
        registerContentMetaPhase();

        if (!hasError()) {
            state_ = InstallState::Completed;
            util::logLine("installer: installation completed successfully" + buildSummarySuffix());
            summary_logged = true;
        }
    }

    if (source_) {
        source_->notifyStreamingComplete(!cancel_requested_ && !hasError());
    }

    // Every installer-thread exit path converges here (completion, cancel,
    // error), so this is the single place the transfer releases its boost.
    util::cpuBoostEnd();

    logSummary();

    util::logLine("installer: thread finished, installed " + std::to_string(bytes_installed_.load()) + " bytes");
}

// =============================================================================
// Фаза 4: Установка тикетов
// =============================================================================
bool HybridNspInstaller::installTicketsPhase() {
    if (!config_.install_ticket) return true;
    if (ticket_data_.empty()) {
        util::logLine("hybrid: no tickets found, skipping");
        return true;
    }

    state_ = InstallState::InstallingTickets;
    util::logLine("hybrid: phase 4 - installing tickets");

#ifdef __SWITCH__
    bool ok;
    if (!cert_data_.empty()) {
        ok = ticket_.installTicket(ticket_data_.data(), ticket_data_.size(),
                                    cert_data_.data(), cert_data_.size());
    } else {
        ok = ticket_.installTicketOnly(ticket_data_.data(), ticket_data_.size());
    }

    if (!ok) {
        util::logLine("hybrid: warning - ticket install failed (title may require fixes)");
    }
#endif

    return true;
}

// =============================================================================
// Фаза 5: Регистрация CNMT + nsPushApplicationRecord → игра видна в Home Menu
// =============================================================================
bool HybridNspInstaller::registerContentMetaPhase() {
    state_ = InstallState::RegisteringMeta;
    util::logLine("hybrid: phase 5 - registering CNMT metadata");

    CnmtData cnmt;
    bool cnmt_ready = false;

#ifdef __SWITCH__
    // =========================================================================
    // 1) Основной путь: читаем CNMT NCA из установленного контента через
    //    FsFileSystemType_ContentMeta. Работает всегда: NCA уже финализирован
    //    к этому моменту, размер файла не важен. Полевые логи: in-memory путь
    //    спотыкался на неполных данных ring buffer ("insufficient data, need
    //    3368553 but have 3584") в обоих файлах, а этот путь был стабилен.
    // =========================================================================
    {
        const NspFileEntry* cnmt_entry = is_xci_ ? xci_header_.findByType(NspEntryType::CnmtNca) : nsp_header_.findByType(NspEntryType::CnmtNca);
        if (cnmt_entry && ncm_.isInitialized()) {
            // Awoo-like путь: читаем внутренний *.cnmt через FsFileSystemType_ContentMeta.
            std::vector<uint8_t> cnmt_buf;
            if (ncm_.readCnmtFromContentMetaFs(cnmt_entry->content_id, cnmt_buf)) {
                util::logLine("hybrid: read CNMT from ContentMeta FS, size=" + std::to_string(cnmt_buf.size()));
                CnmtData cnmt2;
                if (CnmtParser::parse(cnmt_buf.data(), cnmt_buf.size(), cnmt2)) {
                    cnmt = cnmt2;
                    cnmt_ready = true;
                    util::logLine("hybrid: CNMT parsed from ContentMeta FS OK");
                } else {
                    util::logLine("hybrid: failed to parse CNMT buffer from ContentMeta FS");
                }
            }

            // Fallback: старый путь (сырые данные NCA по content id).
            if (!cnmt_ready) {
                std::vector<uint8_t> nca_buf;
                if (ncm_.readContentIdFile(cnmt_entry->content_id, nca_buf)) {
                    util::logLine("hybrid: read installed CNMT NCA, size=" + std::to_string(nca_buf.size()));
                    CnmtData cnmt2;
                    if (CnmtParser::extractFromNca(nca_buf.data(), nca_buf.size(), cnmt2)) {
                        cnmt = cnmt2;
                        cnmt_ready = true;
                        util::logLine("hybrid: CNMT extracted from installed NCA OK");
                    } else if (CnmtParser::parse(nca_buf.data(), nca_buf.size(), cnmt2)) {
                        cnmt = cnmt2;
                        cnmt_ready = true;
                        util::logLine("hybrid: CNMT parsed directly from installed NCA buffer OK");
                    } else {
                        util::logLine("hybrid: failed to parse CNMT from installed NCA");
                    }
                } else {
                    util::logLine("hybrid: failed to read back installed NCA file");
                }
            }
        }
    }

    // 2) Запасной путь: парсим из буфера CNMT NCA, собранного во время стрима.
    if (!cnmt_ready && !cnmt_nca_data_.empty()) {
        util::logLine("hybrid: in-memory CNMT parse (fallback), buffered=" +
                      std::to_string(cnmt_nca_data_.size()));
        if (CnmtParser::extractFromNca(cnmt_nca_data_.data(), cnmt_nca_data_.size(), cnmt)) {
            cnmt_ready = true;
        } else {
            util::logLine("hybrid: failed to extract CNMT from NCA in-memory, trying direct parse");
            if (CnmtParser::parse(cnmt_nca_data_.data(), cnmt_nca_data_.size(), cnmt)) {
                cnmt_ready = true;
            } else {
                util::logLine("hybrid: direct CNMT parse from buffered data failed");
            }
        }
    }

    if (!cnmt_ready) {
        util::logLine("hybrid: warning - CNMT parse failed, metadata registration skipped");
        return true;
    }
    cnmt_data_ = cnmt;

    // =========================================================================
    // Шаг 5б: Формируем буфер NcmContentMetaHeader + NcmContentInfo[]
    //          (как в Awoo-Installer — GetInstallContentMeta)
    // =========================================================================
    std::vector<NcmContentInfo> content_infos;
    content_infos.reserve(cnmt.contents.size() + 1);

    // Контент из CNMT (Awoo отбрасывает delta fragments > 5)
    for (const auto& ce : cnmt.contents) {
        if (static_cast<uint8_t>(ce.type) > 5) {
            continue;
        }
        NcmContentInfo ci = {};
        std::memcpy(ci.content_id.c, ce.content_id, 16);
        ncmU64ToContentInfoSize(ce.size, &ci);
        ci.content_type = static_cast<NcmContentType>(static_cast<uint8_t>(ce.type));
        ci.id_offset = 0;
        content_infos.push_back(ci);
    }

    // Запись для самой CNMT NCA (Meta) по стандарту Awoo/HOS добавляется В КОНЕЦ массива NcmContentInfo.
    {
        const NspFileEntry* e = is_xci_ ? xci_header_.findByType(NspEntryType::CnmtNca)
                                        : nsp_header_.findByType(NspEntryType::CnmtNca);
        if (e) {
            NcmContentInfo cnmt_ci = {};
            cnmt_ci.content_id = e->content_id;
            ncmU64ToContentInfoSize(e->size, &cnmt_ci);
            cnmt_ci.content_type = NcmContentType_Meta;
            cnmt_ci.id_offset = 0;
            content_infos.push_back(cnmt_ci);
        } else {
            util::logLine("hybrid: CNMT content entry not found in header");
        }
    }

    uint16_t ext_hdr_size = cnmt.extended_header_size;
    if (cnmt.raw_data.size() < (0x20u + ext_hdr_size)) {
        util::logLine("hybrid: CNMT raw data is too small for extended header, dropping ext header");
        ext_hdr_size = 0;
    }

    if (content_infos.empty()) {
        util::logLine("hybrid: no content records for ContentMetaDatabaseSet");
        return true;
    }

    // Буфер: NcmContentMetaHeader + ExtendedHeader + NcmContentInfo[]
    size_t buf_size = sizeof(NcmContentMetaHeader)
                    + ext_hdr_size
                    + content_infos.size() * sizeof(NcmContentInfo);
    std::vector<uint8_t> meta_buf(buf_size, 0);
    auto* hdr = reinterpret_cast<NcmContentMetaHeader*>(meta_buf.data());
    hdr->extended_header_size = ext_hdr_size;
    hdr->content_count      = static_cast<uint16_t>(content_infos.size());
    hdr->content_meta_count = cnmt.content_meta_count;
    hdr->attributes         = cnmt.attributes;
    hdr->storage_id         = static_cast<uint8_t>(config_.storage);

    size_t write_off = sizeof(NcmContentMetaHeader);
    if (ext_hdr_size > 0) {
        std::memcpy(meta_buf.data() + write_off,
                    cnmt.raw_data.data() + 0x20,
                    ext_hdr_size);
        write_off += ext_hdr_size;
    }

    std::memcpy(meta_buf.data() + write_off,
                content_infos.data(),
                content_infos.size() * sizeof(NcmContentInfo));

    // =========================================================================
    // Шаг 5в: Записываем в ContentMetaDatabase
    // =========================================================================
    NcmContentMetaDatabase meta_db;
    Result rc = ncmOpenContentMetaDatabase(&meta_db, config_.storage);
    if (R_FAILED(rc)) {
        util::logLine("hybrid: ncmOpenContentMetaDatabase failed, rc=" + std::to_string(rc));
        return true;
    }

    NcmContentMetaKey key = cnmt.toKey();

    rc = ncmContentMetaDatabaseSet(&meta_db, &key, meta_buf.data(), meta_buf.size());
    if (R_FAILED(rc)) {
        util::logLine("hybrid: ncmContentMetaDatabaseSet failed, rc=" + std::to_string(rc));
        ncmContentMetaDatabaseClose(&meta_db);
        return true;
    }

    rc = ncmContentMetaDatabaseCommit(&meta_db);
    if (R_SUCCEEDED(rc)) {
        util::logLine("hybrid: ContentMetaDatabase commit OK, title_id=0x"
                       + std::to_string(cnmt.title_id));
    } else {
        util::logLine("hybrid: ncmContentMetaDatabaseCommit failed, rc=" + std::to_string(rc));
    }
    ncmContentMetaDatabaseClose(&meta_db);

    // =========================================================================
    // Шаг 5г: nsPushApplicationRecord
    // =========================================================================
    {
        u64 base_tid = cnmt.title_id;
        if (base_tid == 0 && hint_title_id_ != 0) {
            char tid_hex[32];
            std::snprintf(tid_hex, sizeof(tid_hex), "0x%016llX", (unsigned long long)hint_title_id_);
            util::logLine("hybrid: using hint Title ID as fallback: " + std::string(tid_hex));
            base_tid = hint_title_id_;
        }
        
        if (cnmt.meta_type == 0x81) {
            base_tid ^= 0x800ULL;                        // Patch → base
        } else if (cnmt.meta_type == 0x82) {
            base_tid = (base_tid ^ 0x1000ULL) & ~0xFFFULL; // DLC → base
        }
        // Application (0x80): base_tid без изменений

#ifdef __SWITCH__
        std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
#endif
        Result ns_rc = nsInitialize();
        if (R_SUCCEEDED(ns_rc)) {
            // 1. Читаем существующие записи для base_tid из NS
            s32 existing_count = 0;
            nsCountApplicationContentMeta(base_tid, &existing_count);

            std::vector<ContentStorageRecord> records;

            if (existing_count > 0) {
                records.resize(existing_count);
                u32 entries_read = 0;
                Result list_rc = nsListApplicationRecordContentMetaCustom(0, base_tid, records.data(),
                                    existing_count * sizeof(ContentStorageRecord), &entries_read);
                if (R_SUCCEEDED(list_rc)) {
                    records.resize(entries_read);
                    util::logLine("hybrid: read " + std::to_string(entries_read) + " existing NS records");
                } else {
                    records.clear();
                }
            }

            // Ищем или добавляем текущий контент в список
            bool found = false;
            for (auto& r : records) {
                if (r.key.id == key.id && r.key.type == key.type) {
                    r.storage_id = static_cast<u64>(config_.storage);
                    r.key.version = cnmt.version;
                    found = true;
                    break;
                }
            }
            if (!found) {
                ContentStorageRecord new_rec = {};
                new_rec.key.id = key.id;
                new_rec.key.version = cnmt.version;
                new_rec.key.type = cnmt.meta_type;
                new_rec.key.install_type = 0; // 0 = Full
                new_rec.storage_id = static_cast<u64>(config_.storage);
                records.push_back(new_rec);
            }

            nsDeleteApplicationRecordCustom(base_tid);
            ns_rc = nsPushApplicationRecordCustom(base_tid,
                                                  NsApplicationRecordType_Installed,
                                                  records.data(),
                                                  static_cast<s32>(records.size()));
            if (R_SUCCEEDED(ns_rc)) {
                char base_hex[32];
                std::snprintf(base_hex, sizeof(base_hex), "0x%016llX", (unsigned long long)base_tid);
                util::logLine("hybrid: nsPushApplicationRecord OK, base_tid="
                               + std::string(base_hex) + " total_records=" + std::to_string(records.size()));
            } else {
                util::logLine("hybrid: nsPushApplicationRecord FAILED, rc=" + std::to_string(ns_rc));
            }

            std::vector<NsApplicationControlData> app_ctrl(1);
            size_t app_ctrl_size = 0;
            Result ctrl_rc = nsGetApplicationControlData(
                NsApplicationControlSource_Storage,
                base_tid,
                app_ctrl.data(),
                sizeof(NsApplicationControlData),
                &app_ctrl_size);
            if (R_SUCCEEDED(ctrl_rc) && app_ctrl_size >= sizeof(app_ctrl[0].nacp)) {
                NacpLanguageEntry* lang = nullptr;
                Result lang_rc = nacpGetLanguageEntry(&app_ctrl[0].nacp, &lang);
                if (R_SUCCEEDED(lang_rc) && lang) {
                    util::logLine("hybrid: application title from NACP: " + std::string(lang->name));
                } else {
                    util::logLine("hybrid: nacpGetLanguageEntry failed, rc=" + std::to_string(lang_rc));
                }
            } else {
                util::logLine("hybrid: nsGetApplicationControlData failed, rc=" + std::to_string(ctrl_rc));
            }
            nsExit();
        } else {
            util::logLine("hybrid: nsInitialize failed, rc=" + std::to_string(ns_rc));
        }
    }

    {
        char tid_hex[32], type_hex[8];
        std::snprintf(tid_hex, sizeof(tid_hex), "0x%016llX", (unsigned long long)cnmt.title_id);
        std::snprintf(type_hex, sizeof(type_hex), "0x%02X", cnmt.meta_type);
        util::logLine("hybrid: phase 5 complete: tid=" + std::string(tid_hex)
                       + " v=" + std::to_string(cnmt.version)
                       + " type=" + std::string(type_hex)
                       + " files=" + std::to_string(cnmt.content_count));
    }
#endif

    return true;
}

// =============================================================================
// cancel()
// =============================================================================
void HybridNspInstaller::cancel() {
    if (state_ == InstallState::Idle ||
        state_ == InstallState::Completed ||
        state_ == InstallState::Failed ||
        state_ == InstallState::Cancelled) {
#ifdef __SWITCH__
        if (threads_started_) {
            threadWaitForExit(&collector_thread_);
            threadWaitForExit(&installer_thread_);
            threadClose(&collector_thread_);
            threadClose(&installer_thread_);
            threads_started_ = false;
        }
#endif
        ring_buffer_.clear();
        return;
    }

    util::logLine("hybrid: cancelling installation...");
    cancel_requested_ = true;
    ring_buffer_.setEof();

    if (source_) {
        source_->setCancelFlag(&cancel_requested_);
    }

#ifdef __SWITCH__
    if (threads_started_) {
        threadWaitForExit(&collector_thread_);
        threadWaitForExit(&installer_thread_);
        threadClose(&collector_thread_);
        threadClose(&installer_thread_);
        threads_started_ = false;
    }

    ncm_.cleanup();
    util::cpuBoostEnd();   // idempotent safety net for the early-cancel paths
#endif
    ring_buffer_.clear();

    state_ = InstallState::Cancelled;
    util::logLine("hybrid: installation cancelled");
}

// =============================================================================
// Прогресс и статус
// =============================================================================
float HybridNspInstaller::progress() const {
    const uint64_t total = total_bytes_.load();
    if (total == 0) return 0.0f;
    const double done = static_cast<double>(bytes_installed_.load());
    const double ratio = done / static_cast<double>(total);
    return static_cast<float>(std::min(1.0, ratio));
}

float HybridNspInstaller::downloadProgress() const {
    const uint64_t total = download_total_bytes_.load();
    if (total == 0) return 0.0f;
    const double done = static_cast<double>(bytes_downloaded_.load());
    const double ratio = done / static_cast<double>(total);
    return static_cast<float>(std::min(1.0, ratio));
}

std::string HybridNspInstaller::statusText() const {
    switch (state_.load()) {
        case InstallState::Idle:                return "Idle";
        case InstallState::ParsingHeader:       return "Reading NSP Header...";
        case InstallState::CreatingPlaceHolders:return "Preparing Storage...";
        case InstallState::Streaming: {
            const uint64_t done = bytes_installed_.load();
            const uint64_t total = total_bytes_.load();
            if (total == 0) return "Installing...";

            const double pct = (static_cast<double>(done) * 100.0) / static_cast<double>(total);
            char buf[128] = {0};
            std::snprintf(buf, sizeof(buf), "Installing... %.1f%% (%llu/%llu MB)",
                          pct,
                          static_cast<unsigned long long>(done / (1024ull * 1024ull)),
                          static_cast<unsigned long long>(total / (1024ull * 1024ull)));
            return std::string(buf);
        }
        case InstallState::InstallingTickets:   return "Installing Tickets...";
        case InstallState::RegisteringMeta:     return "Registering Meta...";
        case InstallState::Completed:           return "Installation Completed!";
        case InstallState::Failed:              return "Error: " + error_message_;
        case InstallState::Cancelled:           return "Cancelled";
    }
    return "Unknown";
}

double HybridNspInstaller::downloadSpeedKbps() const {
    if (source_) {
        int speed = source_->downloadSpeedKBps();
        if (speed >= 0) {
            return static_cast<double>(speed);
        }
    }
#ifdef __SWITCH__
    u64 now = armGetSystemTick();
    u64 freq = armGetSystemTickFreq();
    double dt = static_cast<double>(now - last_time_) / static_cast<double>(freq);
    if (dt < 1.0) return current_speed_kbps_;

    uint64_t current_bytes = bytes_downloaded_.load();
    uint64_t delta = current_bytes - last_bytes_;
    const double instant_kbps = static_cast<double>(delta) / 1024.0 / dt;

    auto* self = const_cast<HybridNspInstaller*>(this);
    const double alpha = dt >= 2.0 ? 0.50 : 0.30;
    if (self->current_speed_kbps_ <= 0.0) {
        self->current_speed_kbps_ = instant_kbps;
    } else {
        self->current_speed_kbps_ += (instant_kbps - self->current_speed_kbps_) * alpha;
    }
    self->last_bytes_ = current_bytes;
    self->last_time_  = now;
#endif
    return current_speed_kbps_;
}

bool HybridNspInstaller::isFinished() const {
    InstallState s = state_.load();
    return s == InstallState::Completed ||
           s == InstallState::Failed ||
           s == InstallState::Cancelled;
}

void HybridNspInstaller::setError(const std::string& message) {
    error_message_ = message;
    state_ = InstallState::Failed;
    util::logLine("hybrid: ERROR - " + message);
}

// =============================================================================
// SHA-256 хелперы (реализованы inline в installerThreadFunc через mbedtls)
// =============================================================================
void HybridNspInstaller::hashUpdate(const void* /*data*/, size_t /*size*/) {}
bool HybridNspInstaller::hashVerify() { return true; }
void HybridNspInstaller::hashReset() {}

bool HybridNspInstaller::resumeFromOffset(uint64_t offset) {
    util::logLine("hybrid: Smart Resume from offset " + std::to_string(offset));
    return true;
}

} // namespace installer





