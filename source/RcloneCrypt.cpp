#include <platform/RcloneCrypt.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <mbedtls/aes.h>
#include <mbedtls/platform_util.h>
#include <sodium.h>

#include <platform/ExitLog.hpp>

namespace shield::platform {
namespace {

constexpr std::uint64_t kFileMagicSize = 8;
constexpr std::uint64_t kFileNonceSize = 24;
constexpr std::uint64_t kFileHeaderSize = kFileMagicSize + kFileNonceSize;
constexpr std::uint64_t kBlockHeaderSize = 16;
constexpr std::uint64_t kBlockDataSize = 64 * 1024;
constexpr std::uint64_t kBlockSize = kBlockHeaderSize + kBlockDataSize;
constexpr std::uint64_t kBatchBlocks = 64;
constexpr std::size_t kDerivedKeySize = 80;
constexpr char kFileMagic[] = "RCLONE\0\0";

constexpr std::array<unsigned char, 16> kDefaultSalt = {
    0xA8, 0x0D, 0xF4, 0x3A, 0x8F, 0xBD, 0x03, 0x08,
    0xA7, 0xCA, 0xB8, 0x3E, 0x58, 0x1F, 0x86, 0xB1
};

constexpr std::array<unsigned char, 32> kObscureKey = {
    0x9c, 0x93, 0x5b, 0x48, 0x73, 0x0a, 0x55, 0x4d,
    0x6b, 0xfd, 0x7c, 0x63, 0xc8, 0x86, 0xa9, 0x2b,
    0xd3, 0x90, 0x19, 0x8e, 0xb8, 0x12, 0x8a, 0xfb,
    0xf4, 0xde, 0x16, 0x2b, 0x8b, 0x95, 0xf6, 0x38
};

std::mutex &ConfigMutex() {
    static std::mutex mutex;
    return mutex;
}

RcloneCryptConfig &ActiveConfig() {
    static RcloneCryptConfig config;
    return config;
}

int Base64UrlValue(char c) {
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if(c >= '0' && c <= '9') return 52 + (c - '0');
    if(c == '-' || c == '+') return 62;
    if(c == '_' || c == '/') return 63;
    return -1;
}

bool DecodeBase64RawUrl(const std::string &input, std::vector<unsigned char> &out) {
    out.clear();
    unsigned int accumulator = 0;
    int bits = 0;
    for(char c : input) {
        if(c == '=') {
            break;
        }
        const int value = Base64UrlValue(c);
        if(value < 0) {
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<unsigned int>(value);
        bits += 6;
        if(bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xffU));
        }
    }
    return true;
}

bool RevealObscuredPassword(const std::string &input, std::string &out) {
    std::vector<unsigned char> ciphertext;
    if(!DecodeBase64RawUrl(input, ciphertext) || ciphertext.size() < 16) {
        return false;
    }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    bool ok = mbedtls_aes_setkey_enc(&aes, kObscureKey.data(), 256) == 0;
    if(ok) {
        unsigned char stream_block[16] = {};
        unsigned char iv[16] = {};
        std::size_t nc_off = 0;
        std::memcpy(iv, ciphertext.data(), sizeof(iv));
        std::vector<unsigned char> plaintext(ciphertext.size() - 16);
        ok = mbedtls_aes_crypt_ctr(&aes, plaintext.size(), &nc_off, iv, stream_block,
            ciphertext.data() + 16, plaintext.data()) == 0;
        if(ok) {
            out.assign(reinterpret_cast<const char *>(plaintext.data()), plaintext.size());
            mbedtls_platform_zeroize(plaintext.data(), plaintext.size());
        }
        mbedtls_platform_zeroize(stream_block, sizeof(stream_block));
        mbedtls_platform_zeroize(iv, sizeof(iv));
    }
    mbedtls_aes_free(&aes);
    return ok;
}

bool DeriveDataKey(const RcloneCryptConfig &config, std::array<unsigned char, 32> &data_key) {
    struct Cache {
        bool valid = false;
        std::string password;
        std::string password2;
        std::array<unsigned char, 32> key{};
    };
    static std::mutex cache_mutex;
    static Cache cache;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if(cache.valid
            && cache.password == config.password
            && cache.password2 == config.password2) {
            data_key = cache.key;
            return true;
        }
    }

    std::string password = config.password;
    std::string salt = config.password2;
    if(!password.empty() && !RevealObscuredPassword(password, password)) {
        return false;
    }
    if(!salt.empty() && !RevealObscuredPassword(salt, salt)) {
        return false;
    }

