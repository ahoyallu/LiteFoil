#include <platform/ForwarderInstaller.hpp>
#include <install/InstallEngine.hpp>
#include <install/NcmWrapper.hpp>

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace shield::platform {

namespace {

bool EnsureDir(const char* path) {
    struct stat st{};
    if(::stat(path, &st) == 0) return true;
    return ::mkdir(path, 0777) == 0;
}

bool CopyFile(const char* src, const char* dst, std::function<void(float)>& progress_cb) {
    FILE* in = ::fopen(src, "rb");
    if(!in) return false;

    ::fseek(in, 0, SEEK_END);
    const long total = ::ftell(in);
    ::rewind(in);

    FILE* out = ::fopen(dst, "wb");
    if(!out) { ::fclose(in); return false; }

    char buf[4096];
    long done = 0;
    std::size_t n;
    while((n = ::fread(buf, 1, sizeof(buf), in)) > 0) {
        if(::fwrite(buf, 1, n, out) != n) {
            ::fclose(in);
            ::fclose(out);
            ::remove(dst);
            return false;
        }
        done += static_cast<long>(n);
        if(progress_cb && total > 0) {
            progress_cb(static_cast<float>(done) / static_cast<float>(total) * 0.1f);
        }
    }

    ::fclose(in);
    ::fclose(out);
    return true;
}

void CleanupForwarderTemp() {
    ::remove(ForwarderInstaller::kNspTempPath);
    // Only succeeds when the directory is empty; keep sdmc:/config intact.
    ::rmdir("sdmc:/config/litefoil");
}

struct ForwarderTempCleanupGuard {
    ~ForwarderTempCleanupGuard() {
        CleanupForwarderTemp();
    }
};

}

bool ForwarderInstaller::IsInstalled() {
    return shield::install::IsTitleInstalled(kTitleId);
}

ForwarderInstallResult ForwarderInstaller::Install(
    std::function<void(float)> progress_cb,
    std::function<bool()>      stop_cb)
{
    // Verify the bundled NSP exists (built at compile time — absent if no prod.keys)
    {
        FILE* probe = ::fopen(kNspRomfsPath, "rb");
        if(!probe) {
            return { false, "Forwarder NSP not bundled. Rebuild with prod.keys in ~/.switch/prod.keys or tools/prod.keys." };
        }
        ::fclose(probe);
    }

    // Ensure SD temp directory exists
    if(!EnsureDir("sdmc:/config") || !EnsureDir("sdmc:/config/litefoil")) {
        return { false, "Failed to create temp directory on SD card" };
    }
    ForwarderTempCleanupGuard cleanup_guard;

    // Copy NSP from romfs to SD (romfs is mounted by main.cpp via romfsMountSelf)
    if(!CopyFile(kNspRomfsPath, kNspTempPath, progress_cb)) {
        return { false, "Failed to copy forwarder NSP from romfs to SD card" };
    }

    if(stop_cb && stop_cb()) {
        return { false, "Cancelled" };
    }

    // Install via existing NCM stack
    shield::install::InstallConfig cfg;
    cfg.dest_storage_id = NcmStorageId_SdCard;
    cfg.verify_nca_sigs = false;  // forwarder uses fake sig (Atmosphere patches this)
    cfg.allow_unsigned  = true;
    cfg.ignore_req_fw   = true;
    cfg.reinstall_ncas  = true;

    // Map [0.1 .. 1.0] of progress to the NCM install phase
    shield::install::ProgressCallback ncm_progress;
    if(progress_cb) {
        ncm_progress = [&progress_cb](const shield::install::InstallProgress& p) {
            if(p.bytes_total > 0) {
                const float ratio = static_cast<float>(p.bytes_done) / static_cast<float>(p.bytes_total);
                progress_cb(0.1f + ratio * 0.9f);
            }
        };
    }

    const auto result = shield::install::InstallFromLocalFile(kNspTempPath, cfg, ncm_progress);

    if(!result.success) {
        return { false, result.error_message };
    }

    if(progress_cb) progress_cb(1.0f);
    return { true, {} };
}

}
