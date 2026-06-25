#pragma once

#include <functional>
#include <string>

#include <catalog/QueueItem.hpp>
#include <platform/AuthorizedDownloadProvider.hpp>
#include <platform/InstallConnectorBridge.hpp>

namespace shield::platform {

struct QueueDownloadResult {
    shield::catalog::QueueItem item;
    bool success = false;
    bool started = false;
    bool install_bridge_deferred = false;
    std::string message;
};

class QueueDownloadWorker {
    public:
        static QueueDownloadResult Run(const shield::catalog::QueueItem &item,
            const std::string &downloads_root,
            const InstallBridgeConfig &bridge_config,
            std::function<void(const DownloadProgress &)> progress_callback = {},
            InstallProgressCallback install_progress_callback = {},
            std::function<DownloadStopReason()> stop_callback = {});
};

}
