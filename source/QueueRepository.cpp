#include <app/QueueRepository.hpp>

#include <cerrno>
#include <fstream>
#include <sys/stat.h>

#include <third_party/nlohmann/json.hpp>

namespace shield::app {
namespace {

using Json = nlohmann::json;

constexpr const char *kRootDirectory = "sdmc:/switch/LiteFoil";
constexpr const char *kQueuePath = "sdmc:/switch/LiteFoil/queue.json";

void EnsureDirectory(const char *path) {
    if((mkdir(path, 0777) != 0) && (errno != EEXIST)) {
        return;
    }
}

void EnsureQueueDirectoryTree() {
    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kRootDirectory);
}

std::string SerializeState(const shield::catalog::QueueItemState state) {
    using shield::catalog::QueueItemState;

    switch(state) {
        case QueueItemState::Downloading:
            return "downloading";
        case QueueItemState::Paused:
            return "paused";
        case QueueItemState::Installing:
            return "installing";
        case QueueItemState::Completed:
            return "completed";
        case QueueItemState::Canceled:
            return "canceled";
        case QueueItemState::Failed:
            return "failed";
        case QueueItemState::Queued:
        default:
            return "queued";
    }
}

static std::string ObfuscateUrl(const std::string &url) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(url.size() * 2);
    for(unsigned char c : url) {
        c ^= 0x5A;
        result += kHex[(c >> 4) & 0xF];
        result += kHex[c & 0xF];
    }
    return result;
}

