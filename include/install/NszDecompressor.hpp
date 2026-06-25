#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

#include <install/CryptoUtils.hpp>
#include <install/NcmWrapper.hpp>

namespace shield::install {

using NczProgressCallback = std::function<void(std::uint64_t, std::uint64_t)>;
using NczStopCallback = std::function<bool()>;

// Reader abstraction matching PackageRangeReader in InstallEngine.hpp.
// Delivers bytes [offset, offset+size) chunk-by-chunk to write_fn.
using NczRangeReader = std::function<bool(
    std::uint64_t offset,
    std::uint64_t size,
    std::function<bool(const void *, std::size_t)> write_fn)>;

// NCZ section magic ("NCZSECTN") – marks the start of NCZ metadata.
static constexpr std::uint64_t kNczSectionMagic = 0x4E544345535A434EULL;

// NCZ block magic ("NCZBLOCK") – optional header for block-based compression.
static constexpr std::uint64_t kNczBlockMagic   = 0x4B434F4C425A434EULL;

// Modern NCZ stores the first 0x4000 bytes uncompressed (NCA header + start of
// section data).  Legacy NCZ used 0xC00 (NCA header only).
static constexpr std::size_t kNczModernPrefix = 0x4000;
static constexpr std::size_t kNczLegacyPrefix = 0xC00;

// Check whether a file entry appears to be a zstd-compressed NCA (.ncz).
bool IsCompressedNca(const std::string &container_path, std::uint64_t nca_offset);

// Decompress a .ncz (compressed NCA) from |container_path| at |nca_offset| with
// |compressed_size| into a temporary file, returning the path on success.
// The decompressed data is re-encrypted with the section crypto info so the
// resulting NCA can be written directly to content storage.
// The caller is responsible for deleting the temporary file after use.
// Returns an empty string on failure.
std::string DecompressNczToTemp(const std::string &container_path,
                                 std::uint64_t nca_offset,
                                 std::uint64_t compressed_size,
                                 const std::string &temp_dir,
                                 NczProgressCallback progress_callback = {},
                                 NczStopCallback stop_callback = {});

// Decompress a .ncz directly into NCM content storage, avoiding a temporary
// full-size file on the SD card. Returns true on success.
bool DecompressNczToStorage(const std::string &container_path,
                             std::uint64_t nca_offset,
                             std::uint64_t compressed_size,
                             ContentStorage &storage,
                             const NcmContentId &nca_id,
                             const HeaderKey &header_key,
                             NczProgressCallback progress_callback = {},
                             NczStopCallback stop_callback = {});

// Decompress a .ncz directly from a range reader into NCM content storage.
// No temporary file is written to SD card — all data flows through memory.
// Returns true on success; falls back gracefully to false so the caller may
// retry with the temp-file path.
bool DecompressNczFromRangeReaderToStorage(
    const NczRangeReader &reader,
    std::uint64_t entry_offset,
    std::uint64_t entry_size,
    ContentStorage &storage,
    const NcmContentId &nca_id,
    const HeaderKey &header_key,
    NczProgressCallback progress_callback = {},
    NczStopCallback stop_callback = {});

}
