#include <platform/RemoteCatalogClient.hpp>
#include <platform/CurlRuntime.hpp>

#include <curl/curl.h>

#include <array>
#include <sstream>
#include <string>

namespace shield::platform {
namespace {

struct FetchControl {
    std::function<bool()> cancel_callback;
};

size_t WriteToString(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *output = reinterpret_cast<std::string *>(userdata);
    if(output == nullptr) {
        return 0;
    }

    output->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string BuildLanguageHeaderValue(const std::string &language_tag) {
    if(language_tag.empty()) {
        return "Language: en-US";
    }

    return "Language: " + language_tag;
}

void BuildVersionAndRevision(const std::string &configured_version, const std::string &configured_revision, std::string &version, std::string &revision) {
    const std::string raw = configured_version.empty() ? "20.0" : configured_version;
    version = raw.empty() ? "20.0" : raw;
    revision = configured_revision.empty() ? "0" : configured_revision;

    const std::size_t first_dot = raw.find('.');
    if(first_dot == std::string::npos) {
        return;
    }

    const std::size_t second_dot = raw.find('.', first_dot + 1);
    if(second_dot == std::string::npos) {
        version = raw;
        return;
    }

    version = raw.substr(0, second_dot);
}

std::string BuildHauthHeaderValue(const std::string &) {
    // CyberFoil can optionally derive a host-bound HAUTH token at build time.
    // Until a private seed is provided for LiteFoil, we keep protocol compatibility
    // by sending the header with the neutral fallback value used by CyberFoil.
    return "HAUTH: 0";
}

int CancelCallback(void *userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto *control = reinterpret_cast<FetchControl *>(userdata);
    if((control == nullptr) || !control->cancel_callback) {
        return 0;
    }

    return control->cancel_callback() ? 1 : 0;
}

std::array<std::string, 7> BuildShopHeaders(const std::string &url, const std::string &language_tag, const std::string &device_uid, const std::string &client_version, const std::string &client_revision) {
    std::string version;
    std::string revision;
    BuildVersionAndRevision(client_version, client_revision, version, revision);

    return {
        "Theme: 0000000000000000000000000000000000000000000000000000000000000000",
        "UID: " + device_uid,
        "Version: " + version,
        "Revision: " + revision,
        BuildLanguageHeaderValue(language_tag),
        BuildHauthHeaderValue(url),
        "UAUTH: 0"
    };
}

}

RemoteCatalogFetchResult RemoteCatalogClient::Fetch(const std::string &url,
    const std::string &language_tag,
    const std::string &device_uid,
    const std::string &username,
    const std::string &password,
    const std::string &client_version,
    const std::string &client_revision,
    std::function<bool()> cancel_callback) {
    RemoteCatalogFetchResult result;

    CURL *curl = AcquireThreadCurlHandle();
    if(curl == nullptr) {
        result.error_message = "curl init failed";
        return result;
    }

    std::string response_body;
    struct curl_slist *headers = nullptr;
    const auto shop_headers = BuildShopHeaders(url, language_tag, device_uid, client_version, client_revision);
    for(const auto &header : shop_headers) {
        headers = curl_slist_append(headers, header.c_str());
    }
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "litefoil");
    // Pin HTTP/1.1: libcurl's HTTP/2 path on devkitPro has observed stalls.
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                     static_cast<long>(CURL_HTTP_VERSION_1_1));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    FetchControl fetch_control{std::move(cancel_callback)};
    if(fetch_control.cancel_callback) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CancelCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &fetch_control);
    }

    if(!username.empty() || !password.empty()) {
        const std::string basic_auth = username + ":" + password;
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERPWD, basic_auth.c_str());
    }

    const CURLcode perform_rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.response_code);

    if((perform_rc == CURLE_OK) && (result.response_code >= 200) && (result.response_code < 300)) {
        result.success = true;
        result.body = std::move(response_body);
    }
    else {
        std::ostringstream error;
        if(perform_rc == CURLE_ABORTED_BY_CALLBACK) {
            error << "catalog fetch canceled";
        }
        else if(perform_rc != CURLE_OK) {
            error << "catalog fetch failed";
            error << " (" << curl_easy_strerror(perform_rc) << ")";
        }
        else {
            error << "catalog fetch failed";
            error << " (HTTP " << result.response_code << ")";
        }

        result.error_message = error.str();
        result.body = std::move(response_body);
    }

    if(headers != nullptr) {
        curl_slist_free_all(headers);
    }
    // Handle intentionally kept alive — see AcquireThreadCurlHandle contract.
    return result;
}

}
