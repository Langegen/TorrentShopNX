#include "nexus_usb_client.h"
#include "../utils/log.h"

#include <cstddef>
#include <cstring>
#include <cstdio>
#include <filesystem>

#ifdef __SWITCH__
#include <mutex>
extern std::recursive_mutex g_switch_service_mutex;
#endif

namespace installer {

namespace {
constexpr uint32_t kCapabilityMagic = 0x3350584E;
constexpr uint32_t kCapabilityAbi = 1u;
constexpr uint32_t kFeatureUsbRegistration = 1u;
constexpr uint32_t kNcmCmdNativeSdStorageOpen = 65030;
constexpr uint32_t kNcmCmdPinnedStorageOpen   = 65031;
constexpr uint32_t kNcmCmdEnsureRegistered    = 65032;
constexpr uint32_t kNcmCmdCapabilities        = 65033;
} // namespace

NexusUsbClient& NexusUsbClient::instance() {
    static NexusUsbClient s_instance;
    return s_instance;
}

NexusUsbClient::NexusUsbClient() = default;

NexusUsbClient::~NexusUsbClient() {
#ifdef __SWITCH__
    closeService();
#endif
}

#ifdef __SWITCH__

namespace {

enum class SmProbeResult {
    Present,
    Absent,
    Error,
};

std::string formatHex32(uint32_t value) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(value));
    return std::string(buf);
}

SmProbeResult smProbeServiceNonBlocking(const char* name) {
    if (!hosversionIsAtmosphere()) {
        static bool logged_non_ams = false;
        if (!logged_non_ams) {
            logged_non_ams = true;
            util::logLine("nexus_usb: not running under Atmosphere, skipping ams:usb probe");
        }
        return SmProbeResult::Absent;
    }

    SmServiceName srv_name = smEncodeName(name);
    u8 has_service = 0;
    Result rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    if (hosversionIsAtmosphere() || hosversionAtLeast(12, 0, 0)) {
        TipcService* tipc_sm = smGetServiceSessionTipc();
        if (tipc_sm && tipc_sm->session != INVALID_HANDLE) {
            rc = tipcDispatchInOut(tipc_sm, 65100, srv_name, has_service);
        }
    }

    if (R_FAILED(rc)) {
        Service* cmif_sm = smGetServiceSession();
        if (cmif_sm && serviceIsActive(cmif_sm)) {
            rc = serviceDispatchInOut(cmif_sm, 65100, srv_name, has_service);
        }
    }

    util::logLine("nexus_usb: AtmosphereHasService('" + std::string(name) +
                  "') rc=" + formatHex32(rc) + " has=" + std::to_string(has_service));
    if (R_FAILED(rc)) {
        return SmProbeResult::Error;
    }
    return (has_service != 0) ? SmProbeResult::Present : SmProbeResult::Absent;
}

} // namespace

void NexusUsbClient::closeService() {
    if (service_opened_) {
        if (serviceIsActive(&usb_service_)) {
            serviceClose(&usb_service_);
        }
        service_opened_ = false;
    }
}

bool NexusUsbClient::ensureServiceOpen() {
    if (service_opened_ && serviceIsActive(&usb_service_)) {
        return true;
    }

    std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
    if (service_opened_ && serviceIsActive(&usb_service_)) {
        return true;
    }

    // Never call smGetService for an unregistered service: Horizon OS /
    // Atmosphere SM defers GetService indefinitely until the service registers,
    // which would permanently deadlock g_switch_service_mutex and the caller.
    if (smProbeServiceNonBlocking("ams:usb") != SmProbeResult::Present) {
        service_opened_ = false;
        return false;
    }

    Result rc = smGetService(&usb_service_, "ams:usb");
    if (R_SUCCEEDED(rc)) {
        service_opened_ = true;
        return true;
    }

    util::logLine("nexus_usb: smGetService(ams:usb) failed, rc=" + formatHex32(rc));
    service_opened_ = false;
    return false;
}

