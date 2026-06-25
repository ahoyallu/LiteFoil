#pragma once

#include <install/ByteBuffer.hpp>
#include <install/NcaStructs.hpp>
#include <install/NcmWrapper.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <switch.h>

namespace shield::install {

// Lightweight wrapper around packaged CNMT (content meta) data read from a
// .cnmt NCA.  Mirrors the role of nx::ncm::ContentMeta in Awoo Installer.

class ContentMeta {
    public:
        ContentMeta() = default;
        ContentMeta(const std::uint8_t *data, std::size_t size);

        PackagedContentMetaHeader GetHeader() const;
        NcmContentMetaKey         GetContentMetaKey() const;
        std::vector<NcmContentInfo> GetContentInfos() const;

        bool GetInstallContentMeta(ByteBuffer &out,
                                   const NcmContentInfo &cnmt_content_info,
                                   bool ignore_required_firmware_version) const;

    private:
        std::vector<std::uint8_t> bytes_;
};

// Read the CNMT data from an NCA that has already been installed to content
// storage.  Uses the system FS service (fsOpenFileSystemWithId) to let the OS
// handle NCA decryption and PFS0 parsing transparently.
bool ReadCnmtFromInstalledNca(ContentStorage &storage,
                               const NcmContentId &nca_id,
                               std::vector<std::uint8_t> &out_cnmt_data);

std::string NcaIdToString(const NcmContentId &id);

NcmContentId NcaIdFromString(const std::string &hex);

}
