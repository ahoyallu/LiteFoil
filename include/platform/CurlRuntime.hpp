#pragma once

#include <cstdint>

typedef void CURL;
typedef std::int32_t curl_socket_t;

namespace shield::platform {

bool EnsureCurlGlobalInit();

// Returns a thread-local, reusable easy handle. On each call the handle is
// reset (options cleared) but keeps its internal connection cache, so repeated
// requests to the same host reuse the established TCP/TLS connection instead
// of performing a full handshake per request. Returns nullptr if the global
// curl runtime could not be initialized. The handle must NEVER be passed to
// curl_easy_cleanup — it lives until the thread terminates.
//
// Every handle acquired here has a default CURLOPT_SOCKOPTFUNCTION installed
// that applies the project socket profile on every socket curl opens:
// SO_LINGER { l_onoff=1, l_linger=0 } plus TCP_MAXSEG tuning. Callers may
// override CURLOPT_SOCKOPTFUNCTION with their own callback, but that callback
// SHOULD also call ApplyCurlDefaultSockopt(fd) to preserve the baseline.
CURL *AcquireThreadCurlHandle();

// Apply the full default socket profile used by AcquireThreadCurlHandle.
// Meant to be invoked from within a custom CURLOPT_SOCKOPTFUNCTION callback.
// Returns CURL_SOCKOPT_OK (0) so the value can be returned directly.
int ApplyCurlDefaultSockopt(curl_socket_t fd);

// Apply the LINGER-0 socket option used as the shutdown-safety baseline.
// Meant to be invoked from within a custom CURLOPT_SOCKOPTFUNCTION callback.
// Returns CURL_SOCKOPT_OK (0) so the value can be returned directly.
int ApplyCurlLingerSockopt(curl_socket_t fd);

// Releases the global list of acquired easy handles WITHOUT calling
// curl_easy_cleanup on them (see CurlRuntime.cpp for rationale — cleanup has
// been observed to segfault on tainted handles).  Must be called on process
// shutdown BEFORE socketExit().
void ShutdownCurl();

}