bool NexusUsbClient::isSupported() {
    if (capabilities_checked_) {
        return capabilities_supported_;
    }

    std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
    if (capabilities_checked_) {
        return capabilities_supported_;
    }

    if (!hosversionIsAtmosphere()) {
        util::logLine("nexus_usb: not running under Atmosphere, Nexus USB disabled");
        capabilities_checked_ = true;
        capabilities_supported_ = false;
        return false;
    }

    if (!(service_opened_ && serviceIsActive(&usb_service_))) {
        SmProbeResult probe = smProbeServiceNonBlocking("ams:usb");
        if (probe == SmProbeResult::Absent) {
            capabilities_checked_ = true;
            capabilities_supported_ = false;
            return false;
        }
        if (probe == SmProbeResult::Error) {
            return false;
        }

        Result open_rc = smGetService(&usb_service_, "ams:usb");
        if (R_FAILED(open_rc)) {
            util::logLine("nexus_usb: smGetService(ams:usb) failed, rc=" + formatHex32(open_rc));
            service_opened_ = false;
            return false;
        }
        service_opened_ = true;
    }

    Result ncm_init_rc = ncmInitialize();
    if (R_FAILED(ncm_init_rc)) {
        util::logLine("nexus_usb: ncmInitialize failed, rc=" + formatHex32(ncm_init_rc));
        return false;
    }

    NexusCapabilities caps = {};
    Result rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    Service* ncm_sess = ncmGetServiceSession();
    if (ncm_sess && serviceIsActive(ncm_sess)) {
        // Nexus-Atmosphere cmd 65033 (GetUsbInstallCapabilities) takes `const sf::OutBuffer &out`
        // (strictly Type-B HipcMapAlias). Using HipcAutoSelect with a 24-byte struct routes into
        // NCM's 0x400-byte Type-C pointer buffer and fails with fs::ResultInvalidArgument.
        rc = serviceDispatch(ncm_sess, kNcmCmdCapabilities,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
            .buffers = { { &caps, sizeof(caps) } }
        );
    }
    ncmExit();

    const bool ok = R_SUCCEEDED(rc) &&
                    caps.magic == kCapabilityMagic &&
                    caps.abi == kCapabilityAbi &&
                    (caps.features & kFeatureUsbRegistration) != 0 &&
                    caps.reserved == 0;

    capabilities_checked_ = true;
    capabilities_supported_ = ok;

    util::logLine("nexus_usb: capability query " + std::string(ok ? "OK" : "unsupported") +
                  " rc=" + formatHex32(rc) +
                  " magic=" + formatHex32(caps.magic) +
                  " abi=" + std::to_string(caps.abi) +
                  " titles=" + std::to_string(caps.titles) +
                  " content=" + std::to_string(caps.content) +
                  " features=" + formatHex32(caps.features) +
                  " reserved=" + std::to_string(caps.reserved));

    return capabilities_supported_;
}

bool NexusUsbClient::isDriveConnected() {
    if (!isSupported()) return false;

    NexusUsbStatus status = {};
    if (!queryStatus(status)) {
        return false;
    }

    return status.isAvailable();
}

bool NexusUsbClient::queryStatus(NexusUsbStatus& out_status) {
    out_status = {};
    if (!isSupported()) return false;

    std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
    if (!ensureServiceOpen()) return false;

    Result rc = serviceDispatchOut(&usb_service_, 0, out_status);
    if (R_FAILED(rc)) {
        util::logLine("nexus_usb: queryStatus failed, rc=" + formatHex32(rc));
        closeService();
        return false;
    }

    if (!has_logged_status_ ||
        last_logged_status_.mounted != out_status.mounted ||
        last_logged_status_.ready != out_status.ready ||
        last_logged_status_.state != out_status.state ||
        last_logged_status_.result != out_status.result ||
        last_logged_status_.generation != out_status.generation) {
        has_logged_status_ = true;
        last_logged_status_ = out_status;
        util::logLine("nexus_usb: status mounted=" + std::to_string(out_status.mounted) +
                      " ready=" + std::to_string(out_status.ready) +
                      " state=" + std::to_string(out_status.state) +
                      " result=" + formatHex32(out_status.result) +
                      " gen=" + std::to_string(out_status.generation) +
                      " avail=" + std::to_string(out_status.isAvailable() ? 1 : 0));
    }

    return true;
}

