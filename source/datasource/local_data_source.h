#pragma once
// =============================================================================
// LocalDataSource — работа с локальным TorrServer, запущенным на Switch.
// По сути тот же HTTP-протокол, но на localhost.
// Включает логику запуска/остановки серверного процесса.
// =============================================================================

#include "i_data_source.h"
#include "remote_data_source.h"

#include <string>

namespace datasource {

class LocalDataSource : public IDataSource {
public:
    /// @param port  порт локального TorrServer/TorrentEngine (по умолчанию 8080)
    explicit LocalDataSource(int port = 8080);
    ~LocalDataSource() override;

    bool open(const std::string& torrent_hash, int file_index) override;
    size_t read(uint64_t offset, void* buf, size_t size) override;
    uint64_t totalSize() const override;
    bool isAvailable() const override;
    SourceType type() const override { return SourceType::LocalInternal; }
    void close() override;

    /// Запустить локальный TorrServer.
    /// @return true если сервер успешно запущен
    bool startServer();

    /// Остановить локальный TorrServer
    void stopServer();

    /// Проверить, запущен ли сервер
    bool isServerRunning() const;

    /// Проверить, достаточно ли памяти для локального сервера.
    /// В аплет-режиме (~442MB) — недостаточно.
    static bool hasEnoughMemory();

    /// Получить тип аплета (для предупреждения пользователю)
    static bool isAppletMode();

private:
    int port_;
    bool server_started_ = false;

    /// Внутренний RemoteDataSource на localhost
    RemoteDataSource local_remote_;
};

} // namespace datasource
