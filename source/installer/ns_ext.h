#pragma once

#ifdef __SWITCH__
#include <switch.h>
#include <vector>

namespace installer {

struct ContentStorageRecord {
    NcmContentMetaKey key;
    u64 storage_id;
};
static_assert(sizeof(ContentStorageRecord) == 24, "ContentStorageRecord must be exactly 24 bytes");

static constexpr u8 NsApplicationRecordType_Installed = 0x3;

Result nsPushApplicationRecordCustom(u64 application_id, u8 record_type,
                                     const ContentStorageRecord* records, s32 count);

Result nsListApplicationRecordContentMetaCustom(u64 application_id,
                                                std::vector<ContentStorageRecord>& out);

Result nsDeleteApplicationRecordCustom(u64 application_id);

void nsSyncLaunchVersion(u64 application_id, u32 max_patch_version);

} // namespace installer
#endif
