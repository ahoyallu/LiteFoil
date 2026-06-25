#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace shield::catalog {

enum class RemoteContentType : std::uint8_t {
    Unknown = 0,
    BaseGame,
    Update,
    Dlc
};

struct RemoteFileEntry {
    std::string url;
    std::uint64_t size = 0;
};

struct RemoteTitleMetadata {
    std::string id;
    std::string base_title_id;
    std::string name;
    std::string file_name;
    std::string publisher;
    std::string description;
    std::string intro;
    std::string developer;
    std::string region;
    std::string icon_url;
    std::string local_icon_path;
    std::string package_format;
    std::vector<std::string> categories;
    std::vector<std::string> languages;
    std::vector<std::string> screenshots;
    std::uint32_t version = 0;
    std::uint64_t size = 0;
    std::uint32_t release_date = 0;
    int rank = 0;
    double critic_score = -1.0;
    int review_count = -1;
    std::string recommendation_source;
    int number_of_players = 0;
    int rating = 0;
    RemoteContentType content_type = RemoteContentType::Unknown;
};

struct RemoteCatalog {
    std::vector<RemoteFileEntry> files;
    std::vector<std::string> directories;
    std::unordered_map<std::string, RemoteTitleMetadata> titles_by_id;
    std::vector<RemoteTitleMetadata> recommended_titles;
    std::string success_message;
    std::string error_message;
};

}
