#include <platform/SystemStatus.hpp>

#include <array>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

#include <switch.h>

namespace shield::platform {
namespace {

std::string ConvertLanguageToTag(const SetLanguage language) {
    switch(language) {
        case SetLanguage_PTBR:
            return "pt-BR";
        case SetLanguage_ENGB:
            return "en-GB";
        case SetLanguage_FR:
            return "fr-FR";
        case SetLanguage_DE:
            return "de-DE";
        case SetLanguage_IT:
            return "it-IT";
        case SetLanguage_ES:
        case SetLanguage_ES419:
            return "es-ES";
        case SetLanguage_JA:
            return "ja-JP";
        case SetLanguage_KO:
            return "ko-KR";
        case SetLanguage_NL:
            return "nl-NL";
        case SetLanguage_PT:
            return "pt-PT";
        case SetLanguage_RU:
            return "ru-RU";
        case SetLanguage_ZHCN:
        case SetLanguage_ZHHANS:
            return "zh-Hans";
        case SetLanguage_ZHTW:
        case SetLanguage_ZHHANT:
            return "zh-Hant";
        case SetLanguage_ENUS:
        case SetLanguage_FRCA:
        default:
            return "en-US";
    }
}

StorageStatus ReadStorageStatus(const bool use_sd_card) {
    StorageStatus status;

    if(R_FAILED(fsInitialize())) {
        return status;
    }

    FsFileSystem file_system;
    Result open_rc = 0;
    if(use_sd_card) {
        open_rc = fsOpenSdCardFileSystem(&file_system);
    }
    else {
        open_rc = fsOpenBisFileSystem(&file_system, FsBisPartitionId_User, "/");
    }

    if(R_SUCCEEDED(open_rc)) {
        s64 free_bytes = 0;
        s64 total_bytes = 0;

        if(R_SUCCEEDED(fsFsGetFreeSpace(&file_system, "/", &free_bytes)) &&
           R_SUCCEEDED(fsFsGetTotalSpace(&file_system, "/", &total_bytes)) &&
           (free_bytes >= 0) &&
           (total_bytes >= 0)) {
            status.available = true;
            status.free_bytes = static_cast<std::uint64_t>(free_bytes);
            status.total_bytes = static_cast<std::uint64_t>(total_bytes);
        }

        fsFsClose(&file_system);
    }

    return status;
}

}

std::string SystemStatus::DetectPreferredLanguageTag(const std::string &configured_language) {
    // A manual language in config always wins over the console language.
    if(!configured_language.empty() && (configured_language != "auto")) {
        return configured_language;
    }

    const Result init_rc = setInitialize();
    if(R_FAILED(init_rc)) {
        return "en-US";
    }

    u64 language_code = 0;
    SetLanguage language = SetLanguage_ENUS;

    if(R_SUCCEEDED(setGetSystemLanguage(&language_code))) {
        if(R_SUCCEEDED(setMakeLanguage(language_code, &language))) {
            setExit();
            return ConvertLanguageToTag(language);
        }
    }

    setExit();
    return "en-US";
}

std::string SystemStatus::BuildClockLabel() {
    const std::time_t current_time = std::time(nullptr);
    const std::tm *local_time = std::localtime(&current_time);
    if(local_time == nullptr) {
        return "--:--";
    }

    char buffer[16] = {};
    std::strftime(buffer, sizeof(buffer), "%H:%M", local_time);
    return buffer;
}

SystemOverview SystemStatus::ReadOverview() {
    SystemOverview overview;
    overview.nand = ReadStorageStatus(false);
    overview.sd_card = ReadStorageStatus(true);

    const Result nifm_rc = nifmInitialize(NifmServiceType_User);
    if(R_FAILED(nifm_rc)) {
        return overview;
    }

    NifmInternetConnectionType connection_type = {};
    NifmInternetConnectionStatus connection_status = NifmInternetConnectionStatus_ConnectingUnknown1;
    u32 wifi_strength = 0;

    if(R_SUCCEEDED(nifmGetInternetConnectionStatus(&connection_type, &wifi_strength, &connection_status))) {
        overview.network_available = true;
        overview.wifi_strength = wifi_strength;

        if(connection_status == NifmInternetConnectionStatus_Connected) {
            switch(connection_type) {
                case NifmInternetConnectionType_WiFi:
                    overview.network_status = NetworkStatus::WiFi;
                    break;
                case NifmInternetConnectionType_Ethernet:
                    overview.network_status = NetworkStatus::Ethernet;
                    break;
                default:
                    overview.network_status = NetworkStatus::Unknown;
                    break;
            }
        }
    }

    return overview;
}

std::string SystemStatus::FormatStorageAmount(const std::uint64_t bytes) {
    constexpr std::array<const char *, 4> units = { "B", "KB", "MB", "GB" };

    double value = static_cast<double>(bytes);
    std::size_t unit_index = 0;
    while((value >= 1024.0) && (unit_index + 1 < units.size())) {
        value /= 1024.0;
        unit_index++;
    }

    std::ostringstream stream;
    if(unit_index >= 2) {
        stream << std::fixed << std::setprecision(1) << value;
    }
    else {
        stream << static_cast<std::uint64_t>(std::round(value));
    }

    stream << ' ' << units[unit_index];
    return stream.str();
}

}
