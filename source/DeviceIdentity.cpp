#include <platform/DeviceIdentity.hpp>

#include <array>
#include <mutex>

#include <switch.h>

namespace shield::platform {

std::string DeviceIdentity::ComputeUidFromMmcCid() {
    static std::once_flag once;
    static std::string uid(64, '0');

    std::call_once(once, []() {
        FsDeviceOperator device_operator = {};
        if(R_FAILED(fsOpenDeviceOperator(&device_operator))) {
            return;
        }

        std::array<unsigned char, 16> mmc_cid = {};
        const Result rc = fsDeviceOperatorGetMmcCid(&device_operator, mmc_cid.data(), mmc_cid.size(), static_cast<s64>(mmc_cid.size()));
        fsDeviceOperatorClose(&device_operator);
        if(R_FAILED(rc)) {
            return;
        }

        // CyberFoil derives a stable per-console UID from the internal MMC CID, so we mirror that behavior here.
        std::array<unsigned char, 32> hash = {};
        sha256CalculateHash(hash.data(), mmc_cid.data(), mmc_cid.size());

        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(hash.size() * 2);
        for(const unsigned char byte : hash) {
            out.push_back(kHex[(byte >> 4) & 0xF]);
            out.push_back(kHex[byte & 0xF]);
        }

        std::fill(mmc_cid.begin(), mmc_cid.end(), 0);
        std::fill(hash.begin(), hash.end(), 0);
        uid = out;
    });

    return uid;
}

}
