#pragma once

#include <functional>
#include <string>

#include <catalog/QueueItem.hpp>
#include <install/InstallEngine.hpp>
#include <platform/AuthorizedDownloadProvider.hpp>
#include <platform/RcloneCrypt.hpp>

namespace shield::platform {

enum class InstallConnectorDecision : unsigned char {
    NoConnector = 0,
    Succeeded,
    Deferred,
    Failed
};

using InstallProgressCallback = std::function<void(const shield::install::InstallProgress &)>;

struct InstallConnectorResult {
    InstallConnectorDecision decision = InstallConnectorDecision::NoConnector;
    std::string message;
    std::uint64_t installed_title_id = 0;
};

// Runtime settings forwarded from AppConfig that the install bridge needs.
struct InstallBridgeConfig {
    bool allow_unsigned_sources = false;
    RcloneCryptConfig rclone_crypt;
};

class InstallConnectorBridge {
    public:
        static InstallConnectorResult HandleDownloadedItem(
            const shield::catalog::QueueItem &item,
            const InstallBridgeConfig &bridge_config,
            InstallProgressCallback install_progress_callback = {});

        // Install directly from the source URL without writing to SD card.
        // Uses HTTP range requests to stream each NCA into NCM content storage.
        static InstallConnectorResult HandleStreamInstall(
            const shield::catalog::QueueItem &item,
            const InstallBridgeConfig &bridge_config,
            InstallProgressCallback install_progress_callback = {},
            std::function<DownloadStopReason()> stop_fn = {});
};

}
