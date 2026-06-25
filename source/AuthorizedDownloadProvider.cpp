#include <platform/AuthorizedDownloadProvider.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unordered_map>

#include <curl/curl.h>
#include <switch.h>

#include <platform/CurlRuntime.hpp>
#include <platform/ExitLog.hpp>
#include <platform/GDriveAuthManager.hpp>
#include <platform/RcloneCrypt.hpp>
#include <third_party/nlohmann/json.hpp>

namespace shield::platform {
namespace {

using Json = nlohmann::json;

#if !defined(LITEFOIL_DOWNLOAD_TCP_NODELAY)
#define LITEFOIL_DOWNLOAD_TCP_NODELAY 1
#endif

#if !defined(LITEFOIL_DOWNLOAD_USER_AGENT)
#define LITEFOIL_DOWNLOAD_USER_AGENT "Mozilla/5.0 (Nintendo Switch) LiteFoil/2.0"
#endif

// Receive buffer handed to curl (CURLOPT_BUFFERSIZE).
static constexpr std::size_t kCurlRecvSize = 512 * 1024;
// RAM accumulator flushed to disk in large aligned chunks.
static constexpr std::size_t kAccumSize = 4 * 1024 * 1024;
// stdio buffer for the underlying FILE* (applied before first fwrite).
static constexpr std::size_t kStdioBufferSize = 512 * 1024;
// Maximum number of download attempts before giving up.
static constexpr int kMaxRetryCount = 3;

int ApplyCurlSocketOptions(void *, curl_socket_t curl_socket, curlsocktype) {
    return shield::platform::ApplyCurlDefaultSockopt(curl_socket);
}

void ConfigureCurlTransfer(CURL *curl, const char *user_agent) {
    const char *effective_user_agent = (LITEFOIL_DOWNLOAD_USER_AGENT[0] != '\0')
        ? LITEFOIL_DOWNLOAD_USER_AGENT
        : user_agent;
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, effective_user_agent);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, ApplyCurlSocketOptions);
    // Stay on HTTP/1.1: the devkitPro libcurl's HTTP/2 path has flow-control
    // quirks that stall long-running range downloads on the Switch.  Keep-alive
    // over the thread-local handle already gives us connection reuse, which is
    // what actually matters for throughput.
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                     static_cast<long>(CURL_HTTP_VERSION_1_1));
    // In-memory cookie jar: needed for Google Drive confirm tokens and for
    // one-shot Cloudflare challenges that set a short-lived cookie before
    // redirecting to the real content.  Empty filename = memory only.
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    // Force identity so Cloudflare (or any intermediate) never delivers a
    // transparently compressed body; byte ranges must address raw file
    // offsets.  Without this, a Content-Type match could trigger gzip and
    // break the offset math used by the stream installer.
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
}

bool IsRetryableCurlError(CURLcode code) {
    switch(code) {
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
            return true;
        default:
            return false;
    }
}

bool IsRetryableHttpStatus(long status) {
    return status == 408 || status == 429 || (status >= 500 && status < 600);
}

