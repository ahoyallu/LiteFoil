#include <platform/InstallConnectorBridge.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include <install/InstallEngine.hpp>
#include <platform/ExitLog.hpp>
#include <platform/SystemStatus.hpp>

namespace shield::platform {
namespace {

std::uint64_t RequiredInstallBytes(const std::uint64_t package_size) {
    if(package_size == 0) {
        return 0;
    }
    return package_size + ((package_size * 30) + 99) / 100;
}

std::string BuildNotEnoughSpaceMessage(const std::string &target_location, const std::uint64_t free_bytes, const std::uint64_t required_bytes) {
    return "Not enough space on " + target_location
        + " (" + SystemStatus::FormatStorageAmount(free_bytes)
        + " free, need " + SystemStatus::FormatStorageAmount(required_bytes) + ")";
}

class StreamDownloadProgressAggregator {
public:
    explicit StreamDownloadProgressAggregator(std::uint64_t total)
        : total_(total),
          last_sample_at_(std::chrono::steady_clock::now()),
          last_receive_at_(this->last_sample_at_) {
    }

    void AddBytes(std::size_t len) {
        const auto now = std::chrono::steady_clock::now();
        this->downloaded_ += static_cast<std::uint64_t>(len);
        this->last_receive_at_ = now;

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - this->last_sample_at_).count();
        if(elapsed_ms >= 500) {
            const std::uint64_t delta = this->downloaded_ - this->last_sample_bytes_;
            this->bytes_per_second_ = (static_cast<double>(delta) * 1000.0)
                / static_cast<double>(elapsed_ms);
            this->last_sample_at_ = now;
            this->last_sample_bytes_ = this->downloaded_;
        }
    }

    shield::install::InstallProgress Apply(shield::install::InstallProgress progress) const {
        if(this->total_ > 0) {
            progress.bytes_done = std::min(this->downloaded_, this->total_);
            progress.bytes_total = this->total_;
        }
        else if(this->downloaded_ > 0) {
            progress.bytes_done = this->downloaded_;
        }

        progress.bytes_per_second = this->CurrentSpeed();
        return progress;
    }

private:
    double CurrentSpeed() const {
        const auto now = std::chrono::steady_clock::now();
        const auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - this->last_receive_at_).count();
        return idle_ms > 2000 ? 0.0 : this->bytes_per_second_;
    }

    std::uint64_t total_ = 0;
    std::uint64_t downloaded_ = 0;
    std::chrono::steady_clock::time_point last_sample_at_;
    std::chrono::steady_clock::time_point last_receive_at_;
    std::uint64_t last_sample_bytes_ = 0;
    double bytes_per_second_ = 0.0;
};

class StreamRangeReadCache {
public:
    StreamRangeReadCache(shield::install::PackageRangeReader upstream, std::uint64_t total_size)
        : upstream_(std::move(upstream)), total_size_(total_size) {
    }

    bool Read(std::uint64_t offset,
              std::uint64_t size,
              const std::function<bool(const void *, std::size_t)> &write_fn) {
        if(size == 0) {
            return this->upstream_(offset, size, write_fn);
        }

        std::uint64_t served = 0;
        if(this->ServeCachedPrefix(offset, size, write_fn, served)) {
            if(served == size) {
                return true;
            }
            offset += served;
            size -= served;
        }

        if(size <= kCacheableRequestSize) {
            if(!this->FillCache(offset, size)) {
                return false;
            }
            served = 0;
            return this->ServeCachedPrefix(offset, size, write_fn, served) && (served == size);
        }

        return this->upstream_(offset, size, write_fn);
    }

private:
    static constexpr std::uint64_t kPrefetchWindowSize = 512 * 1024;
    static constexpr std::uint64_t kCacheableRequestSize = 256 * 1024;

    bool ServeCachedPrefix(std::uint64_t offset,
                           std::uint64_t size,
                           const std::function<bool(const void *, std::size_t)> &write_fn,
                           std::uint64_t &served) const {
        served = 0;
        if(!this->cache_valid_ || this->cache_.empty()) {
            return true;
        }

        const std::uint64_t cache_end = this->cache_offset_ + this->cache_.size();
        if(offset < this->cache_offset_ || offset >= cache_end) {
            return true;
        }

        const std::uint64_t cache_pos = offset - this->cache_offset_;
        const std::uint64_t available = cache_end - offset;
        served = std::min<std::uint64_t>(size, available);
        if(served == 0) {
            return true;
        }

        return write_fn(this->cache_.data() + cache_pos, static_cast<std::size_t>(served));
    }

