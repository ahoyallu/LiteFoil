#include <install/InstallEngine.hpp>
#include <install/ContentMeta.hpp>
#include <install/CryptoUtils.hpp>
#include <install/NcaStructs.hpp>
#include <install/NcmWrapper.hpp>
#include <install/NszDecompressor.hpp>
#include <install/Pfs0Parser.hpp>
#include <install/XciParser.hpp>
#include <platform/ExitLog.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <sys/stat.h>

#include <switch.h>

namespace shield::install {
namespace {

// Shared I/O buffer size for streaming NCAs to content storage.
constexpr std::size_t kStreamBufferSize = 512 * 1024; // 512 KiB

// Flush size for the buffered NCM placeholder writer.  Each
// ncmContentStorageWritePlaceHolder call is a full IPC round-trip, so
// coalescing many small curl chunks (typically 16–64 KiB) into a single
// ~1 MiB write dramatically reduces IPC overhead during stream install.
constexpr std::size_t kPlaceholderFlushSize = 1 * 1024 * 1024; // 1 MiB

// Temp directory for NCZ decompression.
constexpr const char *kTempDir = "sdmc:/switch/LiteFoil/tmp";

// Buffered wrapper around ContentStorage::WritePlaceholder that batches
// small contiguous writes into a single IPC call.  The caller is expected
// to feed strictly contiguous bytes starting at |start_offset|.
class BufferedPlaceholderWriter {
public:
    BufferedPlaceholderWriter(ContentStorage &storage,
                              const NcmContentId &id,
                              std::uint64_t start_offset)
        : storage_(storage), id_(id), write_offset_(start_offset) {
        this->buffer_.reserve(kPlaceholderFlushSize);
    }

    bool Write(const void *data, std::size_t len) {
        const auto *p = static_cast<const std::uint8_t *>(data);
        std::size_t left = len;
        while(left > 0) {
            const std::size_t space = kPlaceholderFlushSize - this->buffer_.size();
            const std::size_t take  = std::min(space, left);
            this->buffer_.insert(this->buffer_.end(), p, p + take);
            p    += take;
            left -= take;
            if(this->buffer_.size() >= kPlaceholderFlushSize) {
                if(!this->Flush()) return false;
            }
        }
        return true;
    }

