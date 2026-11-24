#include "ns_ext.h"

#ifdef __SWITCH__
#include "../utils/log.h"

namespace installer {

Result nsPushApplicationRecordCustom(u64 application_id, u8 record_type,
                                     const ContentStorageRecord* records, s32 count) {
    Service app_manager;
    Result rc = nsGetApplicationManagerInterface(&app_manager);
    if (R_FAILED(rc)) return rc;

    struct {
        u8 record_type;
        u8 pad[7];
        u64 application_id;
    } in = {};
    in.record_type = record_type;
    in.application_id = application_id;

    rc = serviceDispatchIn(&app_manager, 16, in,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
        .buffers = { { records, static_cast<size_t>(count) * sizeof(ContentStorageRecord) } }
    );
    serviceClose(&app_manager);
    return rc;
}

Result nsListApplicationRecordContentMetaCustom(u64 application_id,
                                                std::vector<ContentStorageRecord>& out) {
    out.clear();
    Service app_manager;
    Result rc = nsGetApplicationManagerInterface(&app_manager);
    if (R_FAILED(rc)) return rc;

    u64 offset = 0;
    for (;;) {
        ContentStorageRecord chunk[32] = {};
        struct {
            u64 offset;
            u64 application_id;
        } in = { offset, application_id };
        s32 written = 0;
        rc = serviceDispatchInOut(&app_manager, 17, in, written,
            .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
            .buffers = { { chunk, sizeof(chunk) } }
        );
        if (R_FAILED(rc)) break;
        if (written <= 0) break;
        out.insert(out.end(), chunk, chunk + written);
        offset += static_cast<u64>(written);
        if (written < 32) break;
    }
    serviceClose(&app_manager);

    if (R_SUCCEEDED(rc)) return 0;

    // Fallback via standard libnx command if cmd 17 is unavailable
    out.clear();
    s32 idx = 0;
    Result st_rc = 0;
    for (;;) {
        NsApplicationContentMetaStatus status_buf[32] = {};
        s32 written = 0;
        st_rc = nsListApplicationContentMetaStatus(application_id, idx, status_buf, 32, &written);
        if (R_FAILED(st_rc) || written <= 0) break;
        for (s32 i = 0; i < written; ++i) {
            ContentStorageRecord rec = {};
            rec.key.id = status_buf[i].application_id;
            rec.key.version = status_buf[i].version;
            rec.key.type = status_buf[i].meta_type;
            rec.key.install_type = NcmContentInstallType_Full;
            rec.storage_id = static_cast<u64>(status_buf[i].storageID);
            out.push_back(rec);
        }
        idx += written;
        if (written < 32) break;
    }
    return R_SUCCEEDED(st_rc) ? 0 : rc;
}

Result nsDeleteApplicationRecordCustom(u64 application_id) {
    Service app_manager;
    Result rc = nsGetApplicationManagerInterface(&app_manager);
    if (R_FAILED(rc)) return rc;

    rc = serviceDispatchIn(&app_manager, 27, application_id);
    serviceClose(&app_manager);
    return rc;
}

void nsSyncLaunchVersion(u64 application_id, u32 max_patch_version) {
    Result rc = avmInitialize();
    if (R_FAILED(rc)) return; // Safely ignored on FW < 6.0.0
    Result push_rc = avmPushLaunchVersion(application_id, max_patch_version);
    avmExit();
    util::logLine("ns_ext: avmPushLaunchVersion tid=" + std::to_string(application_id) +
                  " ver=" + std::to_string(max_patch_version) +
                  " rc=" + std::to_string(push_rc));
}

} // namespace installer
#endif
