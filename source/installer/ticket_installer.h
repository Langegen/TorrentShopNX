#pragma once
// =============================================================================
// TicketInstaller — установка тикетов (.tik) и сертификатов (.cert)
// через Eticket Service (es).
// Необходим для того, чтобы установленные игры не показывали
// ошибку «Программа не куплена».
// =============================================================================

#include <cstddef>
#include <cstdint>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace installer {

class TicketInstaller {
public:
    TicketInstaller() = default;
    ~TicketInstaller() = default;

#ifdef __SWITCH__
    /// Установить тикет вместе с сертификатом через ES-сервис.
    /// @param tik_data    буфер с данными тикета
    /// @param tik_size    размер тикета
    /// @param cert_data   буфер с данными сертификата
    /// @param cert_size   размер сертификата
    /// @return true если тикет успешно импортирован
    bool installTicket(const void* tik_data, size_t tik_size,
                       const void* cert_data, size_t cert_size);

    /// Установить только тикет (без сертификата, если уже есть)
    bool installTicketOnly(const void* tik_data, size_t tik_size);
#else
    // Заглушки для хоста
    bool installTicket(const void*, size_t, const void*, size_t) { return true; }
    bool installTicketOnly(const void*, size_t) { return true; }
#endif
};

} // namespace installer
