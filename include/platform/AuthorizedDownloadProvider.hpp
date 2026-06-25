#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <catalog/QueueItem.hpp>

namespace shield::platform {

enum class DownloadStopReason : std::uint8_t {
    None = 0,
    Paused,
    Canceled
};

struct DownloadProgress {
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;
    double bytes_per_second = 0.0;
};

struct DownloadResult {
    bool success = false;
    std::string output_path;
    std::string error_message;
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;
    double bytes_per_second = 0.0;
    DownloadStopReason stop_reason = DownloadStopReason::None;
};

class AuthorizedDownloadProvider {
    public:
        static bool CanHandle(const shield::catalog::QueueItem &item);
        static DownloadResult Download(const shield::catalog::QueueItem &item,
            const std::string &downloads_root,
            std::function<void(const DownloadProgress &)> progress_callback = {},
            std::function<DownloadStopReason()> stop_callback = {});

        // Download a specific byte range [offset, offset+size) from |url|.
        // Each received chunk is forwarded to write_fn; return false from write_fn to abort.
        // Accepts HTTP 200 (full) or 206 (partial). Returns true on success.
        static bool DownloadRange(
            const std::string &url,
            std::uint64_t offset,
            std::uint64_t size,
            std::function<bool(const void *, std::size_t)> write_fn,
            std::function<DownloadStopReason()> stop_fn = {});
};

}
