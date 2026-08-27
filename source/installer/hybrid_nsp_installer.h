#pragma once
// =============================================================================
// HybridNspInstaller — двухпоточный оркестратор потоковой установки NSP.
//
// Архитектура:
//   Thread A (Collector):  DataSource → RingBuffer
//   Thread B (Installer):  RingBuffer → NCM (плейсхолдеры)
//
// После завершения: тикеты → ES, CNMT → ncm → иконка в Home Menu.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <string>
#include <vector>

#include "../datasource/i_data_source.h"
#include "../buffer/ring_buffer.h"
#include "nsp_header.h"
#include "xci_header.h"
#include "ncm_installer.h"
#include "ticket_installer.h"
#include "cnmt_parser.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace installer {

/// Конфигурация установки
struct InstallConfig {
    size_t   buffer_size  = 64 * 1024 * 1024;  ///< Размер кольцевого буфера
    size_t   chunk_size   = 4 * 1024 * 1024;    ///< Размер одного chunk для read/write
    bool     verify_sha256 = true;              ///< Проверять SHA-256 NCA на лету
    bool     install_ticket = true;             ///< Устанавливать тикеты
#ifdef __SWITCH__
    NcmStorageId storage  = NcmStorageId_SdCard; ///< Куда устанавливать (SD/NAND)
#else
    int      storage      = 0;
#endif
};

/// Состояние процесса установки
enum class InstallState {
    Idle,               ///< Не запущен
    ParsingHeader,      ///< Парсинг NSP-заголовка
    CreatingPlaceHolders, ///< Создание плейсхолдеров через NCM
    Streaming,          ///< Двухпоточный стриминг (Collector + Installer)
    InstallingTickets,  ///< Установка тикетов через ES
    RegisteringMeta,    ///< Регистрация CNMT в ContentMetaDatabase
    Completed,          ///< Установка завершена успешно
    Failed,             ///< Ошибка
    Cancelled           ///< Отменено пользователем
};

class HybridNspInstaller {
    // Статические entry points для libnx Thread
    friend void collectorThreadEntry(void* arg);
    friend void installerThreadEntry(void* arg);
public:
    HybridNspInstaller();
    ~HybridNspInstaller();

    // Запрет копирования
    HybridNspInstaller(const HybridNspInstaller&) = delete;
    HybridNspInstaller& operator=(const HybridNspInstaller&) = delete;

    /// Запустить потоковую установку NSP из указанного DataSource.
    /// @param source  источник данных (время жизни управляется вызывающим)
    /// @param config  конфигурация установки
    /// @return true если установка успешно запущена
    bool start(datasource::IDataSource* source, const InstallConfig& config);

    /// Отменить установку
    void cancel();

    /// Текущий прогресс (0.0 — 1.0)
    float progress() const;

    /// Текущее состояние
    InstallState state() const { return state_.load(); }

    /// Текстовое описание текущего статуса (для UI)
    std::string statusText() const;

    /// Завершена ли установка (успешно или с ошибкой)?
    bool isFinished() const;

    /// Была ли ошибка?
    bool hasError() const { return state_.load() == InstallState::Failed; }

    /// Текст ошибки
    std::string errorMessage() const { return error_message_; }

    /// Статистика
    uint64_t bytesDownloaded() const { return bytes_downloaded_.load(); }
    uint64_t downloadTotalBytes() const { return download_total_bytes_.load(); }
    uint64_t bytesInstalled() const { return bytes_installed_.load(); }
    uint64_t totalBytes() const { return total_bytes_.load(); }
    float    downloadProgress() const;
    double   downloadSpeedKbps() const;

    /// Установить подсказку о Title ID (извлекается из имени файла)
    void setHintTitleId(uint64_t tid) { hint_title_id_ = tid; }

    /// Установить подсказку о формате контейнера по имени исходного файла.
    void setSourceFileNameHint(const std::string& name);

private:
    // =========================================================================
    // Потоки
    // =========================================================================

    /// Thread A: скачивает из DataSource → пишет в RingBuffer
    void collectorThreadFunc();

    /// Thread B: читает из RingBuffer → пишет через NCM
    void installerThreadFunc();

    // =========================================================================
    // Этапы установки
    // =========================================================================

    /// 1. Скачать и распарсить NSP-заголовок
    bool parseNspHeaderPhase();

    /// 2. Создать NCM-плейсхолдеры для всех NCA
    bool createPlaceHoldersPhase();

    /// 3. Запустить потоки Collector + Installer
    bool startStreamingPhase();

    /// 4. Установить тикеты (.tik + .cert)
    bool installTicketsPhase();

    /// 5. Зарегистрировать CNMT
    bool registerContentMetaPhase();

    // =========================================================================
    // Smart Resume
    // =========================================================================

    /// Возобновить загрузку с указанного смещения
    bool resumeFromOffset(uint64_t offset);

    // =========================================================================
    // SHA-256 верификация
    // =========================================================================

    /// Обновить хеш текущего NCA
    void hashUpdate(const void* data, size_t size);

    /// Проверить финальный хеш
    bool hashVerify();

    /// Сбросить хеш для нового NCA
    void hashReset();

    // =========================================================================
    // Вспомогательные методы
    // =========================================================================

    /// Определить оптимальный размер буфера на основе доступной RAM
    static size_t autoBufferSize();

    /// Установить ошибку и перейти в состояние Failed
    void setError(const std::string& message);

    // =========================================================================
    // Данные
    // =========================================================================

    datasource::IDataSource* source_ = nullptr;
    InstallConfig config_;

    buffer::RingBuffer ring_buffer_;
    bool               is_xci_ = false;
    NspHeader          nsp_header_;
    XciHeader          xci_header_;
    NcmInstaller       ncm_;
    TicketInstaller    ticket_;

    // Состояние (atomic для потокобезопасности)
    std::atomic<InstallState> state_{InstallState::Idle};
    std::atomic<bool>         cancel_requested_{false};
    std::atomic<uint64_t>     bytes_downloaded_{0};
    std::atomic<uint64_t>     download_total_bytes_{0};
    std::atomic<uint64_t>     bytes_installed_{0};
    std::atomic<uint64_t>     payload_consumed_{0}; ///< Сжатых байт, вычитанных из ring buffer (база pacing, для NSZ не равна bytes_installed_)
    std::atomic<uint64_t>     total_bytes_{0};
    std::atomic<uint32_t>     starvation_count_{0}; ///< Число событий голодания буфера (rb_avail=0, wait>=500ms)

    // Данные NSP-заголовка (хранятся для тикетов/CNMT)
    std::vector<uint8_t> header_data_;

    // Данные тикета и сертификата (буферизованные)
    std::vector<uint8_t> ticket_data_;
    std::vector<uint8_t> cert_data_;

    // Текущий NCA, в который идёт запись
    size_t   current_nca_index_  = 0;
    uint64_t current_nca_offset_ = 0;

    std::string error_message_;
    uint64_t    hint_title_id_ = 0; ///< Fallback Title ID из имени файла
    
    // Speed calculation
    uint64_t last_bytes_ = 0;
    uint64_t last_time_  = 0;
    double   current_speed_kbps_ = 0;

    // Данные CNMT (парсятся из CNMT NCA)
    CnmtData   cnmt_data_;
    std::vector<uint8_t> cnmt_nca_data_;  ///< Буферизованный CNMT NCA

#ifdef __SWITCH__
    Thread collector_thread_ = {};
    Thread installer_thread_ = {};
    bool   threads_started_  = false;
#endif
};

} // namespace installer
