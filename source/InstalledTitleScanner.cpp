#include <platform/InstalledTitleScanner.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace shield::platform {
namespace {

constexpr s32 kApplicationRecordBatchSize = 32;
constexpr s32 kContentStatusBatchSize = 16;

std::string ReadFixedString(const char *buffer, const size_t buffer_size) {
    size_t actual_size = 0;
    while((actual_size < buffer_size) && (buffer[actual_size] != '\0')) {
        actual_size++;
    }

    return std::string(buffer, actual_size);
}

std::string FormatApplicationId(const u64 application_id) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << application_id;
    return stream.str();
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

Result GetApplicationControlData(const u64 application_id, NsApplicationControlData &out_data, size_t &out_icon_size) {
    u64 actual_size = 0;
    Result rc;

    // Newer firmwares expose a faster variant, but the legacy path keeps older systems working too.
    if(hosversionAtLeast(19, 0, 0)) {
        rc = nsGetApplicationControlData2(NsApplicationControlSource_Storage, application_id, std::addressof(out_data), sizeof(out_data), 0xFF, 0, &actual_size, nullptr);
    }
    else {
        rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, application_id, std::addressof(out_data), sizeof(out_data), &actual_size);
    }

    if(R_SUCCEEDED(rc) && (actual_size >= sizeof(NacpStruct))) {
        out_icon_size = static_cast<size_t>(actual_size - sizeof(NacpStruct));
    }
    else {
        out_icon_size = 0;
    }

    return rc;
}

void FillControlData(const u64 application_id, shield::catalog::InstalledTitle &title) {
    NsApplicationControlData control_data = {};
    size_t icon_size = 0;
    if(R_FAILED(GetApplicationControlData(application_id, control_data, icon_size))) {
        return;
    }

    title.has_icon = icon_size > 0;
    title.icon_size = icon_size;
    title.display_version = ReadFixedString(control_data.nacp.display_version, sizeof(control_data.nacp.display_version));

    NacpLanguageEntry *language_entry = nullptr;
    if(R_SUCCEEDED(nacpGetLanguageEntry(&control_data.nacp, &language_entry)) && (language_entry != nullptr)) {
        title.name = ReadFixedString(language_entry->name, sizeof(language_entry->name));
        title.author = ReadFixedString(language_entry->author, sizeof(language_entry->author));
    }
}

void FillContentStatus(const u64 application_id, shield::catalog::InstalledTitle &title) {
    NsApplicationContentMetaStatus status_batch[kContentStatusBatchSize] = {};
    s32 offset = 0;
    u32 application_version = 0;
    u32 patch_version = 0;

    while(true) {
        s32 entry_count = 0;
        if(R_FAILED(nsListApplicationContentMetaStatus(application_id, offset, status_batch, kContentStatusBatchSize, &entry_count))) {
            break;
        }

        if(entry_count == 0) {
            break;
        }

        for(s32 i = 0; i < entry_count; i++) {
            const auto &status = status_batch[i];
            if(status.meta_type == NcmContentMetaType_Patch) {
                if(status.version >= patch_version) {
                    patch_version = status.version;
                    title.storage_id = status.storageID;
                }
            }
            else if(status.meta_type == NcmContentMetaType_Application) {
                if(status.version >= application_version) {
                    application_version = status.version;
                    if(title.storage_id == NcmStorageId_None) {
                        title.storage_id = status.storageID;
                    }
                }
            }
            else if(status.meta_type == NcmContentMetaType_AddOnContent) {
                title.add_on_title_ids.push_back(FormatApplicationId(status.application_id));
                if(title.storage_id == NcmStorageId_None) {
                    title.storage_id = status.storageID;
                }
            }
            else if(title.storage_id == NcmStorageId_None) {
                title.storage_id = status.storageID;
            }
        }

        offset += entry_count;
        if(entry_count < kContentStatusBatchSize) {
            break;
        }
    }

    title.application_content_version = application_version;
    title.patch_content_version = patch_version;
    title.content_version = patch_version > 0 ? patch_version : application_version;
}

}

std::vector<shield::catalog::InstalledTitle> InstalledTitleScanner::Scan() {
    std::vector<shield::catalog::InstalledTitle> titles;

    if(R_FAILED(nsInitialize())) {
        return titles;
    }

    NsApplicationRecord record_batch[kApplicationRecordBatchSize] = {};
    s32 offset = 0;

    while(true) {
        s32 entry_count = 0;
        if(R_FAILED(nsListApplicationRecord(record_batch, kApplicationRecordBatchSize, offset, &entry_count))) {
            break;
        }

        if(entry_count == 0) {
            break;
        }

        for(s32 i = 0; i < entry_count; i++) {
            const auto &record = record_batch[i];
            if(record.application_id == 0) {
                continue;
            }

            shield::catalog::InstalledTitle title = {};
            title.application_id = record.application_id;
            title.title_id_hex = FormatApplicationId(record.application_id);
            title.name = title.title_id_hex;
            title.storage_id = NcmStorageId_None;

            // The scanner keeps name/version extraction independent from content meta parsing so each step can fail safely.
            FillControlData(record.application_id, title);
            FillContentStatus(record.application_id, title);

            titles.push_back(std::move(title));
        }

        offset += entry_count;
        if(entry_count < kApplicationRecordBatchSize) {
            break;
        }
    }

    std::sort(titles.begin(), titles.end(), [](const shield::catalog::InstalledTitle &left, const shield::catalog::InstalledTitle &right) {
        return ToLowerAscii(left.name) < ToLowerAscii(right.name);
    });

    return titles;
}

}
