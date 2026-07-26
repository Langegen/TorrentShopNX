#include "ticket_installer.h"

#include "../utils/log.h"

#include <cstring>
#include <string>
#ifdef __SWITCH__
#include <mutex>
extern std::recursive_mutex g_switch_service_mutex;
#endif

namespace installer {

#ifdef __SWITCH__

static Service g_esSrv;

static Result esInitializeCustom() {
    std::memset(&g_esSrv, 0, sizeof(g_esSrv));
    return smGetService(&g_esSrv, "es");
}

static void esExitCustom() {
    util::logLine("ticket: closing es service");
    serviceClose(&g_esSrv);
    std::memset(&g_esSrv, 0, sizeof(g_esSrv));
    util::logLine("ticket: es service closed");
}

static Result esImportTicketCustom(const void *tik_data, size_t tik_size, const void *cert_data, size_t cert_size) {
    Result rc = 0;
    if (cert_data && cert_size > 0) {
        // Awoo path: Cmd 1 with both buffers (ticket + cert)
        rc = serviceDispatch(&g_esSrv, 1,
            .buffer_attrs = {
                SfBufferAttr_In | SfBufferAttr_HipcMapAlias,
                SfBufferAttr_In | SfBufferAttr_HipcMapAlias
            },
            .buffers = {
                { tik_data, tik_size },
                { cert_data, cert_size }
            }
        );
    } else {
        // Fallback: import only ticket buffer.
        rc = serviceDispatch(&g_esSrv, 1,
            .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
            .buffers = { { tik_data, tik_size } }
        );
    }
    return rc;
}

static bool parseRightsIdFromTicket(const void* tik_data, size_t tik_size, u8* out_rights_id) {
    if (!tik_data || tik_size < 0x1B4 || !out_rights_id) return false;
    const uint8_t* tik = static_cast<const uint8_t*>(tik_data);

    // Смещение 0x1A4 — официальный стандарт расположения Rights ID в HOS Ticket
    bool all_zero = true;
    for (int i = 0; i < 16; ++i) {
        if (tik[0x1A4 + i] != 0) { all_zero = false; break; }
    }
    if (!all_zero) {
        std::memcpy(out_rights_id, tik + 0x1A4, 16);
        return true;
    }

    return false;
}

static bool esHasTicketCustom(const u8 rights_id[16]) {
    if (!rights_id) return false;

    struct {
        u8 rights_id[16];
    } in;
    std::memcpy(in.rights_id, rights_id, 16);

    u64 out_size = 0;
    // Cmd 9: GetCommonTicketSize
    Result rc = serviceDispatchInOut(&g_esSrv, 9, in, out_size);
    return R_SUCCEEDED(rc) && out_size > 0;
}

static std::string rightsIdToHex(const u8 rights_id[16]) {
    char hex[33] = {0};
    for (int i = 0; i < 16; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", rights_id[i]);
    }
    return std::string(hex);
}

bool TicketInstaller::installTicket(const void* tik_data, size_t tik_size,
                                     const void* cert_data, size_t cert_size) {
    if (!tik_data || tik_size == 0) {
        util::logLine("ticket: no ticket data");
        return false;
    }

#ifdef __SWITCH__
    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
#endif

    Result rc = esInitializeCustom();
    if (R_FAILED(rc)) {
        util::logLine("ticket: esInitialize failed, rc=" + std::to_string(rc));
        return false;
    }

    u8 rights_id[16] = {0};
    if (parseRightsIdFromTicket(tik_data, tik_size, rights_id)) {
        std::string hex_str = rightsIdToHex(rights_id);
        util::logLine("ticket: parsed Rights ID = " + hex_str);
        if (esHasTicketCustom(rights_id)) {
            util::logLine("ticket: ticket for Rights ID " + hex_str + " already exists in ES database, skipping overwrite to preserve base game license");
            esExitCustom();
            return true;
        }
    }

    bool ok = true;

    // Импортируем тикет с сертификатом
    rc = esImportTicketCustom(tik_data, tik_size, cert_data, cert_size);
    if (R_FAILED(rc)) {
        util::logLine("ticket: esImportTicket failed, rc=" + std::to_string(rc));
        ok = false;
    } else {
        util::logLine("ticket: esImportTicket OK");
    }

    esExitCustom();
    return ok;
}

bool TicketInstaller::installTicketOnly(const void* tik_data, size_t tik_size) {
    if (!tik_data || tik_size == 0) return false;

#ifdef __SWITCH__
    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
#endif

    Result rc = esInitializeCustom();
    if (R_FAILED(rc)) {
        util::logLine("ticket: esInitialize failed, rc=" + std::to_string(rc));
        return false;
    }

    u8 rights_id[16] = {0};
    if (parseRightsIdFromTicket(tik_data, tik_size, rights_id)) {
        std::string hex_str = rightsIdToHex(rights_id);
        util::logLine("ticket: parsed Rights ID = " + hex_str);
        if (esHasTicketCustom(rights_id)) {
            util::logLine("ticket: ticket for Rights ID " + hex_str + " already exists in ES database, skipping overwrite to preserve base game license");
            esExitCustom();
            return true;
        }
    }

    rc = esImportTicketCustom(tik_data, tik_size, nullptr, 0);
    bool ok = true;
    if (R_FAILED(rc)) {
        util::logLine("ticket: esImportTicket (no cert) failed, rc=" + std::to_string(rc));
        ok = false;
    } else {
        util::logLine("ticket: esImportTicket (no cert) OK");
    }

    esExitCustom();
    return ok;
}

#endif // __SWITCH__

} // namespace installer