void SleepRetryBackoff(int attempt_index) {
    const std::uint64_t seconds = static_cast<std::uint64_t>(1u << std::min(attempt_index, 3));
    svcSleepThread(static_cast<std::int64_t>(seconds * 1'000'000'000ULL));
}

struct DownloadContext {
    FILE *output_fp = nullptr;
    std::unique_ptr<char[]> stdio_buffer;  // setvbuf buffer for output_fp
    std::unique_ptr<char[]> accum;         // RAM write accumulator
    std::size_t accum_used = 0;
    bool write_error = false;
    std::function<void(const DownloadProgress &)> progress_callback;
    std::function<DownloadStopReason()> stop_callback;
    DownloadStopReason stop_reason = DownloadStopReason::None;
    std::uint64_t resume_offset = 0;
    std::chrono::steady_clock::time_point last_speed_sample_at = std::chrono::steady_clock::now();
    std::uint64_t last_speed_sample_bytes = 0;
    std::chrono::steady_clock::time_point last_progress_emit_at = std::chrono::steady_clock::now();
    std::uint64_t last_progress_emit_bytes = 0;
    double bytes_per_second = 0.0;
};

bool FlushAccumulator(DownloadContext *ctx) {
    if(ctx->accum_used == 0) return true;
    const std::size_t written = std::fwrite(ctx->accum.get(), 1, ctx->accum_used, ctx->output_fp);
    const bool ok = (written == ctx->accum_used);
    ctx->accum_used = 0;
    return ok;
}

bool StartsWithHttp(const std::string &value) {
    return (value.rfind("https://", 0) == 0) || (value.rfind("http://", 0) == 0);
}

bool IsGDriveSchemeUrl(const std::string &url) {
    return url.rfind("gdrive:", 0) == 0;
}

bool IsGDriveCryptSchemeUrl(const std::string &url) {
    return url.rfind("gdrivecrypt:", 0) == 0;
}

bool IsPrivateGDriveSchemeUrl(const std::string &url) {
    return IsGDriveSchemeUrl(url) || IsGDriveCryptSchemeUrl(url);
}

bool IsGoogleDriveUrl(const std::string &url) {
    if(IsPrivateGDriveSchemeUrl(url)) return true;
    if(url.find("drive.google.com") != std::string::npos) return true;
    return false;
}

bool ShouldUseRcloneCryptForUrl(const std::string &url) {
    return IsRcloneCryptEnabled() && IsGDriveCryptSchemeUrl(url);
}

// Extract the Google Drive file ID from various URL formats:
//   gdrive:/{FILE_ID}
//   gdrivecrypt:/{FILE_ID}
//   https://drive.google.com/file/d/{FILE_ID}/...
//   https://drive.google.com/open?id={FILE_ID}
//   https://drive.google.com/uc?id={FILE_ID}...
std::string ExtractGoogleDriveFileId(const std::string &url) {
    // gdrive:FILE_ID, gdrive:/FILE_ID, gdrivecrypt:FILE_ID, or gdrivecrypt:/FILE_ID.
    if(IsPrivateGDriveSchemeUrl(url)) {
        std::string id = IsGDriveCryptSchemeUrl(url) ? url.substr(12) : url.substr(7);
        while(!id.empty() && id.front() == '/') id.erase(id.begin());
        const auto hash = id.find('#');
        if(hash != std::string::npos) id = id.substr(0, hash);
        const auto amp = id.find('&');
        if(amp != std::string::npos) id = id.substr(0, amp);
        const auto q = id.find('?');
        if(q != std::string::npos) id = id.substr(0, q);
        while(!id.empty() && id.back() == '/') id.pop_back();
        return id;
    }

    // /file/d/{ID}/
    const auto file_d = url.find("/file/d/");
    if(file_d != std::string::npos) {
        std::string id = url.substr(file_d + 8);
        const auto slash = id.find('/');
        if(slash != std::string::npos) id = id.substr(0, slash);
        const auto q = id.find('?');
        if(q != std::string::npos) id = id.substr(0, q);
        return id;
    }

    // ?id={ID} or &id={ID}
    auto extract_param = [](const std::string &u, const std::string &param) -> std::string {
        const auto pos = u.find(param);
        if(pos == std::string::npos) return {};
        std::string val = u.substr(pos + param.size());
        const auto amp = val.find('&');
        if(amp != std::string::npos) val = val.substr(0, amp);
        return val;
    };

    std::string id = extract_param(url, "id=");
    return id;
}

std::string BuildGoogleDriveDirectUrl(const std::string &file_id) {
    return "https://drive.google.com/uc?id=" + file_id + "&export=download";
}

std::string BuildGoogleDriveApiMediaUrl(const std::string &file_id) {
    return "https://www.googleapis.com/drive/v3/files/" + file_id
        + "?alt=media&supportsAllDrives=true";
}

std::string BuildGoogleDriveApiMetadataUrl(const std::string &file_id) {
    return "https://www.googleapis.com/drive/v3/files/" + file_id
        + "?fields=id,name,size,mimeType,capabilities/canDownload&supportsAllDrives=true";
}

std::string ResolveGoogleDriveConfirmUrl(const std::string &file_id);

struct ResolvedDownloadUrl {
    std::string url;
    bool uses_private_gdrive = false;
    std::string bearer_token;
    bool blocked = false;
    bool auth_failure = false;
    std::string error_message;
};

struct GDriveMetadataResult {
    bool success = false;
    bool auth_failure = false;
    bool downloadable = false;
    std::string error_message;
};

struct HeaderList {
    curl_slist *headers = nullptr;

    HeaderList() = default;
    HeaderList(const HeaderList &) = delete;
    HeaderList &operator=(const HeaderList &) = delete;
    HeaderList(HeaderList &&other) noexcept : headers(other.headers) {
        other.headers = nullptr;
    }
    HeaderList &operator=(HeaderList &&other) noexcept {
        if(this != &other) {
            if(headers != nullptr) {
                curl_slist_free_all(headers);
            }
            headers = other.headers;
            other.headers = nullptr;
        }
        return *this;
    }

    ~HeaderList() {
        if(headers != nullptr) {
            curl_slist_free_all(headers);
        }
    }

    void Append(const std::string &header) {
        headers = curl_slist_append(headers, header.c_str());
    }
};

bool TryGetCachedGDriveMetadata(const std::string &file_id, GDriveMetadataResult &out);
void CacheGDriveMetadata(const std::string &file_id, const GDriveMetadataResult &result);

std::size_t WriteMetadataBody(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
    auto *body = static_cast<std::string *>(userdata);
    const std::size_t bytes = size * nmemb;
    if(body != nullptr && body->size() < 256 * 1024) {
        body->append(ptr, bytes);
    }
    return bytes;
}

bool JsonBoolNested(const Json &json, const char *object_key, const char *value_key, bool fallback) {
    const auto object_it = json.find(object_key);
    if((object_it == json.end()) || !object_it->is_object()) {
        return fallback;
    }

    const auto value_it = object_it->find(value_key);
    if((value_it != object_it->end()) && value_it->is_boolean()) {
        return value_it->get<bool>();
    }
    return fallback;
}

std::string JsonString(const Json &json, const char *key) {
    const auto it = json.find(key);
    if((it != json.end()) && it->is_string()) {
        return it->get<std::string>();
    }
    return {};
}

GDriveMetadataResult QueryPrivateGDriveMetadata(const std::string &file_id, const std::string &bearer_token) {
    GDriveMetadataResult result;
    if(TryGetCachedGDriveMetadata(file_id, result)) {
        return result;
    }

    CURL *curl = AcquireThreadCurlHandle();
    if(curl == nullptr) {
        result.error_message = "Google Drive metadata: curl init failed";
        return result;
    }

    std::string body;
    HeaderList request_headers;
    request_headers.Append("Authorization: Bearer " + bearer_token);
    request_headers.Append("Accept: application/json");

    const std::string metadata_url = BuildGoogleDriveApiMetadataUrl(file_id);
    curl_easy_setopt(curl, CURLOPT_URL, metadata_url.c_str());
    ConfigureCurlTransfer(curl, "Mozilla/5.0 (Nintendo Switch) LiteFoil/2.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request_headers.headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMetadataBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);

    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if(rc != CURLE_OK) {
        result.error_message = std::string("Google Drive metadata failed: ") + curl_easy_strerror(rc);
        return result;
    }

    if(http_code == 401 || http_code == 403) {
        result.auth_failure = true;
        result.error_message = "Google Drive authentication failed (HTTP " + std::to_string(http_code) + ")";
        return result;
    }

    if(http_code == 404) {
        result.error_message = "Google Drive file not found or not shared with this account";
        CacheGDriveMetadata(file_id, result);
        return result;
    }

    if(http_code < 200 || http_code >= 300) {
        result.error_message = "Google Drive metadata failed (HTTP " + std::to_string(http_code) + ")";
        return result;
    }

    const Json root = Json::parse(body, nullptr, false);
    if(root.is_discarded() || !root.is_object()) {
        result.error_message = "Google Drive metadata response is invalid";
        return result;
    }

    const std::string mime_type = JsonString(root, "mimeType");
    if(mime_type.rfind("application/vnd.google-apps.", 0) == 0) {
        result.error_message = "Google Drive file is a Google Docs/Sheets/Slides document, not a downloadable file";
        CacheGDriveMetadata(file_id, result);
        return result;
    }

    if(!JsonBoolNested(root, "capabilities", "canDownload", true)) {
        result.error_message = "Google Drive account cannot download this file";
        CacheGDriveMetadata(file_id, result);
        return result;
    }

    result.success = true;
    result.downloadable = true;
    CacheGDriveMetadata(file_id, result);
    return result;
}

ResolvedDownloadUrl ResolveDownloadUrl(const std::string &url, bool force_private_refresh = false) {
    ResolvedDownloadUrl resolved{ url, false, {} };
    if(!IsGoogleDriveUrl(url)) {
        return resolved;
    }

    const std::string file_id = ExtractGoogleDriveFileId(url);
    if(file_id.empty()) {
        return resolved;
    }

    if(IsPrivateGDriveSchemeUrl(url)) {
        const auto auth = GDriveAuthManager::GetAuthorization(force_private_refresh);
        if(auth.authenticated && !auth.access_token.empty()) {
            const auto metadata = QueryPrivateGDriveMetadata(file_id, auth.access_token);
            if(!metadata.success) {
                resolved.blocked = true;
                resolved.auth_failure = metadata.auth_failure;
                resolved.error_message = metadata.error_message.empty()
                    ? "Google Drive file is not available"
                    : metadata.error_message;
                return resolved;
            }

            resolved.url = BuildGoogleDriveApiMediaUrl(file_id);
            resolved.uses_private_gdrive = true;
            resolved.bearer_token = auth.access_token;
            return resolved;
        }

        resolved.blocked = true;
        resolved.auth_failure = true;
        resolved.error_message = auth.error_message.empty()
            ? "Google Drive credentials are missing or invalid"
            : auth.error_message;
        return resolved;
    }

    resolved.url = ResolveGoogleDriveConfirmUrl(file_id);
    return resolved;
}

HeaderList BuildDownloadHeaders(const ResolvedDownloadUrl &resolved) {
    HeaderList list;
    if(resolved.uses_private_gdrive && !resolved.bearer_token.empty()) {
        list.Append("Authorization: Bearer " + resolved.bearer_token);
        list.Append("Accept: application/octet-stream");
    }
    return list;
}

bool IsAuthFailure(long status) {
    return status == 401 || status == 403;
}

// For large files, Google returns an HTML page with a confirmation token.
// This function performs a HEAD/GET to retrieve the token cookie, then returns
// the confirmed direct URL.  Expects curl global init to already be done.
std::string ResolveGoogleDriveConfirmUrlUncached(const std::string &file_id) {
    const std::string base_url = BuildGoogleDriveDirectUrl(file_id);

    // First request: get cookies (especially the download_warning token).
    // Uses the thread-local reusable handle to avoid a fresh TCP/TLS handshake.
    CURL *curl = AcquireThreadCurlHandle();
    if(!curl) return base_url;

    std::string response_body;

    curl_easy_setopt(curl, CURLOPT_URL, base_url.c_str());
    ConfigureCurlTransfer(curl, "Mozilla/5.0 (Nintendo Switch) LiteFoil/2.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        static_cast<curl_write_callback>(+[](char *ptr, size_t size, size_t nmemb, void *userdata) -> size_t {
            auto *body = static_cast<std::string *>(userdata);
            const std::size_t n = size * nmemb;
            if(body->size() < 512 * 1024) body->append(ptr, n);
            return n;  // never return 0 — that aborts curl before we find the token
        }));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    // Enable cookie engine in memory (no file).
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");

    CURLcode rc = curl_easy_perform(curl);
    if(rc != CURLE_OK) {
        return base_url;
    }

    // Check if the response is HTML (confirmation page) rather than binary data.
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    // Look for a confirmation token in the response body.
    // Google embeds it as: confirm=XXXX or id="uc-download-link" ... &confirm=XXXX
    std::string confirm_token;
    const auto confirm_pos = response_body.find("confirm=");
    if(confirm_pos != std::string::npos) {
        std::string token = response_body.substr(confirm_pos + 8);
        const auto amp = token.find('&');
        if(amp != std::string::npos) token = token.substr(0, amp);
        const auto quote = token.find('"');
        if(quote != std::string::npos) token = token.substr(0, quote);
        confirm_token = token;
    }

    if(!confirm_token.empty()) {
        return base_url + "&confirm=" + confirm_token;
    }

    return base_url;
}

// Process-wide cache: a Google Drive file_id maps to its resolved confirm URL.
// One install triggers dozens of DownloadRange calls for the same file, so
// without this cache each range would pay the HTTP round-trip again.  Entries
// are kept for the lifetime of the app — if Google invalidates the token, the
// next download attempt fails and the user retries from the UI.
std::mutex &GDriveCacheMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, std::string> &GDriveCacheMap() {
    static std::unordered_map<std::string, std::string> m;
    return m;
}

std::mutex &GDriveMetadataCacheMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, GDriveMetadataResult> &GDriveMetadataCacheMap() {
    static std::unordered_map<std::string, GDriveMetadataResult> m;
    return m;
}

bool TryGetCachedGDriveMetadata(const std::string &file_id, GDriveMetadataResult &out) {
    std::lock_guard<std::mutex> g(GDriveMetadataCacheMutex());
    auto &map = GDriveMetadataCacheMap();
    const auto it = map.find(file_id);
    if(it == map.end()) {
        return false;
    }
    out = it->second;
    return true;
}

void CacheGDriveMetadata(const std::string &file_id, const GDriveMetadataResult &result) {
    if(result.auth_failure) {
        return;
    }
    std::lock_guard<std::mutex> g(GDriveMetadataCacheMutex());
    GDriveMetadataCacheMap()[file_id] = result;
}

std::string ResolveGoogleDriveConfirmUrl(const std::string &file_id) {
    if(file_id.empty()) return BuildGoogleDriveDirectUrl(file_id);
    {
        std::lock_guard<std::mutex> g(GDriveCacheMutex());
        auto &map = GDriveCacheMap();
        const auto it = map.find(file_id);
        if(it != map.end()) {
            return it->second;
        }
    }
    const std::string resolved = ResolveGoogleDriveConfirmUrlUncached(file_id);
    {
        std::lock_guard<std::mutex> g(GDriveCacheMutex());
        GDriveCacheMap().emplace(file_id, resolved);
    }
    return resolved;
}

void EnsureDirectory(const char *path) {
    if((mkdir(path, 0777) != 0) && (errno != EEXIST)) {
        return;
    }
}

void EnsureParentDirectories(const std::string &path) {
    // Create the directory tree progressively because downloads may target nested
    // folders that do not exist yet on a fresh SD card.
    std::size_t separator = path.find('/');
    while(separator != std::string::npos) {
        const std::string partial = path.substr(0, separator);
        if(!partial.empty() && (partial != "sdmc:")) {
            EnsureDirectory(partial.c_str());
        }
        separator = path.find('/', separator + 1);
    }
}

std::string GuessExtension(const shield::catalog::QueueItem &item) {
    auto from_path = [](std::string path) {
        const auto query = path.find('?');
        if(query != std::string::npos) {
            path = path.substr(0, query);
        }
        const auto slash = path.find_last_of('/');
        const auto dot = path.find_last_of('.');
        if((dot != std::string::npos) && ((slash == std::string::npos) || (dot > slash))) {
            std::string extension = path.substr(dot);
            std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return extension;
        }
        return std::string{};
    };

    std::string extension = from_path(item.source_url);
    if(!extension.empty()) {
        return extension;
    }
    if(!item.package_format.empty()) {
        std::string lowered = item.package_format;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return "." + lowered;
    }
    return ".bin";
}

std::string BuildOutputPath(const shield::catalog::QueueItem &item, const std::string &downloads_root) {
    std::string root = downloads_root;
    while(!root.empty() && ((root.back() == '/') || (root.back() == '\\'))) {
        root.pop_back();
    }
    if(root.empty()) {
        root = "sdmc:/switch/LiteFoil/downloads";
    }

    std::string base_name = item.title_id.empty() ? item.name : item.title_id;
    std::replace(base_name.begin(), base_name.end(), ' ', '_');
    return root + "/" + base_name + GuessExtension(item);
}

size_t WriteToStream(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *context = reinterpret_cast<DownloadContext *>(userdata);
    if((context == nullptr) || context->write_error || (context->output_fp == nullptr)) {
        return 0;
    }

    const size_t total = size * nmemb;
    size_t remaining = total;
    const char *src = ptr;

    while(remaining > 0) {
        const std::size_t space = kAccumSize - context->accum_used;
        const std::size_t to_copy = remaining < space ? remaining : space;
        std::memcpy(context->accum.get() + context->accum_used, src, to_copy);
        context->accum_used += to_copy;
        src += to_copy;
        remaining -= to_copy;
        if(context->accum_used >= kAccumSize) {
            if(!FlushAccumulator(context)) {
                context->write_error = true;
                return 0;
            }
        }
    }
    return total;
}

int ProgressCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto *context = reinterpret_cast<DownloadContext *>(clientp);
    if(context == nullptr) {
        return 0;
    }

    if(context->stop_callback) {
        context->stop_reason = context->stop_callback();
        if(context->stop_reason != DownloadStopReason::None) {
            return 1;
        }
    }

    if(!context->progress_callback) {
        return 0;
    }

    const auto absolute_now = context->resume_offset + static_cast<std::uint64_t>(std::max<curl_off_t>(0, dlnow));
    const auto absolute_total = context->resume_offset + static_cast<std::uint64_t>(std::max<curl_off_t>(0, dltotal));
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - context->last_speed_sample_at).count();
    if(elapsed_ms >= 400) {
        const auto delta_bytes = absolute_now - context->last_speed_sample_bytes;
        context->bytes_per_second = (static_cast<double>(delta_bytes) * 1000.0) / static_cast<double>(elapsed_ms);
        context->last_speed_sample_at = now;
        context->last_speed_sample_bytes = absolute_now;
    }

    const auto emit_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - context->last_progress_emit_at).count();
    const bool completed = (absolute_total > 0) && (absolute_now >= absolute_total);
    const bool advanced_enough = (absolute_now >= context->last_progress_emit_bytes)
        && ((absolute_now - context->last_progress_emit_bytes) >= (64 * 1024));
    const bool should_emit = completed || (emit_elapsed_ms >= 250) || advanced_enough;
    if(!should_emit) {
        return 0;
    }

    context->last_progress_emit_at = now;
    context->last_progress_emit_bytes = absolute_now;
    context->progress_callback(DownloadProgress{
        absolute_now,
        absolute_total,
        context->bytes_per_second
    });
    return 0;
}

}

