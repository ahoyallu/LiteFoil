#pragma once

#include <optional>
#include <string>

namespace shield::platform {

// Encrypted remote-index decoding is disabled in this build because private
// key material is not stored in the project.
std::optional<std::string> TryTinfoilDecrypt(const std::string &raw);

std::optional<std::string> TryLiteFoilEncryptedJsonDecrypt(const std::string &raw);
std::optional<std::string> TryLiteFoilBase64ZlibEncryptedJsonDecrypt(const std::string &raw);

std::optional<std::string> TryDecodeRemoteJsonPayload(const std::string &raw, bool allow_tinfoil);

}
