#pragma once
// =============================================================================
// NexusUsbClient — интерфейс взаимодействия со службой ams:usb и расширениями
// NCM в составе кастомной прошивки Nexus-USB-HATS (Nexus-Atmosphere).
// Позволяет проверять наличие внешнего диска, опрашивать свободное место,
// открывать закреплённый ContentStorage на USB и регистрировать контент в оверлее.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace installer {

#pragma pack(push, 1)
struct NexusUsbStatus {
    uint8_t  mounted   = 0;
    uint8_t  ready     = 0;
    uint8_t  state     = 0;
    uint8_t  reserved  = 0;
    uint32_t result    = 0;
    uint64_t generation = 0;

    bool isAvailable() const {
        return mounted != 0 && ready != 0 && state == 4 && result == 0 && generation != 0;
    }
};

struct NexusUsbSpace {
    uint64_t free_bytes  = 0;
    uint64_t total_bytes = 0;
};

struct NexusCapabilities {
    uint32_t magic    = 0;
    uint32_t abi      = 0;
    uint32_t titles   = 0;
    uint32_t content  = 0;
    uint32_t features = 0;
    uint32_t reserved = 0;
};
#pragma pack(pop)

class NexusUsbClient {
public:
    static NexusUsbClient& instance();

    /// Проверить, активна ли поддержка Nexus USB на уровне системных сервисов
    bool isSupported();

    /// Проверить, подключен ли внешний USB-накопитель и готов ли к установке
    bool isDriveConnected();

    /// Получить статус USB-накопителя (включая generation)
    bool queryStatus(NexusUsbStatus& out_status);

    /// Получить статистику свободного и общего места на USB-накопителе
    bool getSpace(uint64_t generation, uint64_t& out_free, uint64_t& out_total);

    /// Проверить, разрешены ли изменения накопителя (отсутствие журнала восстановления)
    bool isMutationAllowed();

#ifdef __SWITCH__
    /// Открыть закрепленный ContentStorage для USB (команда 65031)
    Result openPinnedContentStorage(uint64_t generation, NcmContentStorage* out_storage);

    /// Зарегистрировать ContentId в постоянной базе сопоставления Nexus (команда 65032)
    Result ensureContentRegistered(uint64_t generation, const NcmContentId& id, int64_t size);
#endif

    /// Сохранить резервную копию тикета на USB/SD для обеспечения портативности библиотеки
    bool saveTicketBackup(uint64_t title_id, const void* tik_data, size_t tik_size);

private:
    NexusUsbClient();
    ~NexusUsbClient();

    NexusUsbClient(const NexusUsbClient&) = delete;
    NexusUsbClient& operator=(const NexusUsbClient&) = delete;

#ifdef __SWITCH__
    bool ensureServiceOpen();
    void closeService();

    Service usb_service_ = {};
    bool service_opened_ = false;
#endif

    bool capabilities_checked_ = false;
    bool capabilities_supported_ = false;
};

} // namespace installer
