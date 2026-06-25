#include <install/XciParser.hpp>
#include <install/NcaStructs.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace shield::install {
namespace {

bool EndsWith(const std::string &value, const std::string &suffix) {
    if(suffix.size() > value.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin(),
                      [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

bool ParseHfs0(FILE *file, std::uint64_t hfs0_offset,
               std::vector<Hfs0Entry> &entries,
               std::uint64_t &data_offset) {
    entries.clear();
    data_offset = 0;

    if(std::fseek(file, static_cast<long>(hfs0_offset), SEEK_SET) != 0) {
        return false;
    }

    Hfs0Header header{};
    if(std::fread(&header, sizeof(header), 1, file) != 1) {
        return false;
    }

    if(header.magic != kMagicHfs0) {
        return false;
    }

    if(header.file_count == 0) {
        return true;
    }

    const std::size_t entry_table_size = header.file_count * sizeof(Hfs0FileEntry);
    auto entry_buf = std::make_unique<std::uint8_t[]>(entry_table_size);
    if(std::fread(entry_buf.get(), entry_table_size, 1, file) != 1) {
        return false;
    }

    auto string_buf = std::make_unique<char[]>(header.string_table_size);
    if(std::fread(string_buf.get(), header.string_table_size, 1, file) != 1) {
        return false;
    }

    const std::uint64_t data_start = hfs0_offset + sizeof(Hfs0Header)
                                     + entry_table_size
                                     + header.string_table_size;
    data_offset = data_start;

    const auto *raw_entries = reinterpret_cast<const Hfs0FileEntry *>(entry_buf.get());
    entries.reserve(header.file_count);

    for(std::uint32_t i = 0; i < header.file_count; i++) {
        Hfs0Entry entry;
        if(raw_entries[i].string_table_offset < header.string_table_size) {
            entry.name = &string_buf[raw_entries[i].string_table_offset];
        }
        entry.offset = data_start + raw_entries[i].data_offset;
        entry.size   = raw_entries[i].file_size;
        entries.push_back(std::move(entry));
    }

    return true;
}

}

bool ParseXci(const std::string &path, std::vector<XciPartition> &partitions) {
    partitions.clear();

    FILE *file = std::fopen(path.c_str(), "rb");
    if(!file) {
        return false;
    }

    if(std::fseek(file, kXciHeaderOffset, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }

    XciHeader xci_header{};
    if(std::fread(&xci_header, sizeof(xci_header), 1, file) != 1) {
        std::fclose(file);
        return false;
    }

    if(xci_header.magic != kMagicHead) {
        std::fclose(file);
        return false;
    }

    const std::uint64_t root_hfs0_offset = xci_header.hfs0_partition_offset;

    std::vector<Hfs0Entry> root_entries;
    std::uint64_t root_data_offset = 0;
    if(!ParseHfs0(file, root_hfs0_offset, root_entries, root_data_offset)) {
        std::fclose(file);
        return false;
    }

    for(const auto &root_entry : root_entries) {
        XciPartition partition;
        partition.name   = root_entry.name;
        partition.offset = root_entry.offset;
        partition.size   = root_entry.size;

        ParseHfs0(file, root_entry.offset, partition.entries, partition.data_offset);
        partitions.push_back(std::move(partition));
    }

    std::fclose(file);
    return true;
}

const XciPartition *FindPartition(const std::vector<XciPartition> &partitions,
                                   const std::string &name) {
    for(const auto &p : partitions) {
        if(p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

const Hfs0Entry *FindHfs0EntryByExtension(const std::vector<Hfs0Entry> &entries,
                                            const std::string &extension) {
    for(const auto &entry : entries) {
        if(EndsWith(entry.name, extension)) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<const Hfs0Entry *> FindHfs0EntriesByExtension(
    const std::vector<Hfs0Entry> &entries,
    const std::string &extension) {
    std::vector<const Hfs0Entry *> result;
    for(const auto &entry : entries) {
        if(EndsWith(entry.name, extension)) {
            result.push_back(&entry);
        }
    }
    return result;
}

const Hfs0Entry *FindHfs0EntryByNcaId(const std::vector<Hfs0Entry> &entries,
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

}
