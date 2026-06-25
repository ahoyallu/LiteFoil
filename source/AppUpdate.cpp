#include <platform/AppUpdate.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <curl/curl.h>
#include <mbedtls/base64.h>
#include <minizip/unzip.h>
#include <switch.h>

#include <platform/CurlRuntime.hpp>
#include <platform/ExitLog.hpp>
#include <platform/RomfsRuntime.hpp>
#include <platform/TinfoilDecryptor.hpp>
#include <third_party/nlohmann/json.hpp>

namespace shield::platform {
namespace {

using Json = nlohmann::json;

constexpr const char *kAppDir = "sdmc:/switch/LiteFoil";
constexpr const char *kTempDir = "sdmc:/switch/LiteFoil/tmp";
constexpr const char *kDownloadPath = "sdmc:/switch/LiteFoil/LiteFoil.update";
constexpr const char *kExtractedNroPath = "sdmc:/switch/LiteFoil/LiteFoil.update.nro";
constexpr const char *kOldPreparedNroPath = "sdmc:/switch/LiteFoil/tmp/LiteFoil.pending.nro";
constexpr const char *kFallbackNroPath = "sdmc:/switch/LiteFoil/LiteFoil.nro";
constexpr const char *kFallbackNextLoadPath = "/switch/LiteFoil/LiteFoil.nro";
constexpr const char *kUserAgent = "LiteFoil";

struct DownloadContext {
    std::FILE *file = nullptr;
    std::function<void(const AppUpdateProgress &)> progress_callback;
    Aes128CtrContext *aes = nullptr;
    curl_off_t last_total = 0;
    curl_off_t last_now = 0;
};

void EnsureDirectory(const char *path) {
    if((mkdir(path, 0777) != 0) && (errno != EEXIST)) {
        return;
    }
}

void EnsureUpdateDirectory() {
    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kAppDir);
}

void RemoveQuiet(const std::string &path) {
    if(!path.empty()) {
        std::remove(path.c_str());
    }
}

bool FileExists(const std::string &path) {
    struct stat st {};
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string Trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](const unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](const unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if(first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::vector<int> ParseVersionParts(const std::string &version) {
    std::vector<int> parts;
    std::size_t start = 0;
    while(start <= version.size()) {
        const std::size_t dot = version.find('.', start);
        const std::string token = version.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if(token.empty() || !std::all_of(token.begin(), token.end(), [](const unsigned char c) { return std::isdigit(c) != 0; })) {
            return {};
        }
        parts.push_back(std::atoi(token.c_str()));
        if(dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return parts;
}

std::string JsonString(const Json &json, const char *key) {
    const auto it = json.find(key);
    if((it != json.end()) && it->is_string()) {
        return Trim(it->get<std::string>());
    }
    return {};
}

size_t WriteStringCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *output = static_cast<std::string *>(userdata);
    const std::size_t bytes = size * nmemb;
    if(output != nullptr && output->size() < 1024 * 1024) {
        output->append(ptr, bytes);
    }
    return bytes;
}

size_t WriteFileCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *context = static_cast<DownloadContext *>(userdata);
    const std::size_t bytes = size * nmemb;
    if(context == nullptr || context->file == nullptr) {
        return 0;
    }
    if(context->aes != nullptr) {
        std::vector<unsigned char> decrypted(bytes);
        aes128CtrCrypt(context->aes, decrypted.data(), ptr, bytes);
        return std::fwrite(decrypted.data(), 1, bytes, context->file);
    }
    return std::fwrite(ptr, 1, bytes, context->file);
}

int DownloadProgressCallback(void *userdata, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto *context = static_cast<DownloadContext *>(userdata);
    if(context == nullptr) {
        return 0;
    }
    if(total > 0) {
        context->last_total = std::max(context->last_total, total);
    }
    if(now > 0) {
        context->last_now = std::max(context->last_now, now);
    }
    if(context->progress_callback) {
        AppUpdateProgress progress;
        progress.stage = "download";
        progress.bytes_done = static_cast<std::uint64_t>(std::max<curl_off_t>(0, context->last_now));
        progress.bytes_total = static_cast<std::uint64_t>(std::max<curl_off_t>(0, context->last_total));
        progress.ratio = context->last_total > 0
            ? std::min(0.90, static_cast<double>(context->last_now) / static_cast<double>(context->last_total) * 0.90)
            : 0.0;
        context->progress_callback(progress);
    }
    return 0;
}

bool FetchJson(const std::string &url, Json &json, std::string &error) {
    if(url.empty()) {
        return false;
    }
    CURL *curl = AcquireThreadCurlHandle();
    if(curl == nullptr) {
        error = "curl init failed";
        return false;
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);

    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if(rc != CURLE_OK) {
        error = curl_easy_strerror(rc);
        return false;
    }
    if(http_code >= 400) {
        error = "HTTP " + std::to_string(http_code);
        return false;
    }
    const auto decoded = TryDecodeRemoteJsonPayload(response, false);
    const std::string &effective = decoded.has_value() ? decoded.value() : response;
    json = Json::parse(effective, nullptr, false);
    if(json.is_discarded() || !json.is_object()) {
        error = "invalid update JSON";
        return false;
    }
    return true;
}

bool IsMegaUrl(const std::string &url) {
    return url.find("mega.nz") != std::string::npos;
}

std::string MegaId(const std::string &url) {
    const auto old_link = url.find("#!") != std::string::npos;
    if(old_link) {
        const std::size_t start = url.find("#!") + 2;
        const std::size_t end = url.find('!', start);
        return end == std::string::npos ? std::string{} : url.substr(start, end - start);
    }
    const std::string marker = "/file/";
    const std::size_t start = url.find(marker);
    if(start == std::string::npos) {
        return {};
    }
    const std::size_t id_start = start + marker.size();
    const std::size_t end = url.find('#', id_start);
    return end == std::string::npos ? std::string{} : url.substr(id_start, end - id_start);
}

std::string MegaNodeKey(const std::string &url) {
    std::string key;
    const auto old_link = url.find("#!") != std::string::npos;
    if(old_link) {
        const std::size_t start = url.find_last_of('!');
        if(start != std::string::npos) {
            key = url.substr(start + 1);
        }
    }
    else {
        const std::size_t start = url.find('#');
        if(start != std::string::npos) {
            key = url.substr(start + 1);
        }
    }
    std::replace(key.begin(), key.end(), '_', '/');
    std::replace(key.begin(), key.end(), '-', '+');
    const std::size_t remainder = key.size() % 4;
    if(remainder != 0) {
        key.append(4 - remainder, '=');
    }
    if(key.empty()) {
        return {};
    }

    std::string decoded(key.size() * 3 / 4, '\0');
    size_t output_len = 0;
    const int rc = mbedtls_base64_decode(
        reinterpret_cast<unsigned char *>(decoded.data()), decoded.size(), &output_len,
        reinterpret_cast<const unsigned char *>(key.data()), key.size());
    if(rc != 0) {
        return {};
    }
    decoded.resize(output_len);
    return decoded;
}

std::string MegaKey(const std::string &node_key) {
    if(node_key.size() != 32) {
        return {};
    }
    std::string key(16, '\0');
    for(std::size_t i = 0; i < 8; ++i) {
        key[i] = static_cast<char>(node_key[i] ^ node_key[i + 16]);
        key[i + 8] = static_cast<char>(node_key[i + 8] ^ node_key[i + 24]);
    }
    return key;
}

std::string MegaIv(const std::string &node_key) {
    if(node_key.size() != 32) {
        return {};
    }
    std::string iv(16, '\0');
    std::memcpy(iv.data(), node_key.data() + 16, 8);
    return iv;
}

bool ResolveMegaUrl(const std::string &url, std::string &resolved_url, std::string &error) {
    const std::string id = MegaId(url);
    if(id.empty()) {
        error = "invalid Mega URL";
        return false;
    }
    const Json request = Json::array({{
        { "a", "g" },
        { "g", 1 },
        { "p", id }
    }});

    CURL *curl = AcquireThreadCurlHandle();
    if(curl == nullptr) {
        error = "curl init failed";
        return false;
    }
    const std::string body = request.dump();
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, "https://g.api.mega.co.nz/cs");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    const CURLcode rc = curl_easy_perform(curl);
    if(rc != CURLE_OK) {
        error = curl_easy_strerror(rc);
        return false;
    }
    const Json json = Json::parse(response, nullptr, false);
    if(json.is_discarded() || !json.is_array() || json.empty() || !json[0].is_object()) {
        error = "download failed";
        return false;
    }
    resolved_url = JsonString(json[0], "g");
    if(resolved_url.empty()) {
        error = "Mega link unavailable";
        return false;
    }
    return true;
}

bool DownloadToFile(const std::string &url,
    const std::string &path,
    std::function<void(const AppUpdateProgress &)> progress_callback,
    std::string &error) {
    EnsureUpdateDirectory();
    RemoveQuiet(path);

    std::string real_url = url;
    Aes128CtrContext aes {};
    Aes128CtrContext *aes_ptr = nullptr;
    if(IsMegaUrl(url)) {
        std::string mega_error;
        if(!ResolveMegaUrl(url, real_url, mega_error)) {
            error = mega_error;
            return false;
        }
        const std::string node_key = MegaNodeKey(url);
        const std::string key = MegaKey(node_key);
        const std::string iv = MegaIv(node_key);
        if(key.empty() || iv.empty()) {
            error = "invalid Mega key";
            return false;
        }
        aes128CtrContextCreate(&aes, key.data(), reinterpret_cast<const unsigned char *>(iv.data()));
        aes_ptr = &aes;
    }

    std::FILE *file = std::fopen(path.c_str(), "wb");
    if(file == nullptr) {
        error = "failed to create update file";
        return false;
    }

    CURL *curl = AcquireThreadCurlHandle();
    if(curl == nullptr) {
        std::fclose(file);
        RemoveQuiet(path);
        error = "curl init failed";
        return false;
    }

    DownloadContext context { file, std::move(progress_callback), aes_ptr, 0, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, real_url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, DownloadProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);

    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    std::fclose(file);

    if(rc != CURLE_OK) {
        RemoveQuiet(path);
        error = curl_easy_strerror(rc);
        return false;
    }
    if(http_code >= 400) {
        RemoveQuiet(path);
        error = "HTTP " + std::to_string(http_code);
        return false;
    }
    return true;
}

bool IsZipFile(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 4> signature {};
    file.read(reinterpret_cast<char *>(signature.data()), signature.size());
    return file.good()
        && signature[0] == 0x50
        && signature[1] == 0x4B
        && signature[2] == 0x03
        && signature[3] == 0x04;
}

bool IsNroFile(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if(!file.good()) {
        return false;
    }
    file.seekg(0x10);
    std::array<char, 4> magic {};
    file.read(magic.data(), magic.size());
    return file.good()
        && magic[0] == 'N'
        && magic[1] == 'R'
        && magic[2] == 'O'
        && magic[3] == '0';
}

bool HasNroExtension(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return name.size() >= 4 && name.substr(name.size() - 4) == ".nro";
}

bool IsSafeZipPath(const std::string &name) {
    return !name.empty()
        && name.front() != '/'
        && name.front() != '\\'
        && name.find("..") == std::string::npos;
}

bool ExtractFirstNroFromZip(const std::string &zip_path,
    const std::string &output_path,
    std::function<void(const AppUpdateProgress &)> progress_callback,
    std::string &error) {
    RemoveQuiet(output_path);
    unzFile zip = unzOpen64(zip_path.c_str());
    if(zip == nullptr) {
        error = "failed to open update zip";
        return false;
    }

    unz_global_info64 global_info {};
    if(unzGetGlobalInfo64(zip, &global_info) != UNZ_OK || unzGoToFirstFile(zip) != UNZ_OK) {
        unzClose(zip);
        error = "invalid update zip";
        return false;
    }

    std::array<char, 1024> filename {};
    bool found = false;
    for(ZPOS64_T index = 0; index < global_info.number_entry; ++index) {
        filename.fill('\0');
        unz_file_info64 file_info {};
        if(unzGetCurrentFileInfo64(zip, &file_info, filename.data(), filename.size(), nullptr, 0, nullptr, 0) != UNZ_OK) {
            unzClose(zip);
            error = "failed to read update zip";
            return false;
        }

        const std::string entry_name(filename.data());
        if(IsSafeZipPath(entry_name) && HasNroExtension(entry_name)) {
            found = true;
            if(unzOpenCurrentFile(zip) != UNZ_OK) {
                unzClose(zip);
                error = "failed to open nro in zip";
                return false;
            }
            std::FILE *out = std::fopen(output_path.c_str(), "wb");
            if(out == nullptr) {
                unzCloseCurrentFile(zip);
                unzClose(zip);
                error = "failed to create extracted nro";
                return false;
            }
            std::array<char, 0x4000> buffer {};
            ZPOS64_T extracted = 0;
            int read = 0;
            while((read = unzReadCurrentFile(zip, buffer.data(), buffer.size())) > 0) {
                if(std::fwrite(buffer.data(), 1, static_cast<std::size_t>(read), out) != static_cast<std::size_t>(read)) {
                    std::fclose(out);
                    unzCloseCurrentFile(zip);
                    unzClose(zip);
                    RemoveQuiet(output_path);
                    error = "failed to write extracted nro";
                    return false;
                }
                extracted += static_cast<ZPOS64_T>(read);
                if(progress_callback) {
                    AppUpdateProgress progress;
                    progress.stage = "extract";
                    progress.bytes_done = static_cast<std::uint64_t>(extracted);
                    progress.bytes_total = static_cast<std::uint64_t>(file_info.uncompressed_size);
                    progress.ratio = file_info.uncompressed_size > 0
                        ? 0.90 + std::min(0.08, static_cast<double>(extracted) / static_cast<double>(file_info.uncompressed_size) * 0.08)
                        : 0.95;
                    progress_callback(progress);
                }
            }
            std::fclose(out);
            const int close_rc = unzCloseCurrentFile(zip);
            unzClose(zip);
            if(read < 0 || close_rc != UNZ_OK) {
                RemoveQuiet(output_path);
                error = "failed to extract nro";
                return false;
            }
            return FileExists(output_path);
        }

        if(index + 1 < global_info.number_entry && unzGoToNextFile(zip) != UNZ_OK) {
            break;
        }
    }

    unzClose(zip);
    error = found ? "failed to extract nro" : "nro not found in update zip";
    return false;
}

std::string StripSdPrefix(const std::string &path) {
    return path.rfind("sdmc:", 0) == 0 ? path.substr(5) : path;
}

std::string AddSdPrefix(const std::string &path) {
    if(path.rfind("sdmc:", 0) == 0) {
        return path;
    }
    if(!path.empty() && path.front() == '/') {
        return "sdmc:" + path;
    }
    return path;
}

bool InstallNro(const std::string &source_path, const std::string &target_path, std::string &error) {
    RuntimeLog("[app-update] InstallNro: enter source=%s target=%s",
        source_path.c_str(), target_path.c_str());
    if(source_path.empty() || target_path.empty() || !FileExists(source_path)) {
        RuntimeLog("[app-update] InstallNro: source/target invalid source_exists=%d",
            static_cast<int>(FileExists(source_path)));
        error = "downloaded nro not found";
        return false;
    }

    RuntimeLog("[app-update] InstallNro: before ExitRomfs");
    ExitRomfs();
    RuntimeLog("[app-update] InstallNro: after ExitRomfs");
    const std::string backup_path = target_path + ".bak";
    RuntimeLog("[app-update] InstallNro: remove backup=%s", backup_path.c_str());
    RemoveQuiet(backup_path);

    if(FileExists(target_path) && std::rename(target_path.c_str(), backup_path.c_str()) != 0) {
        RuntimeLog("[app-update] InstallNro: rename target->backup failed errno=%d", errno);
        error = "failed to replace current nro (errno " + std::to_string(errno) + ": " + std::strerror(errno) + ")";
        return false;
    }
    RuntimeLog("[app-update] InstallNro: rename target->backup ok");
    if(std::rename(source_path.c_str(), target_path.c_str()) != 0) {
        RuntimeLog("[app-update] InstallNro: rename source->target failed errno=%d", errno);
        if(FileExists(backup_path)) {
            RuntimeLog("[app-update] InstallNro: restoring backup");
            std::rename(backup_path.c_str(), target_path.c_str());
        }
        error = "failed to install new nro (errno " + std::to_string(errno) + ": " + std::strerror(errno) + ")";
        return false;
    }
    RuntimeLog("[app-update] InstallNro: rename source->target ok");
    RemoveQuiet(backup_path);
    const bool installed = FileExists(target_path);
    RuntimeLog("[app-update] InstallNro: exit installed=%d", static_cast<int>(installed));
    return installed;
}

}

void AppUpdate::CleanupCache() {
    RuntimeLog("[app-update] CleanupCache: enter");
    RemoveQuiet(kDownloadPath);
    RemoveQuiet(kExtractedNroPath);
    RemoveQuiet(kOldPreparedNroPath);
    rmdir(kTempDir);
    RuntimeLog("[app-update] CleanupCache: exit");
}

int AppUpdate::CompareVersions(const std::string &left, const std::string &right) {
    const std::vector<int> left_parts = ParseVersionParts(Trim(left));
    const std::vector<int> right_parts = ParseVersionParts(Trim(right));
    if(left_parts.empty() || right_parts.empty()) {
        return 0;
    }
    const std::size_t count = std::max(left_parts.size(), right_parts.size());
    for(std::size_t i = 0; i < count; ++i) {
        const int a = i < left_parts.size() ? left_parts[i] : 0;
        const int b = i < right_parts.size() ? right_parts[i] : 0;
        if(a < b) return -1;
        if(a > b) return 1;
    }
    return 0;
}

AppUpdateInfo AppUpdate::Check(const std::string &update_url, const std::string &local_version) {
    AppUpdateInfo info;
    RuntimeLog("[app-update] Check: enter local=%s has_url=%d",
        local_version.c_str(), static_cast<int>(!update_url.empty()));
    if(update_url.empty()) {
        return info;
    }

    Json root;
    std::string error;
    if(!FetchJson(update_url, root, error)) {
        RuntimeLog("[app-update] Check: FetchJson failed error=%s", error.c_str());
        info.message = error;
        return info;
    }

    const auto appfoil = root.find("appfoil");
    if((appfoil == root.end()) || !appfoil->is_object()) {
        RuntimeLog("[app-update] Check: missing appfoil block");
        info.message = "invalid update metadata";
        return info;
    }

    const Json *block = &(*appfoil);
    info.version = JsonString(*block, "version");
    info.url = JsonString(*block, "url");
    if(info.version.empty() || info.url.empty()) {
        RuntimeLog("[app-update] Check: missing version/url version_empty=%d url_empty=%d",
            static_cast<int>(info.version.empty()), static_cast<int>(info.url.empty()));
        info.message = "invalid update metadata";
        return info;
    }
    info.available = CompareVersions(info.version, local_version) > 0;
    RuntimeLog("[app-update] Check: remote=%s available=%d",
        info.version.c_str(), static_cast<int>(info.available));
    return info;
}

AppUpdateResult AppUpdate::DownloadAndInstall(const AppUpdateInfo &info,
    std::function<void(const AppUpdateProgress &)> progress_callback) {
    RuntimeLog("[app-update] DownloadAndInstall: enter version=%s has_url=%d",
        info.version.c_str(), static_cast<int>(!info.url.empty()));
    if(info.url.empty()) {
        return { false, "update URL is missing", {} };
    }

    CleanupCache();
    std::string error;
    RuntimeLog("[app-update] DownloadAndInstall: before download path=%s", kDownloadPath);
    if(!DownloadToFile(info.url, kDownloadPath, progress_callback, error)) {
        RuntimeLog("[app-update] DownloadAndInstall: download failed error=%s", error.c_str());
        CleanupCache();
        return { false, error, {} };
    }
    RuntimeLog("[app-update] DownloadAndInstall: download ok");

    std::string nro_path = kDownloadPath;
    if(IsZipFile(kDownloadPath)) {
        RuntimeLog("[app-update] DownloadAndInstall: update is zip, extracting to %s", kExtractedNroPath);
        if(!ExtractFirstNroFromZip(kDownloadPath, kExtractedNroPath, progress_callback, error)) {
            RuntimeLog("[app-update] DownloadAndInstall: extract failed error=%s", error.c_str());
            CleanupCache();
            return { false, error, {} };
        }
        nro_path = kExtractedNroPath;
        RuntimeLog("[app-update] DownloadAndInstall: extract ok");
    }
    if(!IsNroFile(nro_path)) {
        RuntimeLog("[app-update] DownloadAndInstall: invalid NRO path=%s", nro_path.c_str());
        CleanupCache();
        return { false, "downloaded update is not a valid NRO", {} };
    }
    RuntimeLog("[app-update] DownloadAndInstall: NRO validation ok path=%s", nro_path.c_str());

    if(progress_callback) {
        progress_callback(AppUpdateProgress{ 1, 1, 0.99, "install" });
    }

    const std::string target_path = GetCurrentNroPath();
    RuntimeLog("[app-update] DownloadAndInstall: target=%s", target_path.c_str());
    if(!InstallNro(nro_path, target_path, error)) {
        RuntimeLog("[app-update] DownloadAndInstall: install failed error=%s", error.c_str());
        CleanupCache();
        return { false, error, {} };
    }
    RuntimeLog("[app-update] DownloadAndInstall: install ok");
    CleanupCache();
    if(progress_callback) {
        progress_callback(AppUpdateProgress{ 1, 1, 1.0, "done" });
    }
    RuntimeLog("[app-update] DownloadAndInstall: success installed=%s", target_path.c_str());
    return { true, "updated", target_path };
}

std::string AppUpdate::GetCurrentNroPath() {
    RuntimeLog("[app-update] GetCurrentNroPath: envHasArgv=%d", static_cast<int>(envHasArgv()));
    if(envHasArgv()) {
        const char *argv = static_cast<const char *>(envGetArgv());
        if(argv != nullptr && argv[0] != '\0') {
            std::string raw = argv;
            if(raw.front() == '"') {
                const std::size_t end_quote = raw.find('"', 1);
                if(end_quote != std::string::npos) {
                    const std::string path = AddSdPrefix(raw.substr(1, end_quote - 1));
                    RuntimeLog("[app-update] GetCurrentNroPath: quoted path=%s", path.c_str());
                    return path;
                }
            }
            const std::size_t space = raw.find(' ');
            if(space != std::string::npos) {
                raw = raw.substr(0, space);
            }
            if(!raw.empty()) {
                const std::string path = AddSdPrefix(raw);
                RuntimeLog("[app-update] GetCurrentNroPath: argv path=%s", path.c_str());
                return path;
            }
        }
    }
    RuntimeLog("[app-update] GetCurrentNroPath: fallback=%s", kFallbackNroPath);
    return kFallbackNroPath;
}

std::string AppUpdate::GetNextLoadPath(const std::string &nro_path) {
    const std::string normalized = StripSdPrefix(nro_path);
    const std::string next = normalized.empty() ? kFallbackNextLoadPath : normalized;
    RuntimeLog("[app-update] GetNextLoadPath: input=%s next=%s", nro_path.c_str(), next.c_str());
    return next;
}

}
