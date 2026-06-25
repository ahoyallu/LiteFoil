#pragma once

#include <string>

namespace shield::platform {

class RemoteImageCache {
    public:
        static std::string GetOrQueue(const std::string &cache_key, const std::string &primary_url, const std::string &fallback_url = "");
        static bool PumpCompletedDownloads();
        static void Reset();
        // Cancel any in-flight curl downloads and drain their futures.  Must
        // be called before the application is destroyed: otherwise the static
        // cache's future destructors block for the full curl timeout on every
        // pending download (potentially minutes on exit).
        static void Shutdown();
};

}