    bool FillCache(std::uint64_t offset, std::uint64_t requested_size) {
        std::uint64_t fetch_size = std::max<std::uint64_t>(requested_size, kPrefetchWindowSize);
        if(this->total_size_ > 0 && offset < this->total_size_) {
            fetch_size = std::min<std::uint64_t>(fetch_size, this->total_size_ - offset);
        }

        std::vector<std::uint8_t> next_cache;
        next_cache.reserve(static_cast<std::size_t>(fetch_size));
        const bool ok = this->upstream_(offset, fetch_size,
            [&next_cache](const void *data, std::size_t len) -> bool {
                const auto *bytes = static_cast<const std::uint8_t *>(data);
                next_cache.insert(next_cache.end(), bytes, bytes + len);
                return true;
            });
        if(!ok || next_cache.size() < requested_size) {
            this->cache_valid_ = false;
            this->cache_.clear();
            return false;
        }

        this->cache_offset_ = offset;
        this->cache_ = std::move(next_cache);
        this->cache_valid_ = true;
        RuntimeLog("[range-cache] fill offset=%llu requested=%llu cached=%zu",
            static_cast<unsigned long long>(offset),
            static_cast<unsigned long long>(requested_size),
            this->cache_.size());
        return true;
    }

    shield::install::PackageRangeReader upstream_;
    std::uint64_t total_size_ = 0;
    bool cache_valid_ = false;
    std::uint64_t cache_offset_ = 0;
    std::vector<std::uint8_t> cache_;
};

}

