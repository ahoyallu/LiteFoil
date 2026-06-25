#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <app/AppConfig.hpp>

namespace shield::platform {

struct RcloneCryptConfig {
    bool enabled = false;
    std::string password;
    std::string password2;
    std::string catalog_sizes = "plain";
    std::string error_message;
};

using RawRangeReader = std::function<bool(
    std::uint64_t offset,
    std::uint64_t size,
    std::function<bool(const void *, std::size_t)> write_fn)>;

RcloneCryptConfig LoadRcloneCryptConfig(const shield::app::AppConfig &app_config);

void ConfigureRcloneCrypt(const RcloneCryptConfig &config);
RcloneCryptConfig GetRcloneCryptConfig();
bool IsRcloneCryptEnabled();

std::uint64_t RcloneCryptDecryptedSize(std::uint64_t encrypted_size, bool *ok = nullptr);
std::uint64_t RcloneCryptEncryptedSize(std::uint64_t plaintext_size);

bool RcloneCryptReadDecryptedRange(
    const RcloneCryptConfig &config,
    const RawRangeReader &raw_reader,
    std::uint64_t offset,
    std::uint64_t size,
    std::function<bool(const void *, std::size_t)> write_fn);

}
