#pragma once

#include "backend_data_source.h"
#include "i_data_source.h"

#include <memory>
#include <string>

namespace datasource {

enum class DataSourceMode {
    Remote,      // External TorrServer
    LocalClient  // Built-in local torrent client
};

class DataSourceManager {
public:
    DataSourceManager();
    ~DataSourceManager() = default;

    void setMode(DataSourceMode mode);
    DataSourceMode mode() const { return mode_; }

    void setRemoteUrl(const std::string& url);
    std::string remoteUrl() const { return remote_url_; }

    void setLocalPort(int port);

    IDataSource* getSource();

    bool isLocalServerAvailable() const;
    bool hasEnoughMemoryForLocal() const;
    bool isAppletMode() const;

    std::string modeDescription() const;

private:
    DataSourceMode mode_ = DataSourceMode::LocalClient;
    std::string remote_url_ = "http://127.0.0.1:8090";
    std::string local_proxy_url_ = "http://127.0.0.1:8080";
    int local_port_ = 8080;

    BackendConfig remote_cfg_{};
    BackendConfig local_cfg_{};
    std::unique_ptr<BackendDataSource> remote_;
    std::unique_ptr<IDataSource> local_client_;
};

} // namespace datasource
