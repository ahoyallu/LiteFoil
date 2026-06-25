#include <install/ContentMeta.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <switch.h>

#include <platform/ExitLog.hpp>

namespace shield::install {

std::string NcaIdToString(const NcmContentId &id) {
    char buf[65] = {};
    for(int i = 0; i < 16; i++) {
        std::snprintf(buf + i * 2, 3, "%02x", id.c[i]);
    }
    return std::string(buf, 32);
}

NcmContentId NcaIdFromString(const std::string &hex) {
    NcmContentId id{};
    const std::size_t max_bytes = (hex.size() < 32) ? (hex.size() / 2) : 16;
    for(std::size_t i = 0; i < max_bytes; i++) {
        unsigned int byte_val = 0;
        std::sscanf(hex.c_str() + i * 2, "%02x", &byte_val);
        id.c[i] = static_cast<std::uint8_t>(byte_val);
    }
    return id;
}

ContentMeta::ContentMeta(const std::uint8_t *data, std::size_t size)
    : bytes_(data, data + size) {}

PackagedContentMetaHeader ContentMeta::GetHeader() const {
    PackagedContentMetaHeader header{};
    if(bytes_.size() >= sizeof(header)) {
        std::memcpy(&header, bytes_.data(), sizeof(header));
    }
    return header;
}

NcmContentMetaKey ContentMeta::GetContentMetaKey() const {
    const auto header = GetHeader();
    NcmContentMetaKey key{};
    key.id      = header.title_id;
    key.version = header.version;
    key.type    = static_cast<NcmContentMetaType>(header.type);
    // install_type left as 0 (Full) – matches awoo/cyberfoil behaviour.
    return key;
}

std::vector<NcmContentInfo> ContentMeta::GetContentInfos() const {
    const auto header = GetHeader();

    const std::size_t info_offset = sizeof(PackagedContentMetaHeader) + header.extended_header_size;
    constexpr std::size_t kPackagedContentInfoSize = 0x38;

    std::vector<NcmContentInfo> infos;
    infos.reserve(header.content_count);

    for(std::uint16_t i = 0; i < header.content_count; i++) {
        const std::size_t entry_offset = info_offset + i * kPackagedContentInfoSize;
        if(entry_offset + kPackagedContentInfoSize > bytes_.size()) {
            break;
        }

        NcmContentInfo info{};
        std::memcpy(&info, bytes_.data() + entry_offset + 0x20, sizeof(info));

        if(info.content_type <= NcmContentType_LegalInformation) {
            infos.push_back(info);
        }
    }

    return infos;
}

bool ContentMeta::GetInstallContentMeta(ByteBuffer &out,
                                         const NcmContentInfo &cnmt_content_info,
                                         bool ignore_required_firmware_version) const {
    if(bytes_.size() < sizeof(PackagedContentMetaHeader)) {
        return false;
    }

    const auto header = GetHeader();
    auto content_infos = GetContentInfos();

    out.Clear();

    NcmContentMetaHeader meta_header{};
    meta_header.extended_header_size = header.extended_header_size;
    meta_header.content_count = static_cast<std::uint16_t>(content_infos.size() + 1);
    meta_header.content_meta_count = header.content_meta_count;
    meta_header.attributes = header.attributes;
    meta_header.storage_id = 0;
    out.AppendValue(meta_header);

    const std::size_t ext_hdr_offset = sizeof(PackagedContentMetaHeader);
    if(ext_hdr_offset + header.extended_header_size > bytes_.size()) {
        return false;
    }

    const std::size_t ext_hdr_buf_offset = out.GetSize();
    out.Append(bytes_.data() + ext_hdr_offset, header.extended_header_size);

    //   NcmApplicationMetaExtendedHeader: u64 patch_id, u32 required_system_version → offset 8.
    //   NcmPatchMetaExtendedHeader:       u64 application_id, u32 required_system_version → offset 8.
    // Both layouts put required_system_version at offset 8 inside the extended header
    // (the first u64 is an id field — patch_id for Application, application_id for Patch).
    // The previous Patch branch wrote at offset 0, corrupting the low 32 bits of
    // application_id, which broke the base-game ↔ update link and made update
    // installs fail or not appear on Home Menu.
    if(ignore_required_firmware_version && header.extended_header_size >= 12) {
        const std::uint32_t zero = 0;
        if(header.type == NcmContentMetaType_Application ||
           header.type == NcmContentMetaType_Patch) {
            std::memcpy(out.GetData() + ext_hdr_buf_offset + 8, &zero, sizeof(zero));
        }
    }

    out.AppendValue(cnmt_content_info);

    for(const auto &info : content_infos) {
        out.AppendValue(info);
    }

    if(header.type == NcmContentMetaType_Patch &&
       header.extended_header_size >= sizeof(NcmPatchMetaExtendedHeader)) {
        NcmPatchMetaExtendedHeader patch_ext{};
        std::memcpy(&patch_ext, bytes_.data() + ext_hdr_offset, sizeof(patch_ext));
        if(patch_ext.extended_data_size > 0) {
            out.Resize(out.GetSize() + patch_ext.extended_data_size);
        }
    }

    return true;
}

// The NCA must already be registered in content storage.  We use the system FS
// service (fsOpenFileSystemWithId) so the OS handles NCA header decryption,
// section layout, and hash verification transparently.

bool ReadCnmtFromInstalledNca(ContentStorage &storage,
                               const NcmContentId &nca_id,
                               std::vector<std::uint8_t> &out_cnmt_data) {
    out_cnmt_data.clear();

    std::string nca_path;
    if(!storage.GetPath(nca_id, nca_path)) {
        return false;
    }

    FsFileSystem fs;
    Result rc = fsOpenFileSystemWithId(&fs, 0, FsFileSystemType_ContentMeta,
                                        nca_path.c_str(), FsContentAttributes_All);
    if(R_FAILED(rc)) {
        shield::platform::RuntimeLog("[cnmt] fsOpenFileSystemWithId failed path=%s rc=0x%x module=%u desc=%u",
            nca_path.c_str(), static_cast<unsigned int>(rc),
            static_cast<unsigned int>(R_MODULE(rc)), static_cast<unsigned int>(R_DESCRIPTION(rc)));
        return false;
    }

    FsDir dir;
    rc = fsFsOpenDirectory(&fs, "/", FsDirOpenMode_ReadFiles, &dir);
    if(R_FAILED(rc)) {
        shield::platform::RuntimeLog("[cnmt] fsFsOpenDirectory failed rc=0x%x module=%u desc=%u",
            static_cast<unsigned int>(rc), static_cast<unsigned int>(R_MODULE(rc)),
            static_cast<unsigned int>(R_DESCRIPTION(rc)));
        fsFsClose(&fs);
        return false;
    }

    FsDirectoryEntry dir_entry{};
    s64 entries_read = 0;
    rc = fsDirRead(&dir, &entries_read, 1, &dir_entry);
    fsDirClose(&dir);

    if(R_FAILED(rc) || entries_read == 0) {
        shield::platform::RuntimeLog("[cnmt] fsDirRead failed/empty rc=0x%x entries=%lld module=%u desc=%u",
            static_cast<unsigned int>(rc), static_cast<long long>(entries_read),
            static_cast<unsigned int>(R_MODULE(rc)), static_cast<unsigned int>(R_DESCRIPTION(rc)));
        fsFsClose(&fs);
        return false;
    }

    char cnmt_path[FS_MAX_PATH + 1];
    std::snprintf(cnmt_path, sizeof(cnmt_path), "/%s", dir_entry.name);

    FsFile cnmt_file;
    rc = fsFsOpenFile(&fs, cnmt_path, FsOpenMode_Read, &cnmt_file);
    if(R_FAILED(rc)) {
        shield::platform::RuntimeLog("[cnmt] fsFsOpenFile failed path=%s rc=0x%x module=%u desc=%u",
            cnmt_path, static_cast<unsigned int>(rc),
            static_cast<unsigned int>(R_MODULE(rc)), static_cast<unsigned int>(R_DESCRIPTION(rc)));
        fsFsClose(&fs);
        return false;
    }

    s64 file_size = 0;
    rc = fsFileGetSize(&cnmt_file, &file_size);
    if(R_FAILED(rc) || file_size <= 0 || file_size > 16 * 1024 * 1024) {
        shield::platform::RuntimeLog("[cnmt] fsFileGetSize failed/bad size=%lld rc=0x%x module=%u desc=%u",
            static_cast<long long>(file_size), static_cast<unsigned int>(rc),
            static_cast<unsigned int>(R_MODULE(rc)), static_cast<unsigned int>(R_DESCRIPTION(rc)));
        fsFileClose(&cnmt_file);
        fsFsClose(&fs);
        return false;
    }

    out_cnmt_data.resize(static_cast<std::size_t>(file_size));
    u64 bytes_read = 0;
    rc = fsFileRead(&cnmt_file, 0, out_cnmt_data.data(), out_cnmt_data.size(),
                     FsReadOption_None, &bytes_read);

    fsFileClose(&cnmt_file);
    fsFsClose(&fs);

    if(R_FAILED(rc) || bytes_read != static_cast<u64>(file_size)) {
        shield::platform::RuntimeLog("[cnmt] fsFileRead failed bytes=%llu/%lld rc=0x%x module=%u desc=%u",
            static_cast<unsigned long long>(bytes_read), static_cast<long long>(file_size),
            static_cast<unsigned int>(rc), static_cast<unsigned int>(R_MODULE(rc)),
            static_cast<unsigned int>(R_DESCRIPTION(rc)));
        out_cnmt_data.clear();
        return false;
    }

    return true;
}

}