struct RangeWriteCtx {
    std::function<bool(const void*, std::size_t)> *write_fn = nullptr;
    std::function<DownloadStopReason()>            *stop_fn  = nullptr;
    bool               error       = false;
    DownloadStopReason stop_reason = DownloadStopReason::None;
    std::uint64_t      received    = 0;
    bool               saw_first_byte = false;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point first_byte_at{};
    std::chrono::steady_clock::time_point last_chunk_at{};
    std::int64_t       max_chunk_gap_ms = 0;
    std::int64_t       max_write_callback_ms = 0;
    std::uint32_t      long_chunk_gap_count = 0;
    std::uint32_t      long_write_callback_count = 0;
};

static std::size_t RangeWriteCb(char *ptr, size_t size, size_t nmemb, void *ud) {
    auto *ctx = static_cast<RangeWriteCtx*>(ud);
    if(ctx->error) return 0;
    const std::size_t n = size * nmemb;
    const auto before_write = std::chrono::steady_clock::now();
    if(!ctx->saw_first_byte) {
        ctx->saw_first_byte = true;
        ctx->first_byte_at = before_write;
    }
    else {
        const auto gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            before_write - ctx->last_chunk_at).count();
        ctx->max_chunk_gap_ms = std::max<std::int64_t>(ctx->max_chunk_gap_ms, gap_ms);
        if(gap_ms >= 500) {
            ctx->long_chunk_gap_count++;
        }
    }
    ctx->last_chunk_at = before_write;
    if(!(*ctx->write_fn)(ptr, n)) { ctx->error = true; return 0; }
    const auto after_write = std::chrono::steady_clock::now();
    const auto write_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        after_write - before_write).count();
    ctx->max_write_callback_ms = std::max<std::int64_t>(ctx->max_write_callback_ms, write_ms);
    if(write_ms >= 500) {
        ctx->long_write_callback_count++;
    }
    ctx->received += n;
    return n;
}

