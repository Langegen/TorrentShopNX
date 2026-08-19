#include "data_source_manager.h"

#include "backend_data_source.h"
#include "custom_engine_client.h"
// TorrestDataSource is disabled; using high-performance CustomEngineBackend
#include "../utils/log.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <utility>

namespace datasource {

namespace {

bool hasEnoughMemoryForEngine() {
#ifdef __SWITCH__
    size_t total = 0, used = 0;
    if (R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) &&
        R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0))) {
        // Require at least 256 MB free for the custom engine + installer buffers.
        size_t avail = total > used ? total - used : 0;
        return avail >= (256ULL * 1024 * 1024);
    }
#endif
    return true;
}

bool detectAppletMode() {
#ifdef __SWITCH__
    AppletType at = appletGetAppletType();
    return at == AppletType_LibraryApplet || at == AppletType_OverlayApplet;
#endif
    return false;
}

} // namespace

DataSourceManager::DataSourceManager() {
    remote_cfg_.remote_url = remote_url_;
    remote_cfg_.timeout_sec = 30;
    remote_cfg_.retry_count = 6;

    custom_cfg_.local_port = local_port_;
    custom_cfg_.timeout_sec = 90;
    custom_cfg_.retry_count = 2;

    remote_ = std::make_unique<BackendDataSource>(BackendType::ExternalTorrServer, remote_cfg_);
    custom_engine_ = std::make_unique<BackendDataSource>(BackendType::CustomEngine, custom_cfg_);
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
    if (local_port_ == port && custom_engine_) {
        return;
    }
    local_port_ = port;

    custom_cfg_.local_port = local_port_;
    custom_engine_ = std::make_unique<BackendDataSource>(BackendType::CustomEngine, custom_cfg_);
    util::logLine("ds_mgr: custom engine port: " + std::to_string(local_port_));
}

IDataSource* DataSourceManager::getSource() {
    if (mode_ == DataSourceMode::LocalClient || mode_ == DataSourceMode::CustomEngine) {
        return custom_engine_.get();
    }

    return remote_.get();
}

bool DataSourceManager::isLocalServerAvailable() const {
    return CustomEngineClient::instance().isEnabled();
}

bool DataSourceManager::hasEnoughMemoryForLocal() const {
    return hasEnoughMemoryForEngine();
}

bool DataSourceManager::isAppletMode() const {
    return detectAppletMode();
}

std::string DataSourceManager::modeDescription() const {
    switch (mode_) {
        case DataSourceMode::Remote:       return "TorrServer";
        case DataSourceMode::LocalClient:  return "Custom engine";
        case DataSourceMode::CustomEngine: return "Custom engine";
    }
    return "Unknown";
}

} // namespace datasource