static std::string DeobfuscateUrl(const std::string &hex) {
    std::string result;
    result.reserve(hex.size() / 2);
    for(std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const char h = hex[i], l = hex[i + 1];
        auto hc = [](char c) -> int {
            if(c >= '0' && c <= '9') return c - '0';
            if(c >= 'a' && c <= 'f') return c - 'a' + 10;
            if(c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = hc(h), lo = hc(l);
        if(hi < 0 || lo < 0) return {};
        result += static_cast<char>(static_cast<unsigned char>((hi << 4) | lo) ^ 0x5A);
    }
    return result;
}

shield::catalog::QueueItemState ParseState(const std::string &value) {
    using shield::catalog::QueueItemState;

    if(value == "downloading") {
        return QueueItemState::Downloading;
    }
    if(value == "paused") {
        return QueueItemState::Paused;
    }
    if(value == "installing") {
        return QueueItemState::Installing;
    }
    if(value == "completed") {
        return QueueItemState::Completed;
    }
    if(value == "canceled") {
        return QueueItemState::Canceled;
    }
    if(value == "failed") {
        return QueueItemState::Failed;
    }

    return QueueItemState::Queued;
}

}

std::vector<shield::catalog::QueueItem> QueueRepository::Load() {
    EnsureQueueDirectoryTree();

    std::ifstream stream(kQueuePath);
    if(!stream.good()) {
        return {};
    }

    const Json json = Json::parse(stream, nullptr, false);
    if(json.is_discarded() || !json.is_array()) {
        return {};
    }

    std::vector<shield::catalog::QueueItem> items;
    items.reserve(json.size());

    for(const auto &entry : json) {
        if(!entry.is_object()) {
            continue;
        }

        shield::catalog::QueueItem item;
        if(entry.contains("titleId") && entry["titleId"].is_string()) {
            item.title_id = entry["titleId"].get<std::string>();
        }
        if(item.title_id.empty()) {
            continue;
        }

        if(entry.contains("baseTitleId") && entry["baseTitleId"].is_string()) {
            item.base_title_id = entry["baseTitleId"].get<std::string>();
        }
        if(entry.contains("name") && entry["name"].is_string()) {
            item.name = entry["name"].get<std::string>();
        }
        if(entry.contains("subtitle") && entry["subtitle"].is_string()) {
            item.subtitle = entry["subtitle"].get<std::string>();
        }
        if(entry.contains("dl") && entry["dl"].is_string()) {
            item.source_url = DeobfuscateUrl(entry["dl"].get<std::string>());
        } else if(entry.contains("sourceUrl") && entry["sourceUrl"].is_string()) {
            item.source_url = entry["sourceUrl"].get<std::string>();
        }
        if(entry.contains("packageFormat") && entry["packageFormat"].is_string()) {
            item.package_format = entry["packageFormat"].get<std::string>();
        }
        if(entry.contains("targetLocation") && entry["targetLocation"].is_string()) {
            item.target_location = entry["targetLocation"].get<std::string>();
        }
        if(entry.contains("localPath") && entry["localPath"].is_string()) {
            item.local_path = entry["localPath"].get<std::string>();
        }
        if(entry.contains("lastError") && entry["lastError"].is_string()) {
            item.last_error = entry["lastError"].get<std::string>();
        }
        if(entry.contains("size") && entry["size"].is_number_unsigned()) {
            item.size = entry["size"].get<std::uint64_t>();
        }
        if(entry.contains("bytesDone") && entry["bytesDone"].is_number_unsigned()) {
            item.bytes_done = entry["bytesDone"].get<std::uint64_t>();
        }
        if(entry.contains("bytesTotal") && entry["bytesTotal"].is_number_unsigned()) {
            item.bytes_total = entry["bytesTotal"].get<std::uint64_t>();
        }
        if(entry.contains("bytesPerSecond") && entry["bytesPerSecond"].is_number()) {
            item.bytes_per_second = entry["bytesPerSecond"].get<double>();
        }
        if(entry.contains("retryCount") && entry["retryCount"].is_number_unsigned()) {
            item.retry_count = entry["retryCount"].get<std::uint32_t>();
        }
        if(entry.contains("retryLimit") && entry["retryLimit"].is_number_unsigned()) {
            item.retry_limit = entry["retryLimit"].get<std::uint32_t>();
        }
        if(entry.contains("deleteAfterDownload") && entry["deleteAfterDownload"].is_boolean()) {
            item.delete_after_download = entry["deleteAfterDownload"].get<bool>();
            item.keep_download = !item.delete_after_download;
        }
        if(entry.contains("installationModel") && entry["installationModel"].is_string()) {
            item.installation_model = entry["installationModel"].get<std::string>();
        } else {
            item.installation_model = item.delete_after_download ? "stream" : "direct";
        }
        if(entry.contains("keepDownload") && entry["keepDownload"].is_boolean()) {
            item.keep_download = entry["keepDownload"].get<bool>();
        }
        if(entry.contains("verifyIntegrity") && entry["verifyIntegrity"].is_boolean()) {
            item.verify_integrity = entry["verifyIntegrity"].get<bool>();
        }
        if(entry.contains("autoStart") && entry["autoStart"].is_boolean()) {
            item.auto_start = entry["autoStart"].get<bool>();
        }
        if(entry.contains("convertStandardCrypto") && entry["convertStandardCrypto"].is_boolean()) {
            item.convert_standard_crypto = entry["convertStandardCrypto"].get<bool>();
        }
        if(entry.contains("reinstallNcas") && entry["reinstallNcas"].is_boolean()) {
            item.reinstall_ncas = entry["reinstallNcas"].get<bool>();
        }
        if(entry.contains("includeAllDlcs") && entry["includeAllDlcs"].is_boolean()) {
            item.include_all_dlcs = entry["includeAllDlcs"].get<bool>();
        }
        if(entry.contains("includeLatestUpdate") && entry["includeLatestUpdate"].is_boolean()) {
            item.include_latest_update = entry["includeLatestUpdate"].get<bool>();
        }
        if(entry.contains("state") && entry["state"].is_string()) {
            item.state = ParseState(entry["state"].get<std::string>());
        }

        if((item.state == shield::catalog::QueueItemState::Downloading) || (item.state == shield::catalog::QueueItemState::Installing)) {
            item.state = shield::catalog::QueueItemState::Paused;
        }

        items.push_back(std::move(item));
    }

    return items;
}

bool QueueRepository::Save(const std::vector<shield::catalog::QueueItem> &items) {
    EnsureQueueDirectoryTree();

    Json json = Json::array();
    for(const auto &item : items) {
        json.push_back({
            { "titleId", item.title_id },
            { "baseTitleId", item.base_title_id },
            { "name", item.name },
            { "subtitle", item.subtitle },
            { "dl", ObfuscateUrl(item.source_url) },
            { "packageFormat", item.package_format },
            { "targetLocation", item.target_location },
            { "localPath", item.local_path },
            { "lastError", item.last_error },
            { "size", item.size },
            { "bytesDone", item.bytes_done },
            { "bytesTotal", item.bytes_total },
            { "bytesPerSecond", item.bytes_per_second },
            { "retryCount", item.retry_count },
            { "retryLimit", item.retry_limit },
            { "deleteAfterDownload", item.delete_after_download },
            { "installationModel", item.installation_model },
            { "keepDownload", item.keep_download },
            { "verifyIntegrity", item.verify_integrity },
            { "autoStart", item.auto_start },
            { "convertStandardCrypto", item.convert_standard_crypto },
            { "reinstallNcas", item.reinstall_ncas },
            { "includeAllDlcs", item.include_all_dlcs },
            { "includeLatestUpdate", item.include_latest_update },
            { "state", SerializeState(item.state) }
        });
    }

    std::ofstream stream(kQueuePath, std::ios::trunc);
    if(!stream.good()) {
        return false;
    }

    stream << json.dump(2) << '\n';
    return stream.good();
}

}
