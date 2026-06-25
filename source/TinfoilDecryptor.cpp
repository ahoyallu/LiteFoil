#include <platform/TinfoilDecryptor.hpp>

namespace shield::platform {

std::optional<std::string> TryTinfoilDecrypt(const std::string &) {
    return std::nullopt;
}

std::optional<std::string> TryLiteFoilEncryptedJsonDecrypt(const std::string &) {
    return std::nullopt;
}

std::optional<std::string> TryLiteFoilBase64ZlibEncryptedJsonDecrypt(const std::string &) {
    return std::nullopt;
}

std::optional<std::string> TryDecodeRemoteJsonPayload(const std::string &, bool) {
    return std::nullopt;
}

}
