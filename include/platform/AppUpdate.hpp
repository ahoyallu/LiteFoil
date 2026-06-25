#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace shield::platform {

struct AppUpdateInfo {
    bool available = false;
    std::string version;
    std::string url;
    std::string message;
};

struct AppUpdateResult {
    bool success = false;
    std::string message;
    std::string installed_path;
};

struct AppUpdateProgress {
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;
    double ratio = 0.0;
    std::string stage;
};

class AppUpdate {
    public:
        static void CleanupCache();
        static AppUpdateInfo Check(const std::string &update_url, const std::string &local_version);
        static AppUpdateResult DownloadAndInstall(const AppUpdateInfo &info,
            std::function<void(const AppUpdateProgress &)> progress_callback = {});
        static int CompareVersions(const std::string &left, const std::string &right);
        static std::string GetCurrentNroPath();
        static std::string GetNextLoadPath(const std::string &nro_path);
};

}
