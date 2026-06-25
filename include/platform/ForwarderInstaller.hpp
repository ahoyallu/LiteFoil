#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace shield::platform {

struct ForwarderInstallResult {
    bool        success = false;
    std::string error_message;
};

class ForwarderInstaller {
    public:
        static constexpr std::uint64_t kTitleId       = 0x0100CAFE00000000ULL;
        static constexpr const char*   kNspRomfsPath  = "romfs:/forwarder/LiteFoil.nsp";
        static constexpr const char*   kNspTempPath   = "sdmc:/config/litefoil/LiteFoil_fwd.nsp";

        // Returns true if the forwarder title is already installed (SD or NAND).
        static bool IsInstalled();

        // Installs the bundled forwarder NSP.
        // Copies the pre-built NSP from romfs to a temp SD path, then installs
        // it via NCM. Cleans up the temp file regardless of outcome.
        // progress_cb receives [0.0, 1.0]. stop_cb returns true to abort.
        static ForwarderInstallResult Install(
            std::function<void(float)>   progress_cb = {},
            std::function<bool()>        stop_cb     = {});
};

}
