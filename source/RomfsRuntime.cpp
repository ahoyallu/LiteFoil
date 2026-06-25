#include <platform/RomfsRuntime.hpp>

#include <atomic>

#include <platform/ExitLog.hpp>

namespace shield::platform {
namespace {

std::atomic<bool> g_romfs_initialized{false};

}

Result InitializeRomfs() {
    RuntimeLog("[romfs] InitializeRomfs: enter initialized=%d",
        static_cast<int>(g_romfs_initialized.load(std::memory_order_acquire)));
    if(g_romfs_initialized.load(std::memory_order_acquire)) {
        return 0;
    }

    const Result rc = romfsInit();
    if(R_SUCCEEDED(rc)) {
        g_romfs_initialized.store(true, std::memory_order_release);
    }
    RuntimeLog("[romfs] InitializeRomfs: rc=0x%x initialized=%d",
        rc, static_cast<int>(g_romfs_initialized.load(std::memory_order_acquire)));
    return rc;
}

void ExitRomfs() {
    RuntimeLog("[romfs] ExitRomfs: enter initialized=%d",
        static_cast<int>(g_romfs_initialized.load(std::memory_order_acquire)));
    if(g_romfs_initialized.exchange(false, std::memory_order_acq_rel)) {
        romfsExit();
        RuntimeLog("[romfs] ExitRomfs: romfsExit called");
    }
    RuntimeLog("[romfs] ExitRomfs: exit initialized=%d",
        static_cast<int>(g_romfs_initialized.load(std::memory_order_acquire)));
}

}
