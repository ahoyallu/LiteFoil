#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <switch/services/ncm.h>

namespace shield::install {

// Progress callback emitted during NCA streaming.
struct InstallProgress {
    std::uint64_t bytes_done       = 0;
    std::uint64_t bytes_total      = 0;
    std::string   current_nca;     // name of the NCA being installed
    std::uint32_t nca_index    = 0; // 1-based index
    std::uint32_t nca_count    = 0; // total NCAs
    bool          decompressing    = false;
    double        bytes_per_second = 0.0;
};

using ProgressCallback = std::function<void(const InstallProgress &)>;
using StopCallback = std::function<bool()>;

// Result returned by the install engine.
struct InstallResult {
    bool        success = false;
    std::string error_message;
    std::uint64_t title_id = 0;
};

// Configuration knobs passed into the install engine.
struct InstallConfig {
    NcmStorageId  dest_storage_id  = NcmStorageId_SdCard;
    bool          verify_nca_sigs  = true;   // verify RSA-PSS header signature
    bool          allow_unsigned   = false;   // allow install if sig fails
    bool          ignore_req_fw    = true;    // zero required firmware version
    bool          reinstall_ncas   = false;   // install even if NCA already exists
};

// Entry point for the install engine.  Detects format from the file extension
// and delegates to the appropriate installer (NSP/XCI).
InstallResult InstallFromLocalFile(const std::string &file_path,
                                    const InstallConfig &config,
                                    ProgressCallback progress = nullptr);

// Format-specific installers (exposed for direct use if needed).
InstallResult InstallNsp(const std::string &file_path,
                          const InstallConfig &config,
                          ProgressCallback progress);

InstallResult InstallXci(const std::string &file_path,
                          const InstallConfig &config,
                          ProgressCallback progress);

// Abstraction over a byte-range source (HTTP range request or local file seek).
// Requests bytes [offset, offset+size) and delivers them chunk-by-chunk to write_fn.
// Returns true when all bytes were delivered and write_fn never returned false.
using PackageRangeReader = std::function<bool(
    std::uint64_t offset,
    std::uint64_t size,
    std::function<bool(const void *, std::size_t)> write_fn)>;

// Stream-install directly from a range-readable source (e.g. HTTP URL).
// No package file is written to SD card; each NCA is streamed directly to
// NCM content storage.  NCZ entries use a small per-NCA temp file that is
// removed immediately after decompression.
InstallResult InstallNspFromReader(const PackageRangeReader &reader,
                                    const InstallConfig &config,
                                    ProgressCallback progress,
                                    StopCallback stop_callback = {});

InstallResult InstallXciFromReader(const PackageRangeReader &reader,
                                    const InstallConfig &config,
                                    ProgressCallback progress,
                                    StopCallback stop_callback = {});

}
