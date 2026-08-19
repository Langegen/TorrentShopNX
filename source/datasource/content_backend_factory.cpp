#include "i_content_backend.h"

#include "external_torrserver_backend.h"
#include "custom_engine_backend.h"

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
            return nullptr;  // legacy backend removed
        case BackendType::CustomEngine:
            return std::make_unique<CustomEngineBackend>(cfg);
    }
    return nullptr;
}

} // namespace datasource
