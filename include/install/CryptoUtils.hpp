#pragma once

#include <cstdint>
#include <cstddef>

namespace shield::install {

// Fixed RSA-2048 public key used by Nintendo to sign NCA headers (signature 0).
extern const unsigned char kNcaHeaderSignatureModulus[0x100];

// Key sources for deriving the NCA header key via SPL (Security Processor).
extern const unsigned char kHeaderKekSource[0x10];
extern const unsigned char kHeaderKeySource[0x20];

struct HeaderKey {
    unsigned char key[0x20];
};

// Derive the NCA header decryption key using splCrypto services.
// Returns true on success.  The resulting 32-byte XTS key is written to |out|.
bool DeriveHeaderKey(HeaderKey &out);

// Decrypt the first 0xC00 bytes of an NCA header in-place using AES-128-XTS
// with the derived header key.  |header| must point to at least kNcaHeaderSize bytes.
void DecryptNcaHeader(void *header, std::size_t length, const HeaderKey &key);

// Re-encrypt an NCA header in-place using AES-128-XTS (inverse of DecryptNcaHeader).
void EncryptNcaHeader(void *header, std::size_t length, const HeaderKey &key);

// Verify the RSA-2048-PSS signature stored in the first 0x100 bytes of the NCA
// header against the data starting at offset 0x200 (magic field onward).
// |data|      – pointer to the 0x200 bytes starting at NcaHeader::magic
// |len|       – should be 0x200
// |signature| – the 0x100-byte fixed_key_sig
// |modulus|   – the 0x100-byte RSA public key modulus (usually kNcaHeaderSignatureModulus)
bool Rsa2048PssVerify(const void *data, std::size_t len,
                      const unsigned char *signature,
                      const unsigned char *modulus);

}
