#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace shield::install {

struct Hfs0Entry {
    std::string   name;
    std::uint64_t offset;
    std::uint64_t size;
};

// High-level representation of the XCI's root HFS0 partition structure.
// An XCI image contains a root HFS0 which indexes named partitions
// (update, normal, secure, logo).  Each partition is itself an HFS0.
struct XciPartition {
    std::string              name;
    std::uint64_t            offset;
    std::uint64_t            size;
    std::vector<Hfs0Entry>   entries;
    std::uint64_t            data_offset;
};

// Parse the root HFS0 from an XCI file and recursively parse its named
// partitions.  The "secure" partition contains the NCAs used for install.
bool ParseXci(const std::string &path, std::vector<XciPartition> &partitions);

const XciPartition *FindPartition(const std::vector<XciPartition> &partitions,
                                   const std::string &name);

const Hfs0Entry *FindHfs0EntryByExtension(const std::vector<Hfs0Entry> &entries,
                                            const std::string &extension);

std::vector<const Hfs0Entry *> FindHfs0EntriesByExtension(
    const std::vector<Hfs0Entry> &entries,
    const std::string &extension);

const Hfs0Entry *FindHfs0EntryByNcaId(const std::vector<Hfs0Entry> &entries,
                                        const std::string &nca_id_hex);

}
