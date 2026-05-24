#include "ticket_installer.h"

#include "../utils/log.h"

#include <cstring>
#include <string>

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

bool TicketInstaller::installTicket(const void* tik_data, size_t tik_size,
                                     const void* cert_data, size_t cert_size) {
    if (!tik_data || tik_size == 0) {
        util::logLine("ticket: no ticket data");
        return false;
    }

    Result rc = esInitializeCustom();
    if (R_FAILED(rc)) {
        util::logLine("ticket: esInitialize failed, rc=" + std::to_string(rc));
        return false;
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

    Result rc = esInitializeCustom();
    if (R_FAILED(rc)) {
        util::logLine("ticket: esInitialize failed, rc=" + std::to_string(rc));
        return false;
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
