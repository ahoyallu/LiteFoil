#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <switch.h>

namespace shield::catalog {

struct InstalledTitle {
    u64 application_id = 0;
    std::string title_id_hex;
    std::string name;
    std::string author;
    std::string description;
    std::string intro;
    std::string developer;
    std::string display_version;
    std::string icon_url;
    u32 application_content_version = 0;
    u32 patch_content_version = 0;
    u32 content_version = 0;
    u32 release_date = 0;
    u64 size = 0;
    int number_of_players = 0;
    int rating = 0;
    std::vector<std::string> add_on_title_ids;
    std::vector<std::string> categories;
    std::vector<std::string> languages;
    std::vector<std::string> screenshots;
    u8 storage_id = NcmStorageId_None;
    bool has_icon = false;
    size_t icon_size = 0;
};

}
