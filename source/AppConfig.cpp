#include <app/AppConfig.hpp>

#include <cerrno>
#include <fstream>
#include <sys/stat.h>

#include <third_party/nlohmann/json.hpp>

namespace shield::app {
namespace {

using Json = nlohmann::json;

constexpr const char *kRootDirectory = "sdmc:/switch/LiteFoil";
constexpr const char *kConfigPath = "sdmc:/switch/LiteFoil/config.json";
constexpr const char *kFallbackConfigPath = "config.json";

void EnsureDirectory(const char *path) {
    // Creating an existing directory is harmless, so only real filesystem failures matter here.
    if((mkdir(path, 0777) != 0) && (errno != EEXIST)) {
        return;
    }
}

void EnsureConfigDirectoryTree() {
    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kRootDirectory);
}

void ApplyStringField(const Json &json, const char *key, std::string &target) {
    if(json.contains(key) && json[key].is_string()) {
        target = json[key].get<std::string>();
    }
}

void ApplyBoolField(const Json &json, const char *key, bool &target) {
    if(json.contains(key) && json[key].is_boolean()) {
        target = json[key].get<bool>();
    }
}

void ApplyJsonToConfig(const Json &json, AppConfig &config) {
    ApplyStringField(json, "language", config.language);
    ApplyStringField(json, "theme", config.theme);
    ApplyStringField(json, "sortMode", config.sort_mode);
    ApplyStringField(json, "recommendedSortMode", config.recommended_sort_mode);
    ApplyStringField(json, "installationModel", config.installation_model);
    ApplyStringField(json, "catalogUrl", config.catalog_url);
    ApplyStringField(json, "catalogTitle", config.catalog_title);
    ApplyStringField(json, "catalogUsername", config.catalog_username);
    ApplyStringField(json, "catalogPassword", config.catalog_password);
    ApplyStringField(json, "update_url", config.update_url);
    ApplyStringField(json, "updateUrl", config.update_url);
    ApplyStringField(json, "catalogClientVersion", config.catalog_client_version);
    ApplyStringField(json, "catalogClientRevision", config.catalog_client_revision);
    ApplyStringField(json, "catalogUidOverride", config.catalog_uid_override);
    ApplyStringField(json, "rcloneCryptPassword", config.rclone_crypt_password);
    ApplyStringField(json, "rcloneCryptPassword2", config.rclone_crypt_password2);
    ApplyStringField(json, "rcloneCryptCatalogSizes", config.rclone_crypt_catalog_sizes);
    ApplyBoolField(json, "allowUnsignedSources", config.allow_unsigned_sources);
    ApplyBoolField(json, "showClock", config.show_clock);
    ApplyBoolField(json, "rcloneCryptEnabled", config.rclone_crypt_enabled);

    const auto crypt_it = json.find("rcloneCrypt");
    if((crypt_it != json.end()) && crypt_it->is_object()) {
        const Json &crypt = *crypt_it;
        ApplyStringField(crypt, "password", config.rclone_crypt_password);
        ApplyStringField(crypt, "password2", config.rclone_crypt_password2);
        ApplyStringField(crypt, "catalogSizes", config.rclone_crypt_catalog_sizes);
        ApplyBoolField(crypt, "enabled", config.rclone_crypt_enabled);
    }
}

bool TryLoadConfigFile(const char *path, AppConfig &config) {
    std::ifstream config_stream(path);
    if(!config_stream.good()) {
        return false;
    }

    const Json json = Json::parse(config_stream, nullptr, false);
    if(json.is_discarded()) {
        return false;
    }

    ApplyJsonToConfig(json, config);
    return true;
}

void MergeMissingString(std::string &target, const std::string &fallback) {
    if(target.empty() && !fallback.empty()) {
        target = fallback;
    }
}

void MergeMissingConfig(AppConfig &primary, const AppConfig &fallback) {
    MergeMissingString(primary.language, fallback.language);
    MergeMissingString(primary.theme, fallback.theme);
    MergeMissingString(primary.sort_mode, fallback.sort_mode);
    MergeMissingString(primary.recommended_sort_mode, fallback.recommended_sort_mode);
    MergeMissingString(primary.installation_model, fallback.installation_model);
    MergeMissingString(primary.catalog_url, fallback.catalog_url);
    MergeMissingString(primary.catalog_title, fallback.catalog_title);
    MergeMissingString(primary.catalog_username, fallback.catalog_username);
    MergeMissingString(primary.catalog_password, fallback.catalog_password);
    MergeMissingString(primary.update_url, fallback.update_url);
    MergeMissingString(primary.catalog_client_version, fallback.catalog_client_version);
    MergeMissingString(primary.catalog_client_revision, fallback.catalog_client_revision);
    MergeMissingString(primary.catalog_uid_override, fallback.catalog_uid_override);
    MergeMissingString(primary.rclone_crypt_password, fallback.rclone_crypt_password);
    MergeMissingString(primary.rclone_crypt_password2, fallback.rclone_crypt_password2);
    MergeMissingString(primary.rclone_crypt_catalog_sizes, fallback.rclone_crypt_catalog_sizes);

    if(!primary.allow_unsigned_sources && fallback.allow_unsigned_sources) {
        primary.allow_unsigned_sources = true;
    }
    if(!primary.show_clock && fallback.show_clock) {
        primary.show_clock = true;
    }
    if(!primary.rclone_crypt_enabled && fallback.rclone_crypt_enabled) {
        primary.rclone_crypt_enabled = true;
    }
}

bool ShouldWriteRcloneCryptConfig(const AppConfig &config) {
    return config.rclone_crypt_enabled
        || !config.rclone_crypt_password.empty()
        || !config.rclone_crypt_password2.empty()
        || (!config.rclone_crypt_catalog_sizes.empty() && config.rclone_crypt_catalog_sizes != "plain");
}

}

