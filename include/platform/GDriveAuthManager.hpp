#pragma once

#include <string>

namespace shield::platform {

struct GDriveAuthorization {
    bool authenticated = false;
    std::string access_token;
    std::string error_message;
};

class GDriveAuthManager {
    public:
        // Returns a valid OAuth access token loaded from local files.  If the
        // saved token is expired or rejected by Google, the caller can request a
        // forced refresh without exposing credentials to the queue or logs.
        static GDriveAuthorization GetAuthorization(bool force_refresh = false);
};

}
