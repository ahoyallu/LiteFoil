#pragma once

#include <functional>
#include <string>

namespace shield::platform {

struct RemoteCatalogFetchResult {
    bool success = false;
    long response_code = 0;
    std::string body;
    std::string error_message;
};

class RemoteCatalogClient {
    public:
        static RemoteCatalogFetchResult Fetch(const std::string &url,
            const std::string &language_tag,
            const std::string &device_uid,
            const std::string &username,
            const std::string &password,
            const std::string &client_version,
            const std::string &client_revision,
            std::function<bool()> cancel_callback = {});
};

}