static int RangeStopCb(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto *ctx = static_cast<RangeWriteCtx*>(clientp);
    if(ctx->stop_fn && *ctx->stop_fn) {
        ctx->stop_reason = (*ctx->stop_fn)();
        if(ctx->stop_reason != DownloadStopReason::None) return 1;
    }
    return 0;
}

bool AuthorizedDownloadProvider::CanHandle(const shield::catalog::QueueItem &item) {
    if(item.source_url.empty()) return false;
    return StartsWithHttp(item.source_url) || IsGoogleDriveUrl(item.source_url);
}

DownloadResult AuthorizedDownloadProvider::Download(const shield::catalog::QueueItem &item,
    const std::string &downloads_root,
    std::function<void(const DownloadProgress &)> progress_callback,
    std::function<DownloadStopReason()> stop_callback) {
    DownloadResult result;
    if(!CanHandle(item)) {
        result.error_message = "Unsupported or empty source URL";
        return result;
    }

    if(!EnsureCurlGlobalInit()) {
        result.error_message = "curl runtime init failed";
        return result;
    }

    EnsureDirectory("sdmc:/switch");
    EnsureDirectory("sdmc:/switch/LiteFoil");
    EnsureDirectory("sdmc:/switch/LiteFoil/downloads");

    result.output_path = BuildOutputPath(item, downloads_root);
    EnsureParentDirectories(result.output_path);

    if(IsGDriveCryptSchemeUrl(item.source_url) && !IsRcloneCryptEnabled()) {
        const RcloneCryptConfig crypt_config = GetRcloneCryptConfig();
        result.error_message = crypt_config.error_message.empty()
            ? "rclone crypt is disabled for gdrivecrypt source"
            : crypt_config.error_message;
        return result;
    }

    if(ShouldUseRcloneCryptForUrl(item.source_url)) {
        if(item.size == 0) {
            result.error_message = "rclone crypt download requires plaintext size in catalog";
            return result;
        }

        DownloadContext context;
        context.accum = std::make_unique<char[]>(kAccumSize);
        context.stdio_buffer = std::make_unique<char[]>(kStdioBufferSize);
        context.stop_callback = stop_callback;
        context.progress_callback = progress_callback;
        context.last_speed_sample_at = std::chrono::steady_clock::now();
        context.last_progress_emit_at = std::chrono::steady_clock::now();

        {
            std::ifstream existing(result.output_path, std::ios::binary | std::ios::ate);
            if(existing.good()) {
                context.resume_offset = static_cast<std::uint64_t>(existing.tellg());
            }
        }
        if((context.resume_offset > 0) && (item.bytes_done != context.resume_offset)) {
            context.resume_offset = 0;
            std::remove(result.output_path.c_str());
        }
        if(context.resume_offset >= item.size) {
            result.bytes_done = item.size;
            result.bytes_total = item.size;
            result.success = true;
            return result;
        }

        context.output_fp = std::fopen(result.output_path.c_str(), context.resume_offset > 0 ? "ab" : "wb");
        if(context.output_fp == nullptr) {
            result.error_message = "Failed to open output file";
            return result;
        }
        std::setvbuf(context.output_fp, context.stdio_buffer.get(), _IOFBF, kStdioBufferSize);

        std::uint64_t total_written = context.resume_offset;
        const std::uint64_t bytes_to_read = item.size - context.resume_offset;
        const bool ok = AuthorizedDownloadProvider::DownloadRange(item.source_url, context.resume_offset, bytes_to_read,
            [&context, &total_written, item](const void *data, std::size_t data_size) {
                const char *src = static_cast<const char *>(data);
                std::size_t remaining = data_size;
                while(remaining > 0) {
                    const std::size_t space = kAccumSize - context.accum_used;
                    const std::size_t to_copy = remaining < space ? remaining : space;
                    std::memcpy(context.accum.get() + context.accum_used, src, to_copy);
                    context.accum_used += to_copy;
                    src += to_copy;
                    remaining -= to_copy;
                    if(context.accum_used >= kAccumSize && !FlushAccumulator(&context)) {
                        context.write_error = true;
                        return false;
                    }
                }
                total_written += data_size;
                if(context.progress_callback) {
                    context.progress_callback(DownloadProgress{total_written, item.size, 0.0});
                }
                return true;
            }, stop_callback);

        if(!context.write_error && !FlushAccumulator(&context)) {
            context.write_error = true;
        }
        std::fclose(context.output_fp);
        context.output_fp = nullptr;

        result.bytes_done = total_written;
        result.bytes_total = item.size;
        result.stop_reason = stop_callback ? stop_callback() : DownloadStopReason::None;
        if(result.stop_reason == DownloadStopReason::Canceled) {
            std::remove(result.output_path.c_str());
            result.error_message = "Download canceled";
            return result;
        }
        if(result.stop_reason == DownloadStopReason::Paused) {
            result.error_message = "Download paused";
            return result;
        }
        if(!ok || context.write_error || total_written != item.size) {
            result.error_message = "rclone crypt download failed";
            std::remove(result.output_path.c_str());
            return result;
        }
        result.success = true;
        return result;
    }

    // Resolve Google Drive URLs once before the retry loop.  The explicit
    // gdrive:/gdrivecrypt: schemes use private Drive API access; public
    // drive.google.com links keep using the confirm-token flow below.
    ResolvedDownloadUrl resolved_url = ResolveDownloadUrl(item.source_url);
    if(resolved_url.blocked) {
        result.error_message = resolved_url.error_message.empty()
            ? "Google Drive download is not available"
            : resolved_url.error_message;
        return result;
    }

    for(int attempt = 0; attempt < kMaxRetryCount; attempt++) {
        DownloadContext context;
        context.accum = std::make_unique<char[]>(kAccumSize);
        context.stdio_buffer = std::make_unique<char[]>(kStdioBufferSize);
        context.stop_callback = stop_callback;
        context.progress_callback = progress_callback;
        context.last_speed_sample_at = std::chrono::steady_clock::now();
        context.last_speed_sample_bytes = 0;
        context.last_progress_emit_at = std::chrono::steady_clock::now();
        context.last_progress_emit_bytes = 0;
        context.resume_offset = 0;

        // Re-read the partial file size at the start of every attempt so that
        // bytes written by the previous attempt are correctly resumed.
        {
            std::ifstream existing(result.output_path, std::ios::binary | std::ios::ate);
            if(existing.good()) {
                context.resume_offset = static_cast<std::uint64_t>(existing.tellg());
            }
        }

        // On the first attempt only, validate that the partial file matches the
        // persisted queue state.  If they diverge, restart cleanly.
        if(attempt == 0) {
            if((context.resume_offset > 0) && (item.bytes_done != context.resume_offset)) {
                context.resume_offset = 0;
                std::remove(result.output_path.c_str());
            }
        }

        // Use FILE* so setvbuf takes effect before any I/O.
        context.output_fp = std::fopen(result.output_path.c_str(),
                                        context.resume_offset > 0 ? "ab" : "wb");
        if(context.output_fp == nullptr) {
            result.error_message = "Failed to open output file";
            return result;
        }
        std::setvbuf(context.output_fp, context.stdio_buffer.get(), _IOFBF, kStdioBufferSize);

        // Reusable per-thread handle: keeps the TCP/TLS connection across retries
        // and across the preceding gdrive-resolve call, cutting handshake costs.
        CURL *curl = AcquireThreadCurlHandle();
        if(curl == nullptr) {
            std::fclose(context.output_fp);
            result.error_message = "curl init failed";
            return result;
        }

        HeaderList request_headers = BuildDownloadHeaders(resolved_url);

        curl_easy_setopt(curl, CURLOPT_URL, resolved_url.url.c_str());
        ConfigureCurlTransfer(curl, "Mozilla/5.0 (Nintendo Switch) LiteFoil/2.0");
        if(request_headers.headers != nullptr) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request_headers.headers);
        }
        if(resolved_url.uses_private_gdrive) {
            curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        }
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, static_cast<long>(kCurlRecvSize));
        curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, static_cast<long>(LITEFOIL_DOWNLOAD_TCP_NODELAY));
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToStream);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        if(context.resume_offset > 0) {
            curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                             static_cast<curl_off_t>(context.resume_offset));
        }

        const CURLcode rc = curl_easy_perform(curl);
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        curl_off_t downloaded_bytes = 0;
        curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &downloaded_bytes);
        // NOTE: the handle is intentionally NOT cleaned up — it is reused on
        // the next retry and on subsequent DownloadRange calls so curl can
        // keep the connection alive and avoid repeated TLS handshakes.

        if(!context.write_error && !FlushAccumulator(&context)) {
            context.write_error = true;
        }
        std::fclose(context.output_fp);
        context.output_fp = nullptr;

        result.bytes_done = context.resume_offset
            + static_cast<std::uint64_t>(std::max<curl_off_t>(0, downloaded_bytes));
        result.bytes_total = item.size > 0 ? item.size : result.bytes_done;
        result.bytes_per_second = context.bytes_per_second;
        result.stop_reason = context.stop_reason;

        // Stop reasons are never retried.
        if(context.stop_reason == DownloadStopReason::Canceled) {
            std::remove(result.output_path.c_str());
            result.error_message = "Download canceled";
            return result;
        }
        if(context.stop_reason == DownloadStopReason::Paused) {
            result.error_message = "Download paused";
            return result;
        }

        // 416 means the server rejected our resume offset — discard the partial
        // file so the next attempt starts from the beginning.
        if(response_code == 416) {
            std::remove(result.output_path.c_str());
            if(attempt + 1 < kMaxRetryCount) {
                SleepRetryBackoff(attempt);
                continue;
            }
            result.error_message = "HTTP 416 (server rejected resume)";
            return result;
        }

        const bool curl_ok = (rc == CURLE_OK);
        const bool http_ok  = (response_code >= 200) && (response_code < 300);

        // Private Drive tokens can expire between queue creation and transfer.
        // Refresh once in-place and retry without discarding the partial file.
        if(resolved_url.uses_private_gdrive && IsAuthFailure(response_code) && attempt + 1 < kMaxRetryCount) {
            resolved_url = ResolveDownloadUrl(item.source_url, true);
            if(resolved_url.blocked) {
                result.error_message = resolved_url.error_message.empty()
                    ? "Google Drive download is not available"
                    : resolved_url.error_message;
                return result;
            }
            SleepRetryBackoff(attempt);
            continue;
        }

        if(curl_ok && http_ok) {
            // Verify the final file size against the catalog-reported size.
            if(item.size > 0) {
                std::ifstream size_check(result.output_path, std::ios::binary | std::ios::ate);
                if(size_check.good()) {
                    const auto final_size = static_cast<std::uint64_t>(size_check.tellg());
                    if(final_size != item.size) {
                        if(final_size > item.size) {
                            // Overrun: discard and retry from scratch.
                            std::remove(result.output_path.c_str());
                        }
                        // Under-run: keep partial and retry with resume.
                        if(attempt + 1 < kMaxRetryCount) {
                            SleepRetryBackoff(attempt);
                            continue;
                        }
                        result.error_message = "Download completed with unexpected size";
                        return result;
                    }
                }
            }
            result.success = true;
            return result;
        }

        // Decide whether the error is worth retrying.
        const bool retryable = IsRetryableCurlError(rc) || IsRetryableHttpStatus(response_code);
        if(!retryable || attempt + 1 >= kMaxRetryCount) {
            std::remove(result.output_path.c_str());
            result.error_message = curl_ok
                ? ("HTTP " + std::to_string(response_code))
                : curl_easy_strerror(rc);
            return result;
        }

        // Retryable failure: wait and loop.  Keep the partial file on disk so
        // the next attempt can resume from where curl stopped.
        SleepRetryBackoff(attempt);
    }

    std::remove(result.output_path.c_str());
    result.error_message = "Download failed after multiple attempts";
    return result;
}

