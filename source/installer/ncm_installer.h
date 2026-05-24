#pragma once
// =============================================================================
// NcmInstaller — запись NCA-контента через системный сервис ncm.
// Имитирует официальный процесс установки из eShop.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace installer {

class NcmInstaller {
public:
    NcmInstaller();
    ~NcmInstaller();

    // Запрет копирования
    NcmInstaller(const NcmInstaller&) = delete;
    NcmInstaller& operator=(const NcmInstaller&) = delete;

#ifdef __SWITCH__
    /// Инициализировать ncm-сессию для указанного хранилища
    /// @param storage NcmStorageId_SdCard или NcmStorageId_BuiltInUser (NAND)
    bool begin(NcmStorageId storage);

    /// Создать плейсхолдер для NCA-файла
    /// @param id      идентификатор контента (из имени NCA)
    /// @param size    полный размер NCA
    bool createPlaceHolder(const NcmContentId& id, uint64_t size);

    /// Записать блок данных в плейсхолдер
    /// @param id      идентификатор контента
    /// @param offset  смещение внутри плейсхолдера
    /// @param data    данные для записи
    /// @param size    размер блока
    bool writePlaceHolder(const NcmContentId& id,
                          uint64_t offset,
                          const void* data, size_t size);

    /// Финализировать плейсхолдер: зарегистрировать как готовый контент
    bool finalizePlaceHolder(const NcmContentId& id);

    /// Удалить плейсхолдер (при отмене/ошибке)
    bool deletePlaceHolder(const NcmContentId& id);

    /// Зарегистрировать метаданные CNMT для появления иконки в Home Menu
    bool registerContentMeta(const NcmContentMetaKey& key,
                             const void* cnmt_data, size_t cnmt_size);

    /// Получить путь к установленному NCA в content storage
    /// (используется для чтения CNMT после расшифровки hardware)
    /// @param id  идентификатор контента
    /// @param out_path  выходной буфер для пути (минимум FS_MAX_PATH)
    /// @return true если путь получен
    bool getStoragePath(const NcmContentId& id, char* out_path, size_t path_size);
    
    /// Считать содержимое установленного NCA файла по его ID
    /// Используется для получения расшифрованного CNMT после finalizePlaceHolder.
    bool readContentIdFile(const NcmContentId& id, std::vector<uint8_t>& out_buf);

    /// Прочитать внутренний CNMT-файл из установленного *.cnmt.nca через FsFileSystemType_ContentMeta.
    /// Это тот же путь, который используют полноценные инсталлеры вроде Awoo.
    bool readCnmtFromContentMetaFs(const NcmContentId& id, std::vector<uint8_t>& out_buf);

    /// Получить ссылку на открытый ContentStorage (для прямого использования)
    NcmContentStorage& contentStorage() { return content_storage_; }

    /// Очистить все созданные плейсхолдеры (вызывается при ошибке)
    void cleanup();

    bool isInitialized() const { return initialized_; }
    NcmStorageId storageId() const { return storage_id_; }

#else
    // Заглушки для компиляции на хосте
    bool begin(int storage) { (void)storage; return true; }
    bool isInitialized() const { return false; }
    void cleanup() {}
#endif

private:
#ifdef __SWITCH__
    NcmContentStorage    content_storage_ = {};
    NcmContentMetaDatabase meta_db_       = {};
    NcmStorageId         storage_id_      = NcmStorageId_SdCard;
    bool                 initialized_     = false;
    bool                 storage_opened_  = false;
    bool                 db_opened_       = false;

    /// Список созданных плейсхолдеров (для cleanup в случае ошибки)
    std::vector<NcmContentId> placeholders_;
#else
    bool initialized_ = false;
#endif
};

} // namespace installer
