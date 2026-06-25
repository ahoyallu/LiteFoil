#include <platform/CurlRuntime.hpp>

#include <mutex>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <curl/curl.h>

#include <platform/ExitLog.hpp>

namespace shield::platform {
namespace {

std::once_flag g_curl_once;
bool g_curl_initialized = false;

// Per-thread reusable easy handle. The handle lives until process shutdown
// (see ShutdownCurl) so that the connection cache and cookie engine survive
// across repeated requests from the same worker thread.
thread_local CURL *t_reusable_handle = nullptr;

// Global list of every handle ever handed out by AcquireThreadCurlHandle,
// so ShutdownCurl can close every single one — including handles from worker
// threads that have already exited. Without this, libcurl's keep-alive TCP
// connections stay open and socketExit() hangs at process exit.
std::mutex &HandlesMutex() {
    static std::mutex m;
    return m;
}

std::vector<CURL *> &AllHandles() {
    static std::vector<CURL *> v;
    return v;
}

std::vector<int> &AllCurlSockets() {
    static std::vector<int> v;
    return v;
}

void TrackCurlSocket(const curl_socket_t fd) {
    std::lock_guard<std::mutex> lock(HandlesMutex());
    AllCurlSockets().push_back(static_cast<int>(fd));
}

#if !defined(LITEFOIL_DOWNLOAD_TCP_MAXSEG)
#define LITEFOIL_DOWNLOAD_TCP_MAXSEG 1412
#endif

extern "C" int ShieldCurlLingerSockopt(void *, curl_socket_t fd, curlsocktype) {
    TrackCurlSocket(fd);
    struct linger lin;
    lin.l_onoff  = 1;
    lin.l_linger = 0;
    setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
    return CURL_SOCKOPT_OK;
}

// Shared SOCKOPTFUNCTION installed on every handle we hand out.  LINGER-0 lets
// socketExit close leaked keep-alive fds without blocking, and TCP_MAXSEG keeps
// large curl transfers on the same low-fragmentation profile.
extern "C" int ShieldCurlDefaultSockopts(void *, curl_socket_t fd, curlsocktype type) {
    ShieldCurlLingerSockopt(nullptr, fd, type);
#if LITEFOIL_DOWNLOAD_TCP_MAXSEG > 0
    const int mss = LITEFOIL_DOWNLOAD_TCP_MAXSEG;
    setsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss));
#endif
    return CURL_SOCKOPT_OK;
}

}

bool EnsureCurlGlobalInit() {
    std::call_once(g_curl_once, []() {
        g_curl_initialized = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    });
    return g_curl_initialized;
}

CURL *AcquireThreadCurlHandle() {
    if(!EnsureCurlGlobalInit()) {
        return nullptr;
    }

    if(t_reusable_handle == nullptr) {
        t_reusable_handle = curl_easy_init();
        if(t_reusable_handle != nullptr) {
            std::lock_guard<std::mutex> lock(HandlesMutex());
            AllHandles().push_back(t_reusable_handle);
        }
    }
    else {
        // Keeps the internal connection cache and cookie engine, but clears
        // all per-request options so the caller starts from a clean slate.
        curl_easy_reset(t_reusable_handle);
    }

    // Always (re-)install the default sockopt callback.  curl_easy_reset clears
    // options, and the very first init has no options yet, so this covers both
    // paths.  Callers may override CURLOPT_SOCKOPTFUNCTION with their own
    // tuning, but should call ApplyCurlDefaultSockopt from that callback.
    if(t_reusable_handle != nullptr) {
        curl_easy_setopt(t_reusable_handle, CURLOPT_SOCKOPTFUNCTION, ShieldCurlDefaultSockopts);
        curl_easy_setopt(t_reusable_handle, CURLOPT_SOCKOPTDATA, nullptr);
        curl_easy_setopt(t_reusable_handle, CURLOPT_FORBID_REUSE, 1L);
        curl_easy_setopt(t_reusable_handle, CURLOPT_FRESH_CONNECT, 1L);
    }
    return t_reusable_handle;
}

int ApplyCurlDefaultSockopt(curl_socket_t fd) {
    return ShieldCurlDefaultSockopts(nullptr, fd, CURLSOCKTYPE_IPCXN);
}

int ApplyCurlLingerSockopt(curl_socket_t fd) {
    return ShieldCurlLingerSockopt(nullptr, fd, CURLSOCKTYPE_IPCXN);
}

void ShutdownCurl() {
    std::vector<CURL *> leaked;
    std::vector<int> sockets;
    {
        std::lock_guard<std::mutex> lock(HandlesMutex());
        leaked.swap(AllHandles());
        sockets.swap(AllCurlSockets());
    }

    // We intentionally do NOT call curl_easy_cleanup.  Observed on Switch:
    // when a handle's connection cache carries state from a cancelled or
    // failed transfer, curl_easy_cleanup deterministically segfaults inside
    // the TLS-shutdown path (exit.log pinpointed it to the first handle).
    // There is no portable way to tell which handles are "tainted", so the
    // only safe option is to leak them and let process tear-down reclaim
    // everything.
    //
    // Keep-alive sockets are disabled on every acquired handle (FORBID_REUSE
    // + FRESH_CONNECT), so curl should close connections after each transfer.
    // The tracked socket count is diagnostic only; force-closing FDs here is
    // unsafe because an old fd number may have been reused by another service.
    //
    // curl_global_cleanup is also skipped: it is optional, and running it
    // after skipping easy cleanup would double-free internal state.
    ExitLog("ShutdownCurl: leaking %zu libcurl handles, tracked %zu sockets",
        leaked.size(), sockets.size());
}

}