    std::array<unsigned char, kDerivedKeySize> derived{};
    if(password.empty()) {
        derived.fill(0);
    }
    else {
        const unsigned char *salt_ptr = kDefaultSalt.data();
        std::size_t salt_len = kDefaultSalt.size();
        if(!salt.empty()) {
            salt_ptr = reinterpret_cast<const unsigned char *>(salt.data());
            salt_len = salt.size();
        }
        if(crypto_pwhash_scryptsalsa208sha256_ll(
            reinterpret_cast<const unsigned char *>(password.data()), password.size(),
            salt_ptr, salt_len, 16384, 8, 1, derived.data(), derived.size()) != 0) {
            return false;
        }
    }

    std::copy_n(derived.begin(), data_key.size(), data_key.begin());
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache.valid = true;
        cache.password = config.password;
        cache.password2 = config.password2;
        cache.key = data_key;
    }
    mbedtls_platform_zeroize(derived.data(), derived.size());
    if(!password.empty()) {
        mbedtls_platform_zeroize(password.data(), password.size());
    }
    if(!salt.empty()) {
        mbedtls_platform_zeroize(salt.data(), salt.size());
    }
    return true;
}

void AddNonceBlocks(std::array<unsigned char, 24> &nonce, std::uint64_t blocks) {
    unsigned int carry = 0;
    for(std::size_t i = 0; i < 8; ++i) {
        const unsigned int digit = nonce[i];
        const unsigned int add = static_cast<unsigned int>(blocks & 0xffU);
        blocks >>= 8;
        const unsigned int sum = digit + add + carry;
        nonce[i] = static_cast<unsigned char>(sum & 0xffU);
        carry = sum >> 8;
    }
    for(std::size_t i = 8; carry != 0 && i < nonce.size(); ++i) {
        const unsigned int sum = nonce[i] + carry;
        nonce[i] = static_cast<unsigned char>(sum & 0xffU);
        carry = sum >> 8;
    }
}

bool FetchRangeToVector(const RawRangeReader &reader, std::uint64_t offset, std::uint64_t size, std::vector<unsigned char> &out) {
    out.clear();
    out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(size, kBlockSize)));
    return reader(offset, size, [&out](const void *data, std::size_t data_size) {
        const auto *bytes = static_cast<const unsigned char *>(data);
        out.insert(out.end(), bytes, bytes + data_size);
        return true;
    });
}

}

RcloneCryptConfig LoadRcloneCryptConfig(const shield::app::AppConfig &app_config) {
    RcloneCryptConfig config;
    config.enabled = app_config.rclone_crypt_enabled;
    config.password = app_config.rclone_crypt_password;
    config.password2 = app_config.rclone_crypt_password2;
    config.catalog_sizes = app_config.rclone_crypt_catalog_sizes.empty()
        ? "plain"
        : app_config.rclone_crypt_catalog_sizes;

    if(!config.enabled) {
        return config;
    }

    if(config.password.empty()) {
        config.enabled = false;
        config.error_message = "rclone crypt password is missing in config.json";
    }
    return config;
}

void ConfigureRcloneCrypt(const RcloneCryptConfig &config) {
    std::lock_guard<std::mutex> lock(ConfigMutex());
    ActiveConfig() = config;
}

RcloneCryptConfig GetRcloneCryptConfig() {
    std::lock_guard<std::mutex> lock(ConfigMutex());
    return ActiveConfig();
}

bool IsRcloneCryptEnabled() {
    return GetRcloneCryptConfig().enabled;
}

std::uint64_t RcloneCryptDecryptedSize(std::uint64_t encrypted_size, bool *ok) {
    if(ok != nullptr) {
        *ok = false;
    }
    if(encrypted_size < kFileHeaderSize) {
        return 0;
    }
    encrypted_size -= kFileHeaderSize;
    const std::uint64_t blocks = encrypted_size / kBlockSize;
    std::uint64_t residue = encrypted_size % kBlockSize;
    std::uint64_t plain = blocks * kBlockDataSize;
    if(residue != 0) {
        if(residue <= kBlockHeaderSize) {
            return 0;
        }
        residue -= kBlockHeaderSize;
        plain += residue;
    }
    if(ok != nullptr) {
        *ok = true;
    }
    return plain;
}

std::uint64_t RcloneCryptEncryptedSize(std::uint64_t plaintext_size) {
    const std::uint64_t blocks = plaintext_size / kBlockDataSize;
    const std::uint64_t residue = plaintext_size % kBlockDataSize;
    std::uint64_t encrypted = kFileHeaderSize + blocks * kBlockSize;
    if(residue != 0) {
        encrypted += kBlockHeaderSize + residue;
    }
    return encrypted;
}

