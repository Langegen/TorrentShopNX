#include "data_source_manager.h"

#include "backend_data_source.h"
#include "internal_torrent_engine.h"
#include "local_data_source.h"
// TorrestDataSource is disabled; using high-performance LocalLibtorrentBackend
#include "../utils/log.h"

#include <utility>

namespace datasource {

DataSourceManager::DataSourceManager() {
    remote_cfg_.remote_url = remote_url_;
    remote_cfg_.timeout_sec = 30;
    remote_cfg_.retry_count = 6;

    local_cfg_.remote_url = local_proxy_url_;
    local_cfg_.local_port = local_port_;
    local_cfg_.timeout_sec = 90;
    local_cfg_.retry_count = 2;

    remote_ = std::make_unique<BackendDataSource>(BackendType::ExternalTorrServer, remote_cfg_);
    local_client_ = std::make_unique<BackendDataSource>(BackendType::LocalLibtorrent, local_cfg_);
}

void DataSourceManager::setMode(DataSourceMode mode) {
    mode_ = mode;
    util::logLine("ds_mgr: mode set to: " + modeDescription());
}

void DataSourceManager::setRemoteUrl(const std::string& url) {
    if (remote_url_ == url && remote_) {
        return;
    }
    remote_url_ = url;
    remote_cfg_.remote_url = remote_url_;
    if (remote_) {
        remote_->setConfig(remote_cfg_);
    } else {
        remote_ = std::make_unique<BackendDataSource>(BackendType::ExternalTorrServer, remote_cfg_);
    }
    util::logLine("ds_mgr: remote URL: " + remote_url_);
}

void DataSourceManager::setLocalPort(int port) {
    if (local_port_ == port && local_client_) {
        return;
    }
    local_port_ = port;
    local_proxy_url_ = "http://127.0.0.1:" + std::to_string(local_port_);
    local_cfg_.remote_url = local_proxy_url_;
    local_cfg_.local_port = local_port_;
    local_client_ = std::make_unique<BackendDataSource>(BackendType::LocalLibtorrent, local_cfg_);
    util::logLine("ds_mgr: local port: " + std::to_string(local_port_));
}

IDataSource* DataSourceManager::getSource() {
    if (mode_ == DataSourceMode::LocalClient) {
        return local_client_.get();
    }

    return remote_.get();
}

bool DataSourceManager::isLocalServerAvailable() const {
    return InternalTorrentEngine::instance().isEnabled();
}

bool DataSourceManager::hasEnoughMemoryForLocal() const {
    return LocalDataSource::hasEnoughMemory();
}

bool DataSourceManager::isAppletMode() const {
    return LocalDataSource::isAppletMode();
}

std::string DataSourceManager::modeDescription() const {
    switch (mode_) {
        case DataSourceMode::Remote:      return "TorrServer";
        case DataSourceMode::LocalClient: return "Local client";
    }
    return "Unknown";
}

} // namespace datasource