bool NexusUsbClient::getSpace(uint64_t generation, uint64_t& out_free, uint64_t& out_total) {
    out_free = 0;
    out_total = 0;
    if (!isSupported()) return false;

    std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
    if (!ensureServiceOpen()) return false;

    NexusUsbStatus cur_status = {};
    if (!queryStatus(cur_status) || !cur_status.isAvailable()) {
        return false;
    }

    if (generation != 0 && cur_status.generation != generation) {
        util::logLine("nexus_usb: generation mismatch: req=" + std::to_string(generation) +
                      " cur=" + std::to_string(cur_status.generation));
        return false;
    }

    const uint64_t expected_gen = (generation != 0) ? generation : cur_status.generation;

    NexusUsbSpace space = {};
    Result rc = serviceDispatchOut(&usb_service_, 9, space);
    if (R_FAILED(rc)) {
        util::logLine("nexus_usb: getSpace failed, rc=" + formatHex32(rc));
        closeService();
        return false;
    }

    if (space.total_bytes == 0 || space.free_bytes > space.total_bytes) {
        util::logLine("nexus_usb: getSpace returned invalid sizes free=" +
                      std::to_string(space.free_bytes) + " total=" + std::to_string(space.total_bytes));
        return false;
    }

    NexusUsbStatus after_status = {};
    if (!queryStatus(after_status) || !after_status.isAvailable() || after_status.generation != expected_gen) {
        util::logLine("nexus_usb: drive changed during getSpace (expected gen=" +
                      std::to_string(expected_gen) + " cur=" + std::to_string(after_status.generation) + ")");
        return false;
    }

    out_free = space.free_bytes;
    out_total = space.total_bytes;
    return true;
}

bool NexusUsbClient::isMutationAllowed() {
    FsFileSystem sd = {};
    Result rc = fsOpenSdCardFileSystem(&sd);
    if (R_FAILED(rc)) {
        util::logLine("nexus_usb: fsOpenSdCardFileSystem failed in isMutationAllowed, rc=" + formatHex32(rc));
        return false;
    }

    char path[FS_MAX_PATH] = {};
    FsDirEntryType type = FsDirEntryType_File;

    std::snprintf(path, sizeof(path), "%s", "/atmosphere/usb_content-repair.pending");
    rc = fsFsGetEntryType(&sd, path, &type);
    if (rc == MAKERESULT(2, 1)) {
        std::memset(path, 0, sizeof(path));
        std::snprintf(path, sizeof(path), "%s", "/atmosphere/usb-import.pending");
        rc = fsFsGetEntryType(&sd, path, &type);
    }
    fsFsClose(&sd);

    // 0x202 (Module 2, Desc 1) = PathNotFound: отсутствие журналов означает разрешение записи
    const bool allowed = (rc == MAKERESULT(2, 1));
    if (!allowed) {
        util::logLine("nexus_usb: mutation blocked (journal check rc=" + formatHex32(rc) +
                      " path=" + std::string(path) + ")");
    }
    return allowed;
}

Result NexusUsbClient::openNativeSdContentStorage(NcmContentStorage* out_storage) {
    if (!out_storage) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *out_storage = {};

    std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        util::logLine("nexus_usb: ncmInitialize failed in openNativeSdContentStorage, rc=" + formatHex32(rc));
        return rc;
    }

    Service* ncm_sess = ncmGetServiceSession();
    if (!ncm_sess || !serviceIsActive(ncm_sess)) {
        ncmExit();
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }

    rc = serviceDispatch(ncm_sess, kNcmCmdNativeSdStorageOpen,
        .out_num_objects = 1,
        .out_objects = &out_storage->s
    );
    ncmExit();

    if (R_FAILED(rc)) {
        util::logLine("nexus_usb: openNativeSdContentStorage failed, rc=" + formatHex32(rc));
    }
    return rc;
}