bool RcloneCryptReadDecryptedRange(
    const RcloneCryptConfig &config,
    const RawRangeReader &raw_reader,
    std::uint64_t offset,
    std::uint64_t size,
    std::function<bool(const void *, std::size_t)> write_fn) {

    if(!config.enabled || !raw_reader || !write_fn) {
        RuntimeLog("[rclone] invalid read request enabled=%d reader=%d write=%d",
            static_cast<int>(config.enabled), static_cast<int>(static_cast<bool>(raw_reader)),
            static_cast<int>(static_cast<bool>(write_fn)));
        return false;
    }
    RuntimeLog("[rclone] enter offset=%llu size=%llu",
        static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size));

    std::array<unsigned char, 32> data_key{};
    if(!DeriveDataKey(config, data_key)) {
        RuntimeLog("[rclone] DeriveDataKey failed");
        return false;
    }

    std::vector<unsigned char> header;
    if(!FetchRangeToVector(raw_reader, 0, kFileHeaderSize, header) || header.size() != kFileHeaderSize) {
        RuntimeLog("[rclone] encrypted header read failed got=%zu", header.size());
        return false;
    }
    if(std::memcmp(header.data(), kFileMagic, kFileMagicSize) != 0) {
        RuntimeLog("[rclone] bad file magic");
        return false;
    }

    std::array<unsigned char, 24> nonce{};
    std::copy_n(header.data() + kFileMagicSize, nonce.size(), nonce.begin());

    const std::uint64_t first_block = offset / kBlockDataSize;
    const std::uint64_t discard = offset % kBlockDataSize;
    std::uint64_t blocks_to_read = 1;
    if(size > 0) {
        const std::uint64_t first_available = kBlockDataSize - discard;
        if(size > first_available) {
            const std::uint64_t remaining = size - first_available;
            blocks_to_read += (remaining + kBlockDataSize - 1) / kBlockDataSize;
        }
    }

    AddNonceBlocks(nonce, first_block);
    std::uint64_t bytes_left = size;
    std::uint64_t current_encrypted_offset = kFileHeaderSize + first_block * kBlockSize;
    RuntimeLog("[rclone] read plain offset=%llu size=%llu first_block=%llu blocks=%llu discard=%llu enc_offset=%llu",
        static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size),
        static_cast<unsigned long long>(first_block), static_cast<unsigned long long>(blocks_to_read),
        static_cast<unsigned long long>(discard), static_cast<unsigned long long>(current_encrypted_offset));

    std::uint64_t block_index = 0;
    while(block_index < blocks_to_read) {
        const std::uint64_t batch_blocks = std::min<std::uint64_t>(kBatchBlocks, blocks_to_read - block_index);
        std::vector<unsigned char> encrypted_batch;
        if(!FetchRangeToVector(raw_reader, current_encrypted_offset, batch_blocks * kBlockSize, encrypted_batch)) {
            RuntimeLog("[rclone] encrypted batch read failed index=%llu enc_offset=%llu got=%zu",
                static_cast<unsigned long long>(block_index),
                static_cast<unsigned long long>(current_encrypted_offset), encrypted_batch.size());
            return false;
        }

        std::size_t batch_offset = 0;
        for(std::uint64_t batch_index = 0; batch_index < batch_blocks; ++batch_index) {
            const std::size_t bytes_remaining = encrypted_batch.size() - batch_offset;
            if(bytes_remaining <= kBlockHeaderSize) {
                RuntimeLog("[rclone] encrypted block too small index=%llu remaining=%zu",
                    static_cast<unsigned long long>(block_index), bytes_remaining);
                return false;
            }

            std::size_t encrypted_block_size = static_cast<std::size_t>(std::min<std::uint64_t>(kBlockSize, bytes_remaining));
            if(encrypted_block_size <= kBlockHeaderSize) {
                return false;
            }

            const unsigned char *encrypted_block = encrypted_batch.data() + batch_offset;
            std::vector<unsigned char> plain_block(encrypted_block_size - kBlockHeaderSize);
            if(crypto_secretbox_open_easy(plain_block.data(), encrypted_block, encrypted_block_size, nonce.data(), data_key.data()) != 0) {
                RuntimeLog("[rclone] secretbox open failed index=%llu enc_size=%zu",
                    static_cast<unsigned long long>(block_index), encrypted_block_size);
                return false;
            }

            const std::size_t start = (block_index == 0) ? static_cast<std::size_t>(discard) : 0;
            if(start > plain_block.size()) {
                return false;
            }
            std::size_t available = plain_block.size() - start;
            if(size > 0) {
                available = static_cast<std::size_t>(std::min<std::uint64_t>(available, bytes_left));
            }
            if(available > 0 && !write_fn(plain_block.data() + start, available)) {
                RuntimeLog("[rclone] write_fn failed index=%llu available=%zu",
                    static_cast<unsigned long long>(block_index), available);
                return false;
            }
            if(size > 0) {
                bytes_left -= available;
                if(bytes_left == 0) {
                    return true;
                }
            }
            else if(plain_block.size() < kBlockDataSize) {
                return true;
            }

            AddNonceBlocks(nonce, 1);
            current_encrypted_offset += encrypted_block_size;
            batch_offset += encrypted_block_size;
            block_index++;
        }
    }

    return size == 0 || bytes_left == 0;
}

}