    bool Flush() {
        if(this->buffer_.empty()) return true;
        if(!this->storage_.WritePlaceholder(this->id_, this->write_offset_,
                                             this->buffer_.data(), this->buffer_.size())) {
            return false;
        }
        this->write_offset_ += this->buffer_.size();
        this->buffer_.clear();
        return true;
    }

private:
    ContentStorage &storage_;
    NcmContentId id_{};
    std::vector<std::uint8_t> buffer_;
    std::uint64_t write_offset_ = 0;
};

bool IsNczFilename(const std::string &name) {
    if(name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ncz";
}

enum class NcaSigStatus { Valid, Invalid, Error };

NcaSigStatus ValidateNcaHeaderSig(FILE *file, std::uint64_t nca_offset,
                                   const HeaderKey &header_key) {
    NcaHeader header{};
    if(std::fseek(file, static_cast<long>(nca_offset), SEEK_SET) != 0) {
        return NcaSigStatus::Error;
    }
    if(std::fread(&header, sizeof(header), 1, file) != 1) {
        return NcaSigStatus::Error;
    }

    DecryptNcaHeader(&header, sizeof(header), header_key);

    if(header.magic != kMagicNca3) {
        return NcaSigStatus::Invalid;
    }

    if(!Rsa2048PssVerify(&header.magic, kNcaSectorSize,
                          header.fixed_key_sig,
                          kNcaHeaderSignatureModulus)) {
        return NcaSigStatus::Invalid;
    }

    return NcaSigStatus::Valid;
}

// If |header_key| is provided, the NCA header is decrypted to read nca_size
// and patch the distribution byte (GameCard → System), then re-encrypted
// before writing.  This matches the behaviour of reference installers
// (cyberfoil NcaWriter::flushHeader, awoo, sphaira).

bool StreamNcaToStorage(FILE *file, std::uint64_t offset, std::uint64_t size,
                         ContentStorage &storage, const NcmContentId &nca_id,
                         ProgressCallback &progress,
                         std::uint32_t nca_index, std::uint32_t nca_count,
                         const std::string &nca_name,
                         const HeaderKey *header_key) {
    storage.DeletePlaceholder(nca_id);

    NcaHeader raw_header{};
    std::uint64_t nca_size = size;

    if(size >= kNcaHeaderSize) {
        if(std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
            return false;
        }
        if(std::fread(&raw_header, kNcaHeaderSize, 1, file) == 1 && header_key) {
            NcaHeader dec = raw_header;
            DecryptNcaHeader(&dec, kNcaHeaderSize, *header_key);

            if(dec.magic == kMagicNca3) {
                if(dec.nca_size >= kNcaHeaderSize && dec.nca_size <= size) {
                    nca_size = dec.nca_size;
                }

                // Patch distribution: GameCard (1) → System/Download (0).
                if(dec.distribution != 0) {
                    dec.distribution = 0;
                    EncryptNcaHeader(&dec, kNcaHeaderSize, *header_key);
                    std::memcpy(&raw_header, &dec, kNcaHeaderSize);
                }
            }
        }
    }

    if(!storage.CreatePlaceholder(nca_id, nca_size)) {
        return false;
    }

    // Guard: deletes the placeholder if the function exits without registering it.
    // The explicit DeletePlaceholder on the success path fires first; the guard
    // destructor is a safe no-op for a non-existent placeholder.
    struct PlaceholderGuard {
        ContentStorage &s;
        const NcmContentId &id;
        ~PlaceholderGuard() { s.DeletePlaceholder(id); }
    } ph_guard{storage, nca_id};

    std::uint64_t written = 0;

    if(size >= kNcaHeaderSize) {
        if(!storage.WritePlaceholder(nca_id, 0, &raw_header, kNcaHeaderSize)) {
            return false;
        }
        written = kNcaHeaderSize;

        if(progress) {
            progress(InstallProgress{
                written, nca_size, nca_name, nca_index, nca_count, false
            });
        }
    }

    if(std::fseek(file, static_cast<long>(offset + written), SEEK_SET) != 0) {
        return false;
    }

    auto buf = std::make_unique<std::uint8_t[]>(kStreamBufferSize);

    while(written < nca_size) {
        const std::size_t chunk = std::min<std::size_t>(kStreamBufferSize, nca_size - written);
        const std::size_t read_count = std::fread(buf.get(), 1, chunk, file);
        if(read_count == 0) {
            break;
        }

        if(!storage.WritePlaceholder(nca_id, written, buf.get(), read_count)) {
            return false;
        }

        written += read_count;

        if(progress) {
            progress(InstallProgress{
                written, nca_size, nca_name, nca_index, nca_count, false
            });
        }
    }

    if(written != nca_size) {
        return false;
    }

    if(!storage.Register(nca_id, nca_id)) {
        // May already exist – treat as non-fatal.
    }

    storage.DeletePlaceholder(nca_id);
    return true;
}

struct GenericEntry {
    std::string   name;
    std::uint64_t offset;
    std::uint64_t size;
};

const GenericEntry *FindEntryByNcaId(const std::vector<GenericEntry> &entries,
                                      const std::string &nca_id_hex) {
    for(const auto &entry : entries) {
        if(entry.name.size() >= 32) {
            bool match = std::equal(
                nca_id_hex.begin(), nca_id_hex.end(), entry.name.begin(),
                [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                });
            if(match) {
                return &entry;
            }
        }
    }
    return nullptr;
}

// Install a single NCA (or NCZ) to content storage.  Handles NCZ decompression
// and returns true on success.
bool InstallSingleNca(FILE *file, const std::string &container_path,
                       const GenericEntry *nca_entry,
                       ContentStorage &storage,
                       const NcmContentId &nca_id,
                       ProgressCallback &progress,
                       std::uint32_t nca_index, std::uint32_t nca_count,
                       const HeaderKey *header_key) {
    if(IsNczFilename(nca_entry->name)) {
        if(header_key != nullptr) {
            const bool installed_directly = DecompressNczToStorage(
                container_path,
                nca_entry->offset,
                nca_entry->size,
                storage,
                nca_id,
                *header_key,
                [&progress, &nca_entry, nca_index, nca_count](std::uint64_t bytes_done, std::uint64_t bytes_total) {
                    if(progress) {
                        progress(InstallProgress{
                            bytes_done,
                            bytes_total,
                            nca_entry->name,
                            nca_index,
                            nca_count,
                            true
                        });
                    }
                });
            if(installed_directly) {
                return true;
            }
        }

        std::string decompressed = DecompressNczToTemp(
            container_path, nca_entry->offset, nca_entry->size, kTempDir,
            [&progress, &nca_entry, nca_index, nca_count](std::uint64_t bytes_done, std::uint64_t bytes_total) {
                if(progress) {
                    progress(InstallProgress{
                        bytes_done,
                        bytes_total,
                        nca_entry->name,
                        nca_index,
                        nca_count,
                        true
                    });
                }
            });

        if(decompressed.empty()) {
            // Fallback: treat as uncompressed.
            return StreamNcaToStorage(file, nca_entry->offset, nca_entry->size,
                                       storage, nca_id,
                                       progress, nca_index, nca_count, nca_entry->name,
                                       header_key);
        }

        FILE *tmp_file = std::fopen(decompressed.c_str(), "rb");
        if(!tmp_file) {
            std::remove(decompressed.c_str());
            return false;
        }

        std::fseek(tmp_file, 0, SEEK_END);
        const std::uint64_t decompressed_size = static_cast<std::uint64_t>(std::ftell(tmp_file));
        std::fseek(tmp_file, 0, SEEK_SET);

        bool ok = StreamNcaToStorage(tmp_file, 0, decompressed_size,
                                      storage, nca_id,
                                      progress, nca_index, nca_count, nca_entry->name,
                                      header_key);
        std::fclose(tmp_file);
        std::remove(decompressed.c_str());
        return ok;
    }

    return StreamNcaToStorage(file, nca_entry->offset, nca_entry->size,
                               storage, nca_id,
                               progress, nca_index, nca_count, nca_entry->name,
                               header_key);
}

InstallResult InstallFromEntries(FILE *file,
                                  const std::string &container_path,
                                  const std::vector<GenericEntry> &entries,
                                  const InstallConfig &config,
                                  ProgressCallback progress) {
    InstallResult result;

    HeaderKey header_key{};
    const bool have_header_key = DeriveHeaderKey(header_key);
    const HeaderKey *hk_ptr = have_header_key ? &header_key : nullptr;

    ContentStorage storage(config.dest_storage_id);
    if(!storage.IsOpen()) {
        result.error_message = "Failed to open content storage";
        return result;
    }

    ContentMetaDatabase meta_db(config.dest_storage_id);
    if(!meta_db.IsOpen()) {
        result.error_message = "Failed to open content meta database";
        return result;
    }

    std::vector<NcmContentId> registered_ids;
    struct CleanupGuard {
        ContentStorage &storage;
        std::vector<NcmContentId> &ids;
        bool armed = true;
        ~CleanupGuard() {
            if(!armed) return;
            for(const auto &id : ids) storage.Delete(id);
        }
    } cleanup_guard{storage, registered_ids};

    // Tickets must be imported before opening NCA filesystems so the OS has
    // the title keys available for decryption.

    std::vector<const GenericEntry *> tik_entries, cert_entries;
    for(const auto &entry : entries) {
        std::string lower_name = entry.name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if(lower_name.size() >= 4 && lower_name.substr(lower_name.size() - 4) == ".tik") {
            tik_entries.push_back(&entry);
        }
        if(lower_name.size() >= 5 && lower_name.substr(lower_name.size() - 5) == ".cert") {
            cert_entries.push_back(&entry);
        }
    }

    for(std::size_t i = 0; i < tik_entries.size(); i++) {
        if(i >= cert_entries.size()) break;

        const auto *tik_e  = tik_entries[i];
        const auto *cert_e = cert_entries[i];

        auto tik_buf  = std::make_unique<std::uint8_t[]>(tik_e->size);
        auto cert_buf = std::make_unique<std::uint8_t[]>(cert_e->size);

        std::fseek(file, static_cast<long>(tik_e->offset), SEEK_SET);
        if(std::fread(tik_buf.get(), tik_e->size, 1, file) != 1) continue;

        std::fseek(file, static_cast<long>(cert_e->offset), SEEK_SET);
        if(std::fread(cert_buf.get(), cert_e->size, 1, file) != 1) continue;

        ImportTicket(tik_buf.get(), tik_e->size, cert_buf.get(), cert_e->size);
    }

    // Like reference installers (awoo/cyberfoil), we install the CNMT NCA to
    // content storage, then use fsOpenFileSystemWithId to let the system OS
    // decrypt and parse the NCA for us.

    std::vector<const GenericEntry *> cnmt_entries;
    for(const auto &entry : entries) {
        const auto &n = entry.name;
        if(n.size() > 9) {
            std::string suffix = n.substr(n.size() - 9);
            std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if(suffix == ".cnmt.nca" || suffix == ".cnmt.ncz") {
                cnmt_entries.push_back(&entry);
            }
        }
    }

    if(cnmt_entries.empty()) {
        result.error_message = "No CNMT NCA found in package";
        return result;
    }

    struct CnmtRecord {
        ContentMeta     meta;
        NcmContentInfo  cnmt_info;
    };
    std::vector<CnmtRecord> cnmt_records;

    for(const auto *cnmt_entry : cnmt_entries) {
        if(cnmt_entry->name.size() < 32) continue;
        std::string nca_id_hex = cnmt_entry->name.substr(0, 32);
        NcmContentId cnmt_nca_id = NcaIdFromString(nca_id_hex);

        if(config.verify_nca_sigs && have_header_key) {
            auto sig_status = ValidateNcaHeaderSig(file, cnmt_entry->offset, header_key);
            if(sig_status == NcaSigStatus::Invalid && !config.allow_unsigned) {
                result.error_message = "NCA header signature verification failed for " + cnmt_entry->name +
                                       " – enable 'Allow unsigned sources' in settings to proceed";
                return result;
            }
        }

        if(!storage.Has(cnmt_nca_id) || config.reinstall_ncas) {
            if(!InstallSingleNca(file, container_path, cnmt_entry,
                                  storage, cnmt_nca_id,
                                  progress, 0, 0, hk_ptr)) {
                result.error_message = "Failed to install CNMT NCA: " + cnmt_entry->name;
                return result;
            }
            registered_ids.push_back(cnmt_nca_id);
        }

        std::vector<std::uint8_t> cnmt_data;
        if(!ReadCnmtFromInstalledNca(storage, cnmt_nca_id, cnmt_data)) {
            result.error_message = "Failed to read CNMT from installed NCA: " + cnmt_entry->name;
            return result;
        }

        CnmtRecord record{};
        record.meta = ContentMeta(cnmt_data.data(), cnmt_data.size());

        record.cnmt_info.content_id   = cnmt_nca_id;
        ncmU64ToContentInfoSize(cnmt_entry->size & 0xFFFFFFFFFFFFULL, &record.cnmt_info);
        record.cnmt_info.content_type = NcmContentType_Meta;

        cnmt_records.push_back(std::move(record));
    }

    struct DeferredPush {
        std::uint64_t      base_title_id;
        NcmContentMetaKey  key;
    };
    std::vector<DeferredPush> deferred_pushes;

    for(auto &record : cnmt_records) {
        const auto key = record.meta.GetContentMetaKey();

        ByteBuffer install_buf;
        if(!record.meta.GetInstallContentMeta(install_buf, record.cnmt_info,
                                               config.ignore_req_fw)) {
            result.error_message = "Failed to build install content meta";
            return result;
        }

        if(!meta_db.Set(key, install_buf.GetData(), install_buf.GetSize())) {
            result.error_message = "Failed to set content meta records";
            return result;
        }

        if(!meta_db.Commit()) {
            result.error_message = "Failed to commit content meta database";
            return result;
        }

        const std::uint64_t base_title_id = GetBaseTitleId(key.id,
                                                            static_cast<std::uint8_t>(key.type));
        deferred_pushes.push_back({ base_title_id, key });
        result.title_id = key.id;
    }

    std::vector<NcmContentInfo> all_content_infos;
    for(auto &record : cnmt_records) {
        auto infos = record.meta.GetContentInfos();
        all_content_infos.insert(all_content_infos.end(), infos.begin(), infos.end());
    }

    const std::uint32_t total_ncas = static_cast<std::uint32_t>(all_content_infos.size());
    std::uint32_t nca_index = 0;

    for(const auto &content_info : all_content_infos) {
        nca_index++;
        const std::string nca_id_hex = NcaIdToString(content_info.content_id);

        if(storage.Has(content_info.content_id) && !config.reinstall_ncas) {
            continue;
        }

        const GenericEntry *nca_entry = FindEntryByNcaId(entries, nca_id_hex);
        if(!nca_entry) {
            result.error_message = "Missing NCA in package: " + nca_id_hex;
            return result;
        }

        if(config.verify_nca_sigs && have_header_key) {
            auto sig_status = ValidateNcaHeaderSig(file, nca_entry->offset, header_key);
            if(sig_status == NcaSigStatus::Invalid && !config.allow_unsigned) {
                result.error_message = "Unsigned NCA blocked: " + nca_entry->name;
                return result;
            }
        }

        if(!InstallSingleNca(file, container_path, nca_entry,
                              storage, content_info.content_id,
                              progress, nca_index, total_ncas,
                              hk_ptr)) {
            result.error_message = "Failed to install NCA: " + nca_entry->name;
            return result;
        }
        registered_ids.push_back(content_info.content_id);
    }

    for(const auto &dp : deferred_pushes) {
        if(!PushApplicationRecord(dp.base_title_id, config.dest_storage_id, dp.key)) {
            result.error_message = "Failed to push application record";
            return result;
        }
    }

    result.success = true;
    cleanup_guard.armed = false;
    return result;
}

std::vector<std::uint8_t> ReadBytesFromReader(const PackageRangeReader &reader,
                                               std::uint64_t offset,
                                               std::uint64_t size) {
    std::vector<std::uint8_t> buf;
    if(size == 0) return buf;
    buf.reserve(size);
    reader(offset, size, [&](const void *data, std::size_t len) -> bool {
        const auto *bytes = static_cast<const std::uint8_t*>(data);
        buf.insert(buf.end(), bytes, bytes + len);
        return true;
    });
    return buf;
}

// Speculative single-shot read size used when parsing PFS0/HFS0 tables from
// a network range reader.  Large enough to cover header + entry table +
// string table for virtually every NSP/XCI in the wild (typical index is
// a few KiB), so we avoid the second HTTP round-trip for the "rest" block.
constexpr std::size_t kIndexSpeculativeSize = 32 * 1024;

bool ParsePfs0FromReader(const PackageRangeReader &reader,
                          std::vector<GenericEntry> &entries) {
    entries.clear();
    // One speculative read covering header + tables.  Falls back to a
    // second range read if the NSP has an unusually large index.
    auto spec = ReadBytesFromReader(reader, 0, kIndexSpeculativeSize);
    if(spec.size() < sizeof(Pfs0Header)) return false;
    Pfs0Header hdr{};
    std::memcpy(&hdr, spec.data(), sizeof(hdr));
    if(hdr.magic != kMagicPfs0) return false;
    if(hdr.file_count == 0) return true;
    const std::size_t entry_table_sz = hdr.file_count * sizeof(Pfs0FileEntry);
    const std::size_t rest_sz = entry_table_sz + hdr.string_table_size;
    const std::size_t total_needed = sizeof(Pfs0Header) + rest_sz;

    const std::uint8_t *rest_ptr = nullptr;
    std::vector<std::uint8_t> fallback;
    if(spec.size() >= total_needed) {
        rest_ptr = spec.data() + sizeof(Pfs0Header);
    } else {
        fallback = ReadBytesFromReader(reader, sizeof(Pfs0Header), rest_sz);
        if(fallback.size() < rest_sz) return false;
        rest_ptr = fallback.data();
    }
    const std::uint64_t data_start = sizeof(Pfs0Header) + rest_sz;
    const auto *raw = reinterpret_cast<const Pfs0FileEntry*>(rest_ptr);
    const char *strtab = reinterpret_cast<const char*>(rest_ptr + entry_table_sz);
    entries.reserve(hdr.file_count);
    for(std::uint32_t i = 0; i < hdr.file_count; i++) {
        GenericEntry e;
        if(raw[i].string_table_offset < hdr.string_table_size) {
            e.name = strtab + raw[i].string_table_offset;
        }
        e.offset = data_start + raw[i].data_offset;
        e.size   = raw[i].file_size;
        entries.push_back(std::move(e));
    }
    return true;
}

bool ParseHfs0FromReader(const PackageRangeReader &reader,
                          std::uint64_t hfs0_off,
                          std::vector<GenericEntry> &out_entries) {
    out_entries.clear();
    // Same speculative read strategy as Pfs0 to collapse two range requests
    // into one on typical XCIs.
    auto spec = ReadBytesFromReader(reader, hfs0_off, kIndexSpeculativeSize);
    if(spec.size() < sizeof(Hfs0Header)) return false;
    Hfs0Header hdr{};
    std::memcpy(&hdr, spec.data(), sizeof(hdr));
    if(hdr.magic != kMagicHfs0) return false;
    if(hdr.file_count == 0) return true;
    const std::size_t entry_table_sz = hdr.file_count * sizeof(Hfs0FileEntry);
    const std::size_t rest_sz = entry_table_sz + hdr.string_table_size;
    const std::size_t total_needed = sizeof(Hfs0Header) + rest_sz;

    const std::uint8_t *rest_ptr = nullptr;
    std::vector<std::uint8_t> fallback;
    if(spec.size() >= total_needed) {
        rest_ptr = spec.data() + sizeof(Hfs0Header);
    } else {
        fallback = ReadBytesFromReader(reader, hfs0_off + sizeof(Hfs0Header), rest_sz);
        if(fallback.size() < rest_sz) return false;
        rest_ptr = fallback.data();
    }
    const std::uint64_t data_start = hfs0_off + sizeof(Hfs0Header) + rest_sz;
    const auto *raw = reinterpret_cast<const Hfs0FileEntry*>(rest_ptr);
    const char *strtab = reinterpret_cast<const char*>(rest_ptr + entry_table_sz);
    out_entries.reserve(hdr.file_count);
    for(std::uint32_t i = 0; i < hdr.file_count; i++) {
        GenericEntry e;
        if(raw[i].string_table_offset < hdr.string_table_size) {
            e.name = strtab + raw[i].string_table_offset;
        }
        e.offset = data_start + raw[i].data_offset;
        e.size   = raw[i].file_size;
        out_entries.push_back(std::move(e));
    }
    return true;
}

bool ParseXciFromReader(const PackageRangeReader &reader,
                         std::vector<GenericEntry> &entries) {
    entries.clear();
    const auto xci_bytes = ReadBytesFromReader(reader, kXciHeaderOffset, sizeof(XciHeader));
    if(xci_bytes.size() < sizeof(XciHeader)) return false;
    XciHeader xci_hdr{};
    std::memcpy(&xci_hdr, xci_bytes.data(), sizeof(xci_hdr));
    if(xci_hdr.magic != kMagicHead) return false;
    const std::uint64_t root_hfs0_offset = xci_hdr.hfs0_partition_offset;
    std::vector<GenericEntry> root_entries;
    if(!ParseHfs0FromReader(reader, root_hfs0_offset, root_entries)) return false;
    for(const auto &e : root_entries) {
        if(e.name == "secure") {
            return ParseHfs0FromReader(reader, e.offset, entries);
        }
    }
    return false;
}

// Temp path used to buffer a single NCZ entry during stream install.
static constexpr const char *kNczStreamTemp = "sdmc:/switch/LiteFoil/tmp/stream_ncz.ncz";

// Stream an NCA from a range reader directly to NCM content storage.
// The NCA header is read, optionally patched, and written first; the body
// follows via repeated range reads into WritePlaceholder.
bool StreamNcaFromReader(const PackageRangeReader &reader,
                          std::uint64_t entry_offset,
                          std::uint64_t entry_size,
                          ContentStorage &storage,
                          const NcmContentId &nca_id,
                          ProgressCallback &progress,
                          StopCallback &stop_callback,
                          std::uint32_t nca_index,
                          std::uint32_t nca_count,
                          const std::string &nca_name,
                          const HeaderKey *header_key) {
    storage.DeletePlaceholder(nca_id);

    if(stop_callback && stop_callback()) {
        return false;
    }

    NcaHeader raw_header{};
    std::uint64_t nca_size = entry_size;

    if(entry_size >= kNcaHeaderSize) {
        const auto hdr_bytes = ReadBytesFromReader(reader, entry_offset, kNcaHeaderSize);
        if(hdr_bytes.size() == kNcaHeaderSize) {
            std::memcpy(&raw_header, hdr_bytes.data(), kNcaHeaderSize);
            if(header_key) {
                NcaHeader dec = raw_header;
                DecryptNcaHeader(&dec, kNcaHeaderSize, *header_key);
                if(dec.magic == kMagicNca3) {
                    if(dec.nca_size >= kNcaHeaderSize && dec.nca_size <= entry_size) {
                        nca_size = dec.nca_size;
                    }
                    if(dec.distribution != 0) {
                        dec.distribution = 0;
                        EncryptNcaHeader(&dec, kNcaHeaderSize, *header_key);
                        std::memcpy(&raw_header, &dec, kNcaHeaderSize);
                    }
                }
            }
        }
    }

    if(!storage.CreatePlaceholder(nca_id, nca_size)) return false;

    std::uint64_t written = 0;
    if(entry_size >= kNcaHeaderSize) {
        if(stop_callback && stop_callback()) {
            storage.DeletePlaceholder(nca_id);
            return false;
        }
        if(!storage.WritePlaceholder(nca_id, 0, &raw_header, kNcaHeaderSize)) {
            storage.DeletePlaceholder(nca_id);
            return false;
        }
        written = kNcaHeaderSize;
        if(progress) progress(InstallProgress{written, nca_size, nca_name, nca_index, nca_count, false});
    }

    const std::uint64_t body_size = nca_size - written;
    BufferedPlaceholderWriter body_writer(storage, nca_id, written);
    const bool read_ok = reader(entry_offset + written, body_size,
        [&](const void *data, std::size_t len) -> bool {
            if(stop_callback && stop_callback()) return false;
            if(!body_writer.Write(data, len)) return false;
            written += len;
            if(progress) progress(InstallProgress{written, nca_size, nca_name, nca_index, nca_count, false});
            return true;
        });

    const bool flush_ok = body_writer.Flush();
    if(!read_ok || written != nca_size || !flush_ok) {
        storage.DeletePlaceholder(nca_id);
        return false;
    }
    if(!storage.Register(nca_id, nca_id)) {}
    storage.DeletePlaceholder(nca_id);
    return true;
}

// Install a compressed NCA (.ncz) entry from a range reader.
// Tries the reader-direct path first (no SD temp file required); only falls
// back to the temp-file approach if the direct path fails.
bool InstallNczFromReader(const PackageRangeReader &reader,
                           std::uint64_t entry_offset,
                           std::uint64_t entry_size,
                           ContentStorage &storage,
                           const NcmContentId &nca_id,
                           ProgressCallback &progress,
                           StopCallback &stop_callback,
                           std::uint32_t nca_index,
                           std::uint32_t nca_count,
                           const std::string &nca_name,
                           const HeaderKey *header_key) {
    if(header_key) {
        const bool ok = DecompressNczFromRangeReaderToStorage(
            reader, entry_offset, entry_size, storage, nca_id, *header_key,
            [&](std::uint64_t done, std::uint64_t total) {
                if(progress) progress(InstallProgress{done, total, nca_name, nca_index, nca_count, true});
            },
            stop_callback);
        if(ok) return true;
        shield::platform::RuntimeLog("[ncz-install] direct-reader path failed for %s (idx=%u/%u size=%llu) — falling back to temp-file path",
            nca_name.c_str(), nca_index, nca_count, (unsigned long long)entry_size);
    }

    mkdir("sdmc:/switch/LiteFoil", 0777);
    mkdir("sdmc:/switch/LiteFoil/tmp", 0777);

    FILE *tmp_fp = std::fopen(kNczStreamTemp, "wb");
    if(!tmp_fp) {
        return StreamNcaFromReader(reader, entry_offset, entry_size,
                                    storage, nca_id, progress, stop_callback,
                                    nca_index, nca_count, nca_name, header_key);
    }

    // Enlarge the stdio buffer so small curl chunks coalesce into large
    // SD-card writes rather than many tiny ones.
    static thread_local std::vector<char> ncz_stdio_buf(1 * 1024 * 1024);
    std::setvbuf(tmp_fp, ncz_stdio_buf.data(), _IOFBF, ncz_stdio_buf.size());

    std::uint64_t ncz_dl_done = 0;
    bool dl_ok = reader(entry_offset, entry_size,
        [&](const void *data, std::size_t len) -> bool {
            if(stop_callback && stop_callback()) return false;
            if(std::fwrite(data, 1, len, tmp_fp) != len) return false;
            ncz_dl_done += len;
            if(progress) progress(InstallProgress{
                ncz_dl_done, entry_size, nca_name, nca_index, nca_count, false
            });
            return true;
        });
    std::fclose(tmp_fp);

    if(!dl_ok) { std::remove(kNczStreamTemp); return false; }

    if(header_key) {
        const bool ok = DecompressNczToStorage(
            kNczStreamTemp, 0, entry_size, storage, nca_id, *header_key,
            [&](std::uint64_t done, std::uint64_t total) {
                if(progress) progress(InstallProgress{done, total, nca_name, nca_index, nca_count, true});
            },
            stop_callback);
        std::remove(kNczStreamTemp);
        if(ok) return true;
    }

    std::string decompressed = DecompressNczToTemp(
        kNczStreamTemp, 0, entry_size, kTempDir,
        [&](std::uint64_t done, std::uint64_t total) {
            if(progress) progress(InstallProgress{done, total, nca_name, nca_index, nca_count, true});
        },
        stop_callback);
    std::remove(kNczStreamTemp);

    if(decompressed.empty()) return false;

    FILE *dec_fp = std::fopen(decompressed.c_str(), "rb");
    if(!dec_fp) { std::remove(decompressed.c_str()); return false; }
    std::fseek(dec_fp, 0, SEEK_END);
    const auto dec_size = static_cast<std::uint64_t>(std::ftell(dec_fp));
    std::fseek(dec_fp, 0, SEEK_SET);
    const bool ok = StreamNcaToStorage(dec_fp, 0, dec_size, storage, nca_id,
                                        progress, nca_index, nca_count, nca_name, header_key);
    std::fclose(dec_fp);
    std::remove(decompressed.c_str());
    return ok;
}

// Install a single NCA or NCZ entry from a range reader.
bool InstallSingleNcaFromReader(const PackageRangeReader &reader,
                                  const GenericEntry *nca_entry,
                                  ContentStorage &storage,
                                  const NcmContentId &nca_id,
                                  ProgressCallback &progress,
                                  StopCallback &stop_callback,
                                  std::uint32_t nca_index,
                                  std::uint32_t nca_count,
                                  const HeaderKey *header_key) {
    if(IsNczFilename(nca_entry->name)) {
        return InstallNczFromReader(reader, nca_entry->offset, nca_entry->size,
                                     storage, nca_id, progress, stop_callback,
                                     nca_index, nca_count, nca_entry->name, header_key);
    }
    return StreamNcaFromReader(reader, nca_entry->offset, nca_entry->size,
                                storage, nca_id, progress, stop_callback,
                                nca_index, nca_count, nca_entry->name, header_key);
}

// Core install loop for the stream path — mirrors InstallFromEntries but
// reads every byte via the PackageRangeReader rather than a local FILE*.
InstallResult InstallFromEntriesFromReader(const PackageRangeReader &reader,
                                            const std::vector<GenericEntry> &entries,
                                            const InstallConfig &config,
                                            ProgressCallback progress,
                                            StopCallback stop_callback) {
    InstallResult result;
    shield::platform::RuntimeLog("[install] reader core start entries=%zu storage=%u reinstall=%d verify=%d allow_unsigned=%d",
        entries.size(), static_cast<unsigned int>(config.dest_storage_id),
        static_cast<int>(config.reinstall_ncas), static_cast<int>(config.verify_nca_sigs),
        static_cast<int>(config.allow_unsigned));

    HeaderKey header_key{};
    const bool have_header_key = DeriveHeaderKey(header_key);
    const HeaderKey *hk_ptr = have_header_key ? &header_key : nullptr;

    ContentStorage storage(config.dest_storage_id);
    if(!storage.IsOpen()) {
        shield::platform::RuntimeLog("[install] open content storage failed");
        result.error_message = "Failed to open content storage";
        return result;
    }

    ContentMetaDatabase meta_db(config.dest_storage_id);
    if(!meta_db.IsOpen()) {
        shield::platform::RuntimeLog("[install] open content meta database failed");
        result.error_message = "Failed to open content meta database";
        return result;
    }

    std::vector<NcmContentId> registered_ids;
    struct CleanupGuard {
        ContentStorage &storage;
        std::vector<NcmContentId> &ids;
        bool armed = true;
        ~CleanupGuard() {
            if(!armed) return;
            for(const auto &id : ids) storage.Delete(id);
        }
    } cleanup_guard{storage, registered_ids};

    // Pre-compute total bytes from all entries for accurate speed/ETA display.
    std::uint64_t total_install_bytes = 0;
    for(const auto &e : entries) total_install_bytes += e.size;

    // Speed + cumulative bytes wrapper around the caller's progress callback.
    // Tracks progress across all NCAs so bytes_done/bytes_total always represent
    // the full-package progress rather than the per-NCA progress.
    std::uint64_t spd_completed = 0;         // bytes from fully-finished NCAs
    std::uint64_t spd_prev_total = 0;        // bytes_total of the in-progress NCA
    std::uint32_t spd_prev_idx = UINT32_MAX; // sentinel (no NCA seen yet)
    auto spd_tp = std::chrono::steady_clock::now();
    std::uint64_t spd_ref_bytes = 0;
    double spd_cur = 0.0;
    ProgressCallback speed_progress = [&](InstallProgress p) {
        if(p.nca_index != spd_prev_idx) {
            if(spd_prev_idx != UINT32_MAX) spd_completed += spd_prev_total;
            spd_prev_idx   = p.nca_index;
            spd_prev_total = p.bytes_total;
        }
        const std::uint64_t cum_now = spd_completed + p.bytes_done;
        const auto now = std::chrono::steady_clock::now();
        const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now - spd_tp).count();
        if(ms >= 500) {
            const double delta = static_cast<double>(cum_now - spd_ref_bytes);
            spd_cur     = (ms > 0) ? (delta * 1000.0 / static_cast<double>(ms)) : 0.0;
            spd_tp      = now;
            spd_ref_bytes = cum_now;
        }
        p.bytes_per_second = spd_cur;
        p.bytes_done       = (total_install_bytes > 0)
            ? std::min(cum_now, total_install_bytes) : cum_now;
        p.bytes_total      = total_install_bytes;
        progress(p);
    };

    std::vector<const GenericEntry*> tik_entries, cert_entries;
    for(const auto &e : entries) {
        std::string lower = e.name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if(lower.size() >= 4 && lower.substr(lower.size() - 4) == ".tik")  tik_entries.push_back(&e);
        if(lower.size() >= 5 && lower.substr(lower.size() - 5) == ".cert") cert_entries.push_back(&e);
    }
    for(std::size_t i = 0; i < tik_entries.size() && i < cert_entries.size(); i++) {
        if(stop_callback && stop_callback()) {
            result.error_message = "Install stopped";
            return result;
        }
        const auto tik_buf  = ReadBytesFromReader(reader, tik_entries[i]->offset,  tik_entries[i]->size);
        const auto cert_buf = ReadBytesFromReader(reader, cert_entries[i]->offset, cert_entries[i]->size);
        if(!tik_buf.empty() && !cert_buf.empty()) {
            ImportTicket(tik_buf.data(), tik_buf.size(), cert_buf.data(), cert_buf.size());
        }
    }

    std::vector<const GenericEntry*> cnmt_entries;
    for(const auto &e : entries) {
        if(e.name.size() > 9) {
            std::string suffix = e.name.substr(e.name.size() - 9);
            std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if(suffix == ".cnmt.nca" || suffix == ".cnmt.ncz") cnmt_entries.push_back(&e);
        }
    }
    if(cnmt_entries.empty()) {
        shield::platform::RuntimeLog("[install] no CNMT entries found");
        result.error_message = "No CNMT NCA found in package";
        return result;
    }

    struct CnmtRecord { ContentMeta meta; NcmContentInfo cnmt_info; };
    std::vector<CnmtRecord> cnmt_records;

    for(const auto *cnmt_entry : cnmt_entries) {
        if(stop_callback && stop_callback()) {
            result.error_message = "Install stopped";
            return result;
        }
        if(cnmt_entry->name.size() < 32) continue;
        const std::string nca_id_hex = cnmt_entry->name.substr(0, 32);
        NcmContentId cnmt_nca_id = NcaIdFromString(nca_id_hex);

        if(!storage.Has(cnmt_nca_id) || config.reinstall_ncas) {
            shield::platform::RuntimeLog("[install] installing CNMT %s size=%llu",
                cnmt_entry->name.c_str(), static_cast<unsigned long long>(cnmt_entry->size));
            if(!InstallSingleNcaFromReader(reader, cnmt_entry, storage, cnmt_nca_id,
                                            speed_progress, stop_callback, 0, 0, hk_ptr)) {
                shield::platform::RuntimeLog("[install] CNMT install failed: %s", cnmt_entry->name.c_str());
                result.error_message = "Failed to install CNMT NCA: " + cnmt_entry->name;
                return result;
            }
            registered_ids.push_back(cnmt_nca_id);
        }

        std::vector<std::uint8_t> cnmt_data;
        if(!ReadCnmtFromInstalledNca(storage, cnmt_nca_id, cnmt_data)) {
            shield::platform::RuntimeLog("[install] read CNMT failed: %s", cnmt_entry->name.c_str());
            result.error_message = "Failed to read CNMT from installed NCA: " + cnmt_entry->name;
            return result;
        }

        CnmtRecord record{};
        record.meta = ContentMeta(cnmt_data.data(), cnmt_data.size());
        record.cnmt_info.content_id   = cnmt_nca_id;
        ncmU64ToContentInfoSize(cnmt_entry->size & 0xFFFFFFFFFFFFULL, &record.cnmt_info);
        record.cnmt_info.content_type = NcmContentType_Meta;
        cnmt_records.push_back(std::move(record));
    }

    struct DeferredPush { std::uint64_t base_title_id; NcmContentMetaKey key; };
    std::vector<DeferredPush> deferred_pushes;

    for(auto &record : cnmt_records) {
        if(stop_callback && stop_callback()) {
            result.error_message = "Install stopped";
            return result;
        }
        const auto key = record.meta.GetContentMetaKey();
        ByteBuffer install_buf;
        if(!record.meta.GetInstallContentMeta(install_buf, record.cnmt_info, config.ignore_req_fw)) {
            result.error_message = "Failed to build install content meta"; return result;
        }
        if(!meta_db.Set(key, install_buf.GetData(), install_buf.GetSize())) {
            result.error_message = "Failed to set content meta records"; return result;
        }
        if(!meta_db.Commit()) { result.error_message = "Failed to commit content meta database"; return result; }
        deferred_pushes.push_back({ GetBaseTitleId(key.id, static_cast<std::uint8_t>(key.type)), key });
        result.title_id = key.id;
    }

    std::vector<NcmContentInfo> all_content_infos;
    for(auto &record : cnmt_records) {
        auto infos = record.meta.GetContentInfos();
        all_content_infos.insert(all_content_infos.end(), infos.begin(), infos.end());
    }

    const std::uint32_t total_ncas = static_cast<std::uint32_t>(all_content_infos.size());
    std::uint32_t nca_index = 0;

    for(const auto &content_info : all_content_infos) {
        if(stop_callback && stop_callback()) {
            result.error_message = "Install stopped";
            return result;
        }
        nca_index++;
        const std::string nca_id_hex = NcaIdToString(content_info.content_id);
        if(storage.Has(content_info.content_id) && !config.reinstall_ncas) continue;
        const GenericEntry *nca_entry = FindEntryByNcaId(entries, nca_id_hex);
        if(!nca_entry) { result.error_message = "Missing NCA in package: " + nca_id_hex; return result; }
        if(!InstallSingleNcaFromReader(reader, nca_entry, storage, content_info.content_id,
                                        speed_progress, stop_callback, nca_index, total_ncas, hk_ptr)) {
            result.error_message = "Failed to install NCA: " + nca_entry->name;
            shield::platform::RuntimeLog("[install] stream install failed at NCA %u/%u: %s",
                nca_index, total_ncas, nca_entry->name.c_str());
            return result;
        }
        registered_ids.push_back(content_info.content_id);
    }

    for(const auto &dp : deferred_pushes) {
        if(stop_callback && stop_callback()) {
            result.error_message = "Install stopped";
            return result;
        }
        if(!PushApplicationRecord(dp.base_title_id, config.dest_storage_id, dp.key)) {
            result.error_message = "Failed to push application record"; return result;
        }
    }

    result.success = true;
    shield::platform::RuntimeLog("[install] reader core success title_id=%016llx",
        static_cast<unsigned long long>(result.title_id));
    cleanup_guard.armed = false;
    return result;
}

}

InstallResult InstallNsp(const std::string &file_path,
                          const InstallConfig &config,
                          ProgressCallback progress) {
    InstallResult result;

    std::vector<Pfs0Entry> pfs0_entries;
    std::uint64_t data_offset = 0;
    if(!ParsePfs0(file_path, pfs0_entries, data_offset)) {
        result.error_message = "Failed to parse NSP (PFS0) header";
        return result;
    }

    if(pfs0_entries.empty()) {
        result.error_message = "NSP contains no files";
        return result;
    }

    FILE *file = std::fopen(file_path.c_str(), "rb");
    if(!file) {
        result.error_message = "Cannot open NSP file";
        return result;
    }

    // Prevent screen dimming / sleep during long installs.
    appletSetMediaPlaybackState(true);

    std::vector<GenericEntry> generic_entries;
    generic_entries.reserve(pfs0_entries.size());
    for(const auto &e : pfs0_entries) {
        generic_entries.push_back({ e.name, e.offset, e.size });
    }

    result = InstallFromEntries(file, file_path, generic_entries, config, progress);

    appletSetMediaPlaybackState(false);
    std::fclose(file);
    return result;
}

InstallResult InstallXci(const std::string &file_path,
                          const InstallConfig &config,
                          ProgressCallback progress) {
    InstallResult result;

    std::vector<XciPartition> partitions;
    if(!ParseXci(file_path, partitions)) {
        result.error_message = "Failed to parse XCI header";
        return result;
    }

    const XciPartition *secure = FindPartition(partitions, "secure");
    if(!secure || secure->entries.empty()) {
        result.error_message = "XCI secure partition not found or empty";
        return result;
    }

    FILE *file = std::fopen(file_path.c_str(), "rb");
    if(!file) {
        result.error_message = "Cannot open XCI file";
        return result;
    }

    appletSetMediaPlaybackState(true);

    std::vector<GenericEntry> generic_entries;
    generic_entries.reserve(secure->entries.size());
    for(const auto &e : secure->entries) {
        generic_entries.push_back({ e.name, e.offset, e.size });
    }

    result = InstallFromEntries(file, file_path, generic_entries, config, progress);

    appletSetMediaPlaybackState(false);
    std::fclose(file);
    return result;
}

InstallResult InstallFromLocalFile(const std::string &file_path,
                                    const InstallConfig &config,
                                    ProgressCallback progress) {
    std::string ext;
    const auto dot = file_path.find_last_of('.');
    if(dot != std::string::npos) {
        ext = file_path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    if(ext == ".nsp" || ext == ".nsz") {
        return InstallNsp(file_path, config, std::move(progress));
    }

    if(ext == ".xci" || ext == ".xcz") {
        return InstallXci(file_path, config, std::move(progress));
    }

    InstallResult result;
    result.error_message = "Unsupported package format: " + ext;
    return result;
}

InstallResult InstallNspFromReader(const PackageRangeReader &reader,
                                    const InstallConfig &config,
                                    ProgressCallback progress,
                                    StopCallback stop_callback) {
    InstallResult result;
    shield::platform::RuntimeLog("[install] InstallNspFromReader start");
    std::vector<GenericEntry> entries;
    if(!ParsePfs0FromReader(reader, entries)) {
        shield::platform::RuntimeLog("[install] ParsePfs0FromReader failed");
        result.error_message = "Failed to parse NSP (PFS0) header from stream";
        return result;
    }
    shield::platform::RuntimeLog("[install] ParsePfs0FromReader ok entries=%zu", entries.size());
    if(entries.empty()) {
        result.error_message = "NSP stream contains no files";
        return result;
    }
    appletSetMediaPlaybackState(true);
    result = InstallFromEntriesFromReader(reader, entries, config, std::move(progress), std::move(stop_callback));
    appletSetMediaPlaybackState(false);
    return result;
}

InstallResult InstallXciFromReader(const PackageRangeReader &reader,
                                    const InstallConfig &config,
                                    ProgressCallback progress,
                                    StopCallback stop_callback) {
    InstallResult result;
    shield::platform::RuntimeLog("[install] InstallXciFromReader start");
    std::vector<GenericEntry> entries;
    if(!ParseXciFromReader(reader, entries)) {
        shield::platform::RuntimeLog("[install] ParseXciFromReader failed");
        result.error_message = "Failed to parse XCI header from stream";
        return result;
    }
    shield::platform::RuntimeLog("[install] ParseXciFromReader ok entries=%zu", entries.size());
    if(entries.empty()) {
        result.error_message = "XCI stream secure partition is empty";
        return result;
    }
    appletSetMediaPlaybackState(true);
    result = InstallFromEntriesFromReader(reader, entries, config, std::move(progress), std::move(stop_callback));
    appletSetMediaPlaybackState(false);
    return result;
}

}
