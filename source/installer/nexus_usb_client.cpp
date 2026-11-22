#include "nexus_usb_client.h"
#include "../utils/log.h"

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
constexpr uint32_t kFeatureUsbRegistration = 1u;
constexpr uint32_t kNcmCmdPinnedStorageOpen = 65031;
constexpr uint32_t kNcmCmdEnsureRegistered  = 65032;
constexpr uint32_t kNcmCmdCapabilities      = 65033;
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

bool smHasServiceNonBlocking(const char* name) {
    if (!hosversionIsAtmosphere()) {
        util::logLine("nexus_usb: not running under Atmosphere, skipping ams:usb probe");
        return false;
    }

    SmServiceName srv_name = smEncodeName(name);
    u8 has_service = 0;
    Result rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    if (hosversionAtLeast(12, 0, 0)) {
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
                  "') rc=" + std::to_string(rc) + " has=" + std::to_string(has_service));
    return R_SUCCEEDED(rc) && (has_service != 0);
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
    if (!smHasServiceNonBlocking("ams:usb")) {
        service_opened_ = false;
        return false;
    }

    Result rc = smGetService(&usb_service_, "ams:usb");
    if (R_SUCCEEDED(rc)) {
        service_opened_ = true;
        return true;
    }

    util::logLine("nexus_usb: smGetService(ams:usb) failed, rc=" + std::to_string(rc));
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

    capabilities_checked_ = true;
    capabilities_supported_ = false;

    if (!ensureServiceOpen()) {
        return false;
    }

    bool ncm_inited_here = false;
    Service* ncm_sess = ncmGetServiceSession();
    if (!ncm_sess || !serviceIsActive(ncm_sess)) {
        if (R_SUCCEEDED(ncmInitialize())) {
            ncm_inited_here = true;
            ncm_sess = ncmGetServiceSession();
        }
    }

    if (ncm_sess && serviceIsActive(ncm_sess)) {
        NexusCapabilities caps = {};
        Result rc = serviceDispatchOut(ncm_sess, kNcmCmdCapabilities, caps);
        if (R_FAILED(rc)) {
            rc = serviceDispatch(ncm_sess, kNcmCmdCapabilities,
                .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out },
                .buffers = { { &caps, sizeof(caps) } }
            );
        }

        if (R_SUCCEEDED(rc) && caps.magic == kCapabilityMagic && (caps.features & kFeatureUsbRegistration)) {
            capabilities_supported_ = true;
            util::logLine("nexus_usb: detected Nexus-Atmosphere with USB registration support");
        } else {
            util::logLine("nexus_usb: capability query returned rc=" + std::to_string(rc) +
                          " magic=0x" + std::to_string(caps.magic));
        }
    }

    if (ncm_inited_here) {
        ncmExit();
    }

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
    if (!ensureServiceOpen()) return false;

    std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
    Result rc = serviceDispatchOut(&usb_service_, 0, out_status);
    if (R_FAILED(rc)) {
        closeService();
        return false;
    }

    return true;
}

bool NexusUsbClient::getSpace(uint64_t generation, uint64_t& out_free, uint64_t& out_total) {
    out_free = 0;
    out_total = 0;
    if (!isSupported()) return false;
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

    std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
    NexusUsbSpace space = {};
    Result rc = serviceDispatchOut(&usb_service_, 9, space);
    if (R_FAILED(rc)) {
        util::logLine("nexus_usb: getSpace failed, rc=" + std::to_string(rc));
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
        return false;
    }

    FsDirEntryType type = FsDirEntryType_File;
    rc = fsFsGetEntryType(&sd, "/atmosphere/usb_content-repair.pending", &type);
    fsFsClose(&sd);

    // 0x202 (Module 2, Desc 1) = PathNotFound: отсутсвие журнала означает разрешение записи
    return (rc == MAKERESULT(2, 1));
}

Result NexusUsbClient::openPinnedContentStorage(uint64_t generation, NcmContentStorage* out_storage) {
    if (!out_storage) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    if (!isMutationAllowed()) {
        util::logLine("nexus_usb: mutation not allowed due to pending repair journal");
        return MAKERESULT(505, 900);
    }

    Service* ncm_sess = ncmGetServiceSession();
    if (!ncm_sess || !serviceIsActive(ncm_sess)) {
        Result rc = ncmInitialize();
        if (R_FAILED(rc)) return rc;
        ncm_sess = ncmGetServiceSession();
    }

    std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
    Result rc = serviceDispatchIn(ncm_sess, kNcmCmdPinnedStorageOpen, generation,
        .out_num_objects = 1,
        .out_objects = &out_storage->s
    );

    if (R_FAILED(rc)) {
        util::logLine("nexus_usb: openPinnedContentStorage failed, rc=" + std::to_string(rc));
    }
    return rc;
}

Result NexusUsbClient::ensureContentRegistered(uint64_t generation, const NcmContentId& id, int64_t size) {
    if (!generation || size <= 0) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Service* ncm_sess = ncmGetServiceSession();
    if (!ncm_sess || !serviceIsActive(ncm_sess)) {
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }

#pragma pack(push, 1)
    struct EnsureRequest {
        NcmContentId content_id;
        uint64_t generation;
        int64_t expected_size;
    };
#pragma pack(pop)

    EnsureRequest in = { id, generation, size };

    std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
    Result rc = serviceDispatchIn(ncm_sess, kNcmCmdEnsureRegistered, in);
    if (R_FAILED(rc)) {
        util::logLine("nexus_usb: ensureContentRegistered failed, rc=" + std::to_string(rc));
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