InstallConnectorResult InstallConnectorBridge::HandleDownloadedItem(
    const shield::catalog::QueueItem &item,
    const InstallBridgeConfig &bridge_config,
    InstallProgressCallback install_progress_callback) {
    InstallConnectorResult result;
    RuntimeLog("[bridge] local install path=%s size=%llu target=%s",
        item.local_path.c_str(), static_cast<unsigned long long>(item.size),
        item.target_location.c_str());

    if(item.local_path.empty()) {
        result.decision = InstallConnectorDecision::NoConnector;
        result.message  = "No local file to install";
        return result;
    }

    std::string ext;
    const auto dot = item.local_path.find_last_of('.');
    if(dot != std::string::npos) {
        ext = item.local_path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    if(ext != ".nsp" && ext != ".nsz" && ext != ".xci" && ext != ".xcz") {
        result.decision = InstallConnectorDecision::NoConnector;
        result.message  = "Unsupported format for auto-install: " + ext;
        return result;
    }

    const bool target_nand = (item.target_location == "NAND");
    const NcmStorageId dest_storage = target_nand ? NcmStorageId_BuiltInUser
                                                  : NcmStorageId_SdCard;

    if(item.size > 0) {
        const auto overview = SystemStatus::ReadOverview();
        const auto &storage = target_nand ? overview.nand : overview.sd_card;
        const std::uint64_t required_bytes = RequiredInstallBytes(item.size);
        if(storage.available && (required_bytes > storage.free_bytes)) {
            result.decision = InstallConnectorDecision::Failed;
            result.message  = BuildNotEnoughSpaceMessage(item.target_location, storage.free_bytes, required_bytes);
            return result;
        }
    }

    shield::install::InstallConfig install_config;
    install_config.dest_storage_id  = dest_storage;
    install_config.verify_nca_sigs  = item.verify_integrity;
    install_config.allow_unsigned   = bridge_config.allow_unsigned_sources;
    install_config.ignore_req_fw    = true;   // always reset for base installs
    install_config.reinstall_ncas   = item.reinstall_ncas;

    const auto install_result = shield::install::InstallFromLocalFile(
        item.local_path, install_config, std::move(install_progress_callback));

    if(install_result.success) {
        result.decision           = InstallConnectorDecision::Succeeded;
        result.installed_title_id = install_result.title_id;
        result.message            = "Installed successfully";

        if(item.delete_after_download && !item.local_path.empty()) {
            std::remove(item.local_path.c_str());
        }
    }
    else {
        result.decision = InstallConnectorDecision::Failed;
        result.message  = install_result.error_message;
    }

    return result;
}

InstallConnectorResult InstallConnectorBridge::HandleStreamInstall(
    const shield::catalog::QueueItem &item,
    const InstallBridgeConfig &bridge_config,
    InstallProgressCallback install_progress_callback,
    std::function<DownloadStopReason()> stop_fn) {

    InstallConnectorResult result;
    RuntimeLog("[bridge] stream install url=%s size=%llu target=%s rclone=%d",
        item.source_url.c_str(), static_cast<unsigned long long>(item.size),
        item.target_location.c_str(), static_cast<int>(bridge_config.rclone_crypt.enabled));

    if(item.source_url.empty()) {
        result.decision = InstallConnectorDecision::NoConnector;
        result.message  = "No source URL for stream install";
        return result;
    }

    std::string ext;
    {
        std::string url_path = item.source_url;
        const auto q = url_path.find('?');
        if(q != std::string::npos) url_path = url_path.substr(0, q);
        const auto dot   = url_path.find_last_of('.');
        const auto slash = url_path.find_last_of('/');
        if(dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            ext = url_path.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
    }
    if(ext.empty() && !item.package_format.empty()) {
        ext = "." + item.package_format;
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    if(ext != ".nsp" && ext != ".nsz" && ext != ".xci" && ext != ".xcz") {
        result.decision = InstallConnectorDecision::NoConnector;
        result.message  = "Unsupported format for stream install: " + ext;
        return result;
    }

    const bool target_nand = (item.target_location == "NAND");
    const NcmStorageId dest_storage = target_nand ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
    if(item.size > 0) {
        const auto overview = SystemStatus::ReadOverview();
        const auto &storage_info = target_nand ? overview.nand : overview.sd_card;
        const std::uint64_t required_bytes = RequiredInstallBytes(item.size);
        if(storage_info.available && (required_bytes > storage_info.free_bytes)) {
            result.decision = InstallConnectorDecision::Failed;
            result.message  = BuildNotEnoughSpaceMessage(item.target_location, storage_info.free_bytes, required_bytes);
            return result;
        }
    }

    shield::install::InstallConfig install_config;
    install_config.dest_storage_id = dest_storage;
    install_config.verify_nca_sigs = item.verify_integrity;
    install_config.allow_unsigned  = bridge_config.allow_unsigned_sources;
    install_config.ignore_req_fw   = true;
    install_config.reinstall_ncas  = item.reinstall_ncas;

    StreamDownloadProgressAggregator stream_progress(item.size);

    const std::string url = item.source_url;
    shield::install::PackageRangeReader raw_reader =
        [url, stop_fn, &stream_progress](std::uint64_t offset, std::uint64_t size,
                         std::function<bool(const void*, std::size_t)> write_fn) -> bool {
            return AuthorizedDownloadProvider::DownloadRange(
                url, offset, size,
                [&stream_progress, write_fn = std::move(write_fn)](const void *data, std::size_t len) mutable -> bool {
                    stream_progress.AddBytes(len);
                    return write_fn(data, len);
                },
                stop_fn);
        };
    StreamRangeReadCache range_cache(std::move(raw_reader), item.size);
    shield::install::PackageRangeReader reader =
        [&range_cache](std::uint64_t offset, std::uint64_t size,
                       std::function<bool(const void *, std::size_t)> write_fn) -> bool {
            return range_cache.Read(offset, size, write_fn);
        };

    InstallProgressCallback aggregated_progress_callback;
    if(install_progress_callback) {
        aggregated_progress_callback = [&stream_progress, install_progress_callback = std::move(install_progress_callback)](
            const shield::install::InstallProgress &progress) mutable {
            install_progress_callback(stream_progress.Apply(progress));
        };
    }

    // Run the stream install.
    shield::install::InstallResult install_result;
    auto install_stop_callback = [stop_fn]() {
        return stop_fn && (stop_fn() != DownloadStopReason::None);
    };
    if(ext == ".nsp" || ext == ".nsz") {
        install_result = shield::install::InstallNspFromReader(
            reader, install_config, std::move(aggregated_progress_callback), install_stop_callback);
    } else {
        install_result = shield::install::InstallXciFromReader(
            reader, install_config, std::move(aggregated_progress_callback), install_stop_callback);
    }

    if(install_result.success) {
        result.decision           = InstallConnectorDecision::Succeeded;
        result.installed_title_id = install_result.title_id;
        result.message            = "Stream install succeeded";
    } else {
        result.decision = InstallConnectorDecision::Failed;
        result.message  = install_result.error_message;
    }
    return result;
}

}
