#pragma once

#include <string>

namespace shield::platform {

class DeviceIdentity {
    public:
        static std::string ComputeUidFromMmcCid();
};

}
