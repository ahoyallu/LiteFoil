#include <platform/QueueDownloadWorker.hpp>

#include <cstdio>
#include <sys/stat.h>

#include <platform/InstallConnectorBridge.hpp>
#include <platform/ExitLog.hpp>
#include <platform/RcloneCrypt.hpp>
#include <platform/SystemStatus.hpp>

namespace shield::platform {

namespace {

constexpr std::uint64_t kFat32MaxFileSize = 0xFFFFFFFFULL;

}

// Check whether a previously downloaded file is still on disk and complete.
static bool IsFileAlreadyComplete(const std::string &path, std::uint64_t expected_size) {
    if(path.empty()) return false;
    struct stat st{};
    if(stat(path.c_str(), &st) != 0) return false;
    if(st.st_size <= 0) return false;
    // If expected_size is unknown (0), any non-empty file counts as complete.
    return expected_size == 0 || static_cast<std::uint64_t>(st.st_size) >= expected_size;
}

static bool IsStreamInstallModel(const shield::catalog::QueueItem &item) {
    return item.installation_model == "stream";
}

static void ForceStreamInstall(shield::catalog::QueueItem &item) {
    item.installation_model = "stream";
    item.keep_download = false;
    item.delete_after_download = true;
    item.local_path.clear();
}

static std::uint64_t AddPercentCeil(const std::uint64_t value, const std::uint64_t percent) {
    return value + ((value * percent) + 99) / 100;
}

static std::uint64_t RequiredInstallBytes(const shield::catalog::QueueItem &item) {
    return item.size == 0 ? 0 : AddPercentCeil(item.size, 30);
}

static std::uint64_t RequiredDirectSdBytes(const shield::catalog::QueueItem &item) {
    if(item.size == 0) {
        return 0;
    }
    if(item.target_location == "NAND") {
        return item.size;
    }
    return item.size + RequiredInstallBytes(item);
}

static std::string BuildSpaceMessage(const std::string &storage_name, const std::uint64_t free_bytes, const std::uint64_t required_bytes) {
    return "Not enough space on " + storage_name
        + " (" + SystemStatus::FormatStorageAmount(free_bytes)
        + " free, need " + SystemStatus::FormatStorageAmount(required_bytes) + ")";
}

static bool PrepareDirectInstallOrFallback(shield::catalog::QueueItem &item, std::string &failure_message) {
    if(IsStreamInstallModel(item)) {
        return true;
    }

    if(item.size > kFat32MaxFileSize) {
        RuntimeLog("[queue] direct forced to stream by FAT32 file size limit title_id=%s size=%llu limit=%llu",
            item.title_id.c_str(),
            static_cast<unsigned long long>(item.size),
            static_cast<unsigned long long>(kFat32MaxFileSize));
        ForceStreamInstall(item);
        return true;
    }

    if(item.size == 0) {
        return true;
    }

    const auto overview = SystemStatus::ReadOverview();
    const auto &dest_storage = (item.target_location == "NAND") ? overview.nand : overview.sd_card;
    const std::uint64_t install_required = RequiredInstallBytes(item);
    if(dest_storage.available && (install_required > dest_storage.free_bytes)) {
        failure_message = BuildSpaceMessage(item.target_location == "NAND" ? "NAND" : "SD", dest_storage.free_bytes, install_required);
        RuntimeLog("[queue] direct blocked by destination space title_id=%s target=%s free=%llu required=%llu",
            item.title_id.c_str(), item.target_location.c_str(),
            static_cast<unsigned long long>(dest_storage.free_bytes),
            static_cast<unsigned long long>(install_required));
        return false;
    }

    const auto &sd_storage = overview.sd_card;
    const std::uint64_t direct_required = RequiredDirectSdBytes(item);
    if(sd_storage.available && (direct_required > sd_storage.free_bytes)) {
        RuntimeLog("[queue] direct fallback to stream title_id=%s sd_free=%llu direct_required=%llu install_required=%llu size=%llu",
            item.title_id.c_str(),
            static_cast<unsigned long long>(sd_storage.free_bytes),
            static_cast<unsigned long long>(direct_required),
            static_cast<unsigned long long>(install_required),
            static_cast<unsigned long long>(item.size));
        ForceStreamInstall(item);
    }

    return true;
}

QueueDownloadResult QueueDownloadWorker::Run(const shield::catalog::QueueItem &item,
    const std::string &downloads_root,
    const InstallBridgeConfig &bridge_config,
    std::function<void(const DownloadProgress &)> progress_callback,
    InstallProgressCallback install_progress_callback,
    std::function<DownloadStopReason()> stop_callback) {
    QueueDownloadResult result;
    result.item = item;
    shield::catalog::QueueItem effective_item = item;
    ConfigureRcloneCrypt(bridge_config.rclone_crypt);
    RuntimeLog("[queue] start title_id=%s name=%s install_model=%s keep_download=%d delete_after=%d rclone=%d size=%llu",
        item.title_id.c_str(), item.name.c_str(), item.installation_model.c_str(),
        static_cast<int>(item.keep_download), static_cast<int>(item.delete_after_download),
        static_cast<int>(bridge_config.rclone_crypt.enabled),
        static_cast<unsigned long long>(item.size));

    // Leave provider resolution at the worker boundary so the UI can queue items
    // freely while download capability stays centralized and easy to swap later.
    if(!AuthorizedDownloadProvider::CanHandle(item)) {
        result.message = "No authorized provider for this source";
        result.item.state = shield::catalog::QueueItemState::Failed;
        result.item.last_error = result.message;
        return result;
    }

    std::string space_failure;
    if(!PrepareDirectInstallOrFallback(effective_item, space_failure)) {
        result.message = space_failure;
        result.item.state = shield::catalog::QueueItemState::Failed;
        result.item.last_error = result.message;
        return result;
    }
    result.item = effective_item;

    if(effective_item.size > kFat32MaxFileSize && !IsStreamInstallModel(effective_item)) {
        result.message = "Files over 4 GiB must be installed by stream";
        result.item.state = shield::catalog::QueueItemState::Failed;
        result.item.last_error = result.message;
        return result;
    }

    // If a previous attempt already downloaded the file, skip straight to install
    // instead of re-downloading the exact same bytes.
    if(!IsStreamInstallModel(effective_item) && IsFileAlreadyComplete(effective_item.local_path, effective_item.size)) {
        result.started = true;
        result.item.state = shield::catalog::QueueItemState::Installing;
        result.item.bytes_done = effective_item.size;
        result.item.bytes_total = effective_item.size;

        const auto bridge_result = InstallConnectorBridge::HandleDownloadedItem(result.item, bridge_config, std::move(install_progress_callback));
        if(bridge_result.decision != InstallConnectorDecision::Succeeded) {
            result.item.state = shield::catalog::QueueItemState::Failed;
            result.item.last_error = bridge_result.message;
            result.item.retry_count++;
            result.message = bridge_result.message;
            return result;
        }

        result.success = true;
        result.item.state = shield::catalog::QueueItemState::Completed;
        result.item.last_error.clear();
        result.install_bridge_deferred = (bridge_result.decision == InstallConnectorDecision::Deferred);
        result.message = bridge_result.message.empty() ? "Installed successfully" : bridge_result.message;
        return result;
    }

    // Stream install bypasses disk entirely and installs each NCA directly from
    // HTTP range requests into NCM content storage.
    if(IsStreamInstallModel(effective_item)) {
        result.started = true;
        result.item.state = shield::catalog::QueueItemState::Installing;
        const auto bridge_result = InstallConnectorBridge::HandleStreamInstall(
            result.item, bridge_config, install_progress_callback, stop_callback);
        if(bridge_result.decision != InstallConnectorDecision::Succeeded) {
            const auto stop_reason = stop_callback ? stop_callback() : DownloadStopReason::None;
            if(stop_reason == DownloadStopReason::Paused) {
                result.item.state = shield::catalog::QueueItemState::Paused;
                result.item.auto_start = false;
                result.item.bytes_per_second = 0.0;
                result.item.last_error.clear();
                result.message = "Download paused";
                return result;
            }
            if(stop_reason == DownloadStopReason::Canceled) {
                result.item.state = shield::catalog::QueueItemState::Canceled;
                result.item.auto_start = false;
                result.item.bytes_done = 0;
                result.item.bytes_per_second = 0.0;
                result.item.last_error.clear();
                result.item.local_path.clear();
                result.message = "Download canceled";
                return result;
            }
            result.item.state = shield::catalog::QueueItemState::Failed;
            result.item.last_error = bridge_result.message;
            result.item.retry_count++;
            result.message = bridge_result.message;
            return result;
        }
        result.success = true;
        result.item.state = shield::catalog::QueueItemState::Completed;
        result.item.bytes_done = result.item.size;
        result.item.bytes_total = result.item.size;
        result.item.bytes_per_second = 0.0;
        result.item.last_error.clear();
        result.install_bridge_deferred = (bridge_result.decision == InstallConnectorDecision::Deferred);
        result.message = bridge_result.message.empty() ? "Stream install completed" : bridge_result.message;
        return result;
    }

    result.started = true;
    result.item.state = shield::catalog::QueueItemState::Downloading;
    const auto download_result = AuthorizedDownloadProvider::Download(effective_item, downloads_root, std::move(progress_callback), std::move(stop_callback));
    result.item.bytes_done = download_result.bytes_done;
    result.item.bytes_total = download_result.bytes_total;
    result.item.bytes_per_second = download_result.bytes_per_second;
    result.item.local_path = download_result.output_path;

    if(download_result.success) {
        result.item.state = shield::catalog::QueueItemState::Installing;
        const auto bridge_result = InstallConnectorBridge::HandleDownloadedItem(result.item, bridge_config, std::move(install_progress_callback));
        if(bridge_result.decision != InstallConnectorDecision::Succeeded) {
            result.item.state = shield::catalog::QueueItemState::Failed;
            result.item.last_error = bridge_result.message;
            result.item.retry_count++;
            result.message = bridge_result.message;
            return result;
        }

        result.success = true;
        result.item.state = shield::catalog::QueueItemState::Completed;
        result.item.last_error.clear();
        result.install_bridge_deferred = (bridge_result.decision == InstallConnectorDecision::Deferred);
        result.message = bridge_result.message.empty() ? "Downloaded successfully" : bridge_result.message;
        return result;
    }

    result.success = false;
    if(download_result.stop_reason == DownloadStopReason::Paused) {
        result.item.state = shield::catalog::QueueItemState::Paused;
        result.item.auto_start = false;
        result.item.bytes_per_second = 0.0;
        result.item.last_error.clear();
        result.message = download_result.error_message;
        return result;
    }

    if(download_result.stop_reason == DownloadStopReason::Canceled) {
        result.item.state = shield::catalog::QueueItemState::Canceled;
        result.item.auto_start = false;
        result.item.last_error.clear();
        result.item.bytes_done = 0;
        result.item.bytes_per_second = 0.0;
        result.item.local_path.clear();
        result.message = download_result.error_message;
        return result;
    }

    result.item.state = shield::catalog::QueueItemState::Failed;
    result.item.retry_count++;
    result.item.last_error = download_result.error_message;
    result.message = download_result.error_message;
    return result;
}

}
