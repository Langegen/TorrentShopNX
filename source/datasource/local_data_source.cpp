#include "local_data_source.h"

#include "../torrent/torrent_engine.h"
#include "../utils/log.h"

#include <string>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace datasource {

LocalDataSource::LocalDataSource(int port)
    : port_(port)
    , local_remote_("http://127.0.0.1:" + std::to_string(port)) {
}

LocalDataSource::~LocalDataSource() {
    close();
    stopServer();
}

bool LocalDataSource::startServer() {
    if (server_started_) {
        util::logLine("local_ds: server is already running");
        return true;
    }

    if (!hasEnoughMemory()) {
        util::logLine("local_ds: not enough memory for local TorrServer");
        util::logLine("local_ds: launch app in full mode (hold R on a game)");
        return false;
    }

    if (!torrent::TorrentEngine::instance().start(port_)) {
        util::logLine("local_ds: local engine failed to start: " + torrent::TorrentEngine::instance().lastError());
        return false;
    }

    local_remote_ = RemoteDataSource(torrent::TorrentEngine::instance().serverUrl());
    server_started_ = true;
    util::logLine("local_ds: local TorrentEngine is available on port " + std::to_string(port_));
    return true;
}

void LocalDataSource::stopServer() {
    if (!server_started_) return;

    torrent::TorrentEngine::instance().stop();
    server_started_ = false;
    util::logLine("local_ds: server stopped");
}

bool LocalDataSource::isServerRunning() const {
    return server_started_;
}

bool LocalDataSource::hasEnoughMemory() {
#ifdef __SWITCH__
    AppletType type = appletGetAppletType();
    if (type == AppletType_LibraryApplet || type == AppletType_OverlayApplet) {
        return false;
    }
    return true;
#else
    return true;
#endif
}

bool LocalDataSource::isAppletMode() {
#ifdef __SWITCH__
    AppletType type = appletGetAppletType();
    return (type == AppletType_LibraryApplet || type == AppletType_OverlayApplet);
#else
    return false;
#endif
}

bool LocalDataSource::open(const std::string& torrent_hash, int file_index) {
    if (!server_started_ && !startServer()) {
        return false;
    }
    return local_remote_.open(torrent_hash, file_index);
}

size_t LocalDataSource::read(uint64_t offset, void* buf, size_t size) {
    return local_remote_.read(offset, buf, size);
}

uint64_t LocalDataSource::totalSize() const {
    return local_remote_.totalSize();
}

bool LocalDataSource::isAvailable() const {
    return local_remote_.isAvailable();
}

void LocalDataSource::close() {
    local_remote_.close();
}

} // namespace datasource
