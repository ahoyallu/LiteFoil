#pragma once

// Lightweight file logger dedicated to the shutdown / exit path.
// Writes to sdmc:/switch/LiteFoil/exit.log and flushes+closes after every
// line so that, even if the process hangs, the last line is persisted to SD.
// Intentionally header-only and dependency-free to avoid pulling in Plutonium.

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <sys/stat.h>
#include <switch.h>

#include <AppVersion.hpp>

namespace shield::platform {

#if !defined(LITEFOIL_ENABLE_EXIT_LOGS)
#define LITEFOIL_ENABLE_EXIT_LOGS 0
#endif

#if !defined(LITEFOIL_ENABLE_RUNTIME_LOGS)
#define LITEFOIL_ENABLE_RUNTIME_LOGS 0
#endif

inline void ExitLog(const char *fmt, ...) {
#if LITEFOIL_ENABLE_EXIT_LOGS
    std::FILE *fp = std::fopen("sdmc:/switch/LiteFoil/exit.log", "a");
    if(fp == nullptr) {
        return;
    }

    const u64 ticks = armGetSystemTick();
    // TickFreq is ~19.2 MHz; print raw ticks and a millisecond derivation.
    const u64 ms = ticks / 19200ULL;
    std::fprintf(fp, "[%llu ms] ", static_cast<unsigned long long>(ms));

    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(fp, fmt, args);
    va_end(args);

    std::fputc('\n', fp);
    std::fflush(fp);
    std::fclose(fp);
#else
    (void)fmt;
#endif
}

inline void ExitLogReset() {
#if LITEFOIL_ENABLE_EXIT_LOGS
    std::FILE *fp = std::fopen("sdmc:/switch/LiteFoil/exit.log", "w");
    if(fp != nullptr) {
        std::fclose(fp);
    }
#endif
}

// Runtime diagnostic log, separate from exit.log so that it survives a normal
// shutdown (ExitLogReset wipes exit.log at BeginShutdown).  Use for install /
// download pipeline tracing that must be preserved across the app's lifetime.
inline void RuntimeLog(const char *fmt, ...) {
#if LITEFOIL_ENABLE_RUNTIME_LOGS
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/LiteFoil", 0777);

    std::FILE *fp = std::fopen("sdmc:/switch/LiteFoil/runtime.log", "a");
    if(fp == nullptr) {
        return;
    }

    const u64 ticks = armGetSystemTick();
    const u64 ms = ticks / 19200ULL;
    std::fprintf(fp, "[%llu ms] ", static_cast<unsigned long long>(ms));

    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(fp, fmt, args);
    va_end(args);

    std::fputc('\n', fp);
    std::fflush(fp);
    std::fclose(fp);
#else
    (void)fmt;
#endif
}

}