static bool DownloadRangeRaw(
    const std::string &url,
    std::uint64_t offset,
    std::uint64_t size,
    std::function<bool(const void*, std::size_t)> write_fn,
    std::function<DownloadStopReason()> stop_fn,
    bool allow_short_success = false) {

    if(!EnsureCurlGlobalInit()) return false;

    static std::atomic<std::uint64_t> next_range_id{1};
    const std::uint64_t range_id = next_range_id.fetch_add(1, std::memory_order_relaxed);

    ResolvedDownloadUrl resolved_url = ResolveDownloadUrl(url);
    if(resolved_url.blocked) {
        RuntimeLog("[range] resolve blocked: %s", resolved_url.error_message.c_str());
        return false;
    }

    // Track bytes successfully delivered to write_fn across retry attempts.
    // Each attempt resumes from (offset + delivered) so write_fn always
    // receives a contiguous, non-overlapping byte stream.
    std::uint64_t delivered = 0;

    for(int attempt = 0; attempt < kMaxRetryCount; attempt++) {
        if(attempt > 0) {
            SleepRetryBackoff(attempt - 1);
        }

        // Reusable per-thread handle: DownloadRange is called many times per
        // install (one or more ranges per NCA/NCZ plus PFS0/HFS0 parsing reads);
        // reusing the connection turns dozens of TLS handshakes into a single one.
        // AcquireThreadCurlHandle resets the handle, so each attempt starts clean.
        CURL *curl = AcquireThreadCurlHandle();
        if(!curl) return false;

        const std::uint64_t attempt_offset = offset + delivered;
        const std::uint64_t attempt_size   = (size > 0) ? (size - delivered) : 0;

        RangeWriteCtx ctx;
        ctx.write_fn = &write_fn;
        ctx.stop_fn  = stop_fn ? &stop_fn : nullptr;
        ctx.started_at = std::chrono::steady_clock::now();

        HeaderList request_headers = BuildDownloadHeaders(resolved_url);

        curl_easy_setopt(curl, CURLOPT_URL, resolved_url.url.c_str());
        ConfigureCurlTransfer(curl, "Mozilla/5.0 (Nintendo Switch) LiteFoil/2.0");
        if(request_headers.headers != nullptr) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request_headers.headers);
        }
        if(resolved_url.uses_private_gdrive) {
            curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        }
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, static_cast<long>(kCurlRecvSize));
        curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, static_cast<long>(LITEFOIL_DOWNLOAD_TCP_NODELAY));
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, RangeWriteCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

        if(attempt_size > 0) {
            const std::string range_str = std::to_string(attempt_offset) + "-" +
                                          std::to_string(attempt_offset + attempt_size - 1);
            curl_easy_setopt(curl, CURLOPT_RANGE, range_str.c_str());
        } else if(attempt_offset > 0 && size == 0) {
            const std::string range_str = std::to_string(attempt_offset) + "-";
            curl_easy_setopt(curl, CURLOPT_RANGE, range_str.c_str());
        }

        if(stop_fn) {
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, RangeStopCb);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        } else {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        }

        RuntimeLog("[range] attempt start id=%llu attempt=%d offset=%llu size=%llu delivered=%llu attempt_offset=%llu attempt_size=%llu",
            static_cast<unsigned long long>(range_id), attempt + 1,
            static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(delivered),
            static_cast<unsigned long long>(attempt_offset),
            static_cast<unsigned long long>(attempt_size));

        const CURLcode rc = curl_easy_perform(curl);
        const auto finished_at = std::chrono::steady_clock::now();
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        long new_connections = 0;
        curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &new_connections);
        double name_lookup_s = 0.0;
        double connect_s = 0.0;
        double appconnect_s = 0.0;
        double starttransfer_s = 0.0;
        double total_s = 0.0;
        double curl_speed = 0.0;
        curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &name_lookup_s);
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect_s);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &appconnect_s);
        curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &starttransfer_s);
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_s);
        curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD, &curl_speed);
        // Handle intentionally kept alive — see AcquireThreadCurlHandle contract.

        delivered += ctx.received;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            finished_at - ctx.started_at).count();
        const auto first_byte_ms = ctx.saw_first_byte
            ? std::chrono::duration_cast<std::chrono::milliseconds>(ctx.first_byte_at - ctx.started_at).count()
            : -1;
        const double avg_bps = elapsed_ms > 0
            ? (static_cast<double>(ctx.received) * 1000.0) / static_cast<double>(elapsed_ms)
            : 0.0;
        RuntimeLog("[range] attempt end id=%llu attempt=%d rc=%d http=%ld received=%llu delivered=%llu/%llu elapsed_ms=%lld first_byte_ms=%lld avg_bps=%.0f curl_bps=%.0f new_conn=%ld timings_ms=%.0f/%.0f/%.0f/%.0f/%.0f max_gap_ms=%lld gap500=%u max_write_ms=%lld write500=%u error=%d stop=%u",
            static_cast<unsigned long long>(range_id), attempt + 1,
            static_cast<int>(rc), http_code,
            static_cast<unsigned long long>(ctx.received),
            static_cast<unsigned long long>(delivered),
            static_cast<unsigned long long>(size),
            static_cast<long long>(elapsed_ms),
            static_cast<long long>(first_byte_ms),
            avg_bps,
            curl_speed,
            new_connections,
            name_lookup_s * 1000.0,
            connect_s * 1000.0,
            appconnect_s * 1000.0,
            starttransfer_s * 1000.0,
            total_s * 1000.0,
            static_cast<long long>(ctx.max_chunk_gap_ms),
            ctx.long_chunk_gap_count,
            static_cast<long long>(ctx.max_write_callback_ms),
            ctx.long_write_callback_count,
            static_cast<int>(ctx.error),
            static_cast<unsigned int>(ctx.stop_reason));

        // write_fn signalled failure or stop was requested: abort without retry.
        if(ctx.error || ctx.stop_reason != DownloadStopReason::None) return false;

        if(resolved_url.uses_private_gdrive && IsAuthFailure(http_code) && attempt + 1 < kMaxRetryCount) {
            resolved_url = ResolveDownloadUrl(url, true);
            if(resolved_url.blocked) {
                return false;
            }
            continue;
        }

        // Open-ended range (size == 0): can't resume, accept any successful transfer.
        if(size == 0) {
            return rc == CURLE_OK && (http_code == 200 || http_code == 206);
        }

        // Wrong HTTP status: never retry (404, policy errors, etc.).
        if(http_code != 200 && http_code != 206) {
            RuntimeLog("[range] HTTP unexpected code=%ld rc=%d delivered=%llu received=%llu",
                http_code, static_cast<int>(rc), static_cast<unsigned long long>(delivered),
                static_cast<unsigned long long>(ctx.received));
            return false;
        }

        // All bytes delivered: success.
        if(delivered >= size) return true;
        if(allow_short_success && rc == CURLE_OK && ctx.received > 0) return true;

        // Partial delivery: retry from delivered if attempts remain.
        // Retryable when curl reported a recoverable error or when the server
        // closed the connection cleanly after a short-send (CURLE_OK but under-run).
        const bool retryable = IsRetryableCurlError(rc) || (rc == CURLE_OK && ctx.received > 0);
        if(!retryable || attempt + 1 >= kMaxRetryCount) {
            RuntimeLog("[range] exhausted rc=%d http=%ld delivered=%llu/%llu received=%llu retryable=%d",
                static_cast<int>(rc), http_code, static_cast<unsigned long long>(delivered),
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(ctx.received),
                static_cast<int>(retryable));
            return false;
        }
        // Loop: next attempt resumes at offset + delivered.
    }

    return false;
}

