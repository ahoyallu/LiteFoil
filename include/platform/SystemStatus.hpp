#pragma once

#include <cstdint>
#include <string>

namespace shield::platform {

struct StorageStatus {
    bool available = false;
    std::uint64_t free_bytes = 0;
    std::uint64_t total_bytes = 0;
};

enum class NetworkStatus {
    Offline,
    WiFi,
    Ethernet,
    Unknown
};

struct SystemOverview {
    StorageStatus nand;
    StorageStatus sd_card;
    NetworkStatus network_status = NetworkStatus::Offline;
    bool network_available = false;
    std::uint32_t wifi_strength = 0;
};

class SystemStatus {
    public:
        static std::string DetectPreferredLanguageTag(const std::string &configured_language);
        static std::string BuildClockLabel();
        static SystemOverview ReadOverview();
        static std::string FormatStorageAmount(std::uint64_t bytes);
};

}
