#pragma once

#include <cstdint>
#include <string>

namespace shield::catalog {

enum class QueueItemState : std::uint8_t {
    Queued = 0,
    Downloading,
    Paused,
    Installing,
    Completed,
    Canceled,
    Failed
};

struct QueueItem {
    std::string title_id;
    std::string base_title_id;
    std::string name;
    std::string subtitle;
    std::string source_url;
    std::string package_format;
    std::string target_location = "SD";
    std::string local_path;
    std::string last_error;
    std::uint64_t size = 0;
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;
    double bytes_per_second = 0.0;
    std::uint32_t retry_count = 0;
    std::uint32_t retry_limit = 2;
    bool delete_after_download = true;
    std::string installation_model = "direct";
    bool keep_download = false;
    bool verify_integrity = true;
    bool auto_start = true;
    bool convert_standard_crypto = false;
    bool reinstall_ncas = false;
    bool include_all_dlcs = true;
    bool include_latest_update = true;
    QueueItemState state = QueueItemState::Queued;
};

}