bool AuthorizedDownloadProvider::DownloadRange(
    const std::string &url,
    std::uint64_t offset,
    std::uint64_t size,
    std::function<bool(const void*, std::size_t)> write_fn,
    std::function<DownloadStopReason()> stop_fn) {

    RcloneCryptConfig crypt_config = GetRcloneCryptConfig();
    if(IsGDriveCryptSchemeUrl(url) && !crypt_config.enabled) {
        RuntimeLog("[range] gdrivecrypt requested while rclone is disabled: %s",
            crypt_config.error_message.c_str());
        return false;
    }
    crypt_config.enabled = crypt_config.enabled && IsGDriveCryptSchemeUrl(url);
    RuntimeLog("[range] request offset=%llu size=%llu rclone=%d url=%s",
        static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size),
        static_cast<int>(crypt_config.enabled), url.c_str());
    if(!crypt_config.enabled) {
        const bool ok = DownloadRangeRaw(url, offset, size, std::move(write_fn), std::move(stop_fn));
        RuntimeLog("[range] raw result offset=%llu size=%llu ok=%d",
            static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size),
            static_cast<int>(ok));
        return ok;
    }

    RawRangeReader raw_reader = [url, stop_fn](std::uint64_t encrypted_offset,
                                std::uint64_t encrypted_size,
                                std::function<bool(const void*, std::size_t)> raw_write_fn) -> bool {
        RuntimeLog("[range] crypt raw request enc_offset=%llu enc_size=%llu",
            static_cast<unsigned long long>(encrypted_offset),
            static_cast<unsigned long long>(encrypted_size));
        const bool ok = DownloadRangeRaw(url, encrypted_offset, encrypted_size, std::move(raw_write_fn), stop_fn, true);
        RuntimeLog("[range] crypt raw result enc_offset=%llu enc_size=%llu ok=%d",
            static_cast<unsigned long long>(encrypted_offset),
            static_cast<unsigned long long>(encrypted_size), static_cast<int>(ok));
        return ok;
    };
    const bool ok = RcloneCryptReadDecryptedRange(crypt_config, raw_reader, offset, size, std::move(write_fn));
    RuntimeLog("[range] crypt result offset=%llu size=%llu ok=%d",
        static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size),
        static_cast<int>(ok));
    return ok;
}

}