Result NexusUsbClient::openPinnedContentStorage(uint64_t generation, NcmContentStorage* out_storage) {
    if (!out_storage || generation == 0) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *out_storage = {};

    if (!isMutationAllowed()) {
        util::logLine("nexus_usb: mutation not allowed due to pending repair/import journal");
        return MAKERESULT(505, 900);
    }

    std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        util::logLine("nexus_usb: ncmInitialize failed in openPinnedContentStorage, rc=" + formatHex32(rc));
        return rc;
    }

    Service* ncm_sess = ncmGetServiceSession();
    if (!ncm_sess || !serviceIsActive(ncm_sess)) {
        ncmExit();
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }

    rc = serviceDispatchIn(ncm_sess, kNcmCmdPinnedStorageOpen, generation,
        .out_num_objects = 1,
        .out_objects = &out_storage->s
    );
    ncmExit();

    if (R_FAILED(rc)) {
        util::logLine("nexus_usb: openPinnedContentStorage failed, rc=" + formatHex32(rc));
    }
    return rc;
}

Result NexusUsbClient::ensureContentRegistered(uint64_t generation, const NcmContentId& id, int64_t size) {
    if (!generation || size <= 0) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

#pragma pack(push, 1)
    struct EnsureRequest {
        NcmContentId content_id;
        uint64_t generation;
        int64_t expected_size;
    };
#pragma pack(pop)
    static_assert(sizeof(EnsureRequest) == 32);
    static_assert(offsetof(EnsureRequest, content_id) == 0);
    static_assert(offsetof(EnsureRequest, generation) == 16);
    static_assert(offsetof(EnsureRequest, expected_size) == 24);

    EnsureRequest in = { id, generation, size };

    std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        util::logLine("nexus_usb: ncmInitialize failed in ensureContentRegistered, rc=" + formatHex32(rc));
        return rc;
    }

    Service* ncm_sess = ncmGetServiceSession();
    if (!ncm_sess || !serviceIsActive(ncm_sess)) {
        ncmExit();
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }

    rc = serviceDispatchIn(ncm_sess, kNcmCmdEnsureRegistered, in);
    ncmExit();

    if (R_FAILED(rc)) {
        util::logLine("nexus_usb: ensureContentRegistered failed, rc=" + formatHex32(rc));
    }
    return rc;
}

#else

// Stubs for PC / Desktop build
bool NexusUsbClient::isSupported() { return false; }
bool NexusUsbClient::isDriveConnected() { return false; }
bool NexusUsbClient::queryStatus(NexusUsbStatus& out_status) { (void)out_status; return false; }
bool NexusUsbClient::getSpace(uint64_t, uint64_t& out_free, uint64_t& out_total) { out_free = 0; out_total = 0; return false; }
bool NexusUsbClient::isMutationAllowed() { return true; }

#endif

bool NexusUsbClient::saveTicketBackup(uint64_t title_id, const void* tik_data, size_t tik_size) {
    if (!tik_data || tik_size == 0 || title_id == 0) return false;

    try {
        std::string dir_path = "sdmc:/atmosphere/usb_content/tickets";
        std::error_code ec;
        std::filesystem::create_directories(dir_path, ec);

        char filename[64];
        std::snprintf(filename, sizeof(filename), "%016llx.tik", static_cast<unsigned long long>(title_id));
        std::string file_path = dir_path + "/" + filename;

        FILE* f = std::fopen(file_path.c_str(), "wb");
        if (!f) return false;

        size_t written = std::fwrite(tik_data, 1, tik_size, f);
        std::fclose(f);
        return (written == tik_size);
    } catch (...) {
        return false;
    }
}

} // namespace installer
