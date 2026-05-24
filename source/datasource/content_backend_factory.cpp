#include "i_content_backend.h"

#include "external_torrserver_backend.h"

#ifdef TSNX_USE_LIBTORRENT
#include "local_libtorrent_backend.h"
#endif

namespace datasource {

const char* streamStateName(StreamState state) {
    switch (state) {
        case StreamState::Idle: return "Idle";
        case StreamState::FetchingMetadata: return "FetchingMetadata";
        case StreamState::FileSelected: return "FileSelected";
        case StreamState::PrebufferInstallInfo: return "PrebufferInstallInfo";
        case StreamState::InstallInfoParsed: return "InstallInfoParsed";
        case StreamState::MainBuffering: return "MainBuffering";
        case StreamState::StreamingOrInstalling: return "StreamingOrInstalling";
        case StreamState::Stalled: return "Stalled";
        case StreamState::Completed: return "Completed";
        case StreamState::Stopping: return "Stopping";
        case StreamState::Error: return "Error";
    }
    return "Unknown";
}

std::unique_ptr<IContentBackend> create_backend(BackendType type, const BackendConfig& cfg) {
    switch (type) {
        case BackendType::ExternalTorrServer:
            return std::make_unique<ExternalTorrServerBackend>(cfg);
        case BackendType::LocalLibtorrent:
#ifdef TSNX_USE_LIBTORRENT
            return std::make_unique<LocalLibtorrentBackend>(cfg);
#else
            return nullptr;  // libtorrent не собран
#endif
    }
    return nullptr;
}

} // namespace datasource
