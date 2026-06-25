#pragma once

#include <string>

namespace shield::app {

struct AppConfig {
    std::string language = "auto";
    std::string theme = "auto";
    std::string sort_mode = "recent";
    std::string recommended_sort_mode = "critic_score";
    std::string installation_model = "direct";
    std::string catalog_url;
    std::string catalog_title;
    std::string catalog_username;
    std::string catalog_password;
    std::string update_url;
    std::string catalog_client_version = "20.0";
    std::string catalog_client_revision = "0";
    std::string catalog_uid_override;
    std::string rclone_crypt_password;
    std::string rclone_crypt_password2;
    std::string rclone_crypt_catalog_sizes = "plain";
    bool allow_unsigned_sources = false;
    bool show_clock = true;
    bool rclone_crypt_enabled = false;
};

class AppConfigRepository {
    public:
        static AppConfig LoadOrCreate();
        static bool Save(const AppConfig &config);
        static std::string GetConfigPath();
};

}