std::string AppConfigRepository::GetConfigPath() {
    return kConfigPath;
}

AppConfig AppConfigRepository::LoadOrCreate() {
    AppConfig config;
    AppConfig fallback_config;
    const bool has_fallback = TryLoadConfigFile(kFallbackConfigPath, fallback_config);

    // The first run should bootstrap a clean configuration file on the SD card automatically.
    EnsureConfigDirectoryTree();

    // Prefer the canonical SD config, but merge in any missing fields from the
    // workspace fallback during development so blank SD configs do not wipe
    // valid local catalog credentials.
    if(TryLoadConfigFile(kConfigPath, config)) {
        if(has_fallback) {
            MergeMissingConfig(config, fallback_config);
        }
        return config;
    }
    if(has_fallback) {
        config = fallback_config;
        return config;
    }

    // If no readable config exists we keep the app bootable and write defaults to the canonical SD path.
    Save(config);
    return config;
}

bool AppConfigRepository::Save(const AppConfig &config) {
    EnsureConfigDirectoryTree();

    Json json = {
        { "language", config.language },
        { "theme", config.theme },
        { "sortMode", config.sort_mode },
        { "recommendedSortMode", config.recommended_sort_mode },
        { "installationModel", config.installation_model },
        { "catalogUrl", config.catalog_url },
        { "catalogTitle", config.catalog_title },
        { "catalogUsername", config.catalog_username },
        { "catalogPassword", config.catalog_password },
        { "allowUnsignedSources", config.allow_unsigned_sources },
        { "showClock", config.show_clock }
    };

    if(!config.update_url.empty()) {
        json["update_url"] = config.update_url;
    }

    if(ShouldWriteRcloneCryptConfig(config)) {
        json["rcloneCrypt"] = Json{
            { "enabled", config.rclone_crypt_enabled },
            { "password", config.rclone_crypt_password },
            { "password2", config.rclone_crypt_password2 },
            { "catalogSizes", config.rclone_crypt_catalog_sizes }
        };
    }

    std::ofstream config_stream(GetConfigPath(), std::ios::trunc);
    if(!config_stream.good()) {
        return false;
    }

    // Pretty-printing helps when we need to inspect or hand-edit settings during development.
    config_stream << json.dump(4) << '\n';
    return config_stream.good();
}

}
