#include <platform/CustomIndexParser.hpp>
#include <platform/TinfoilDecryptor.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string_view>

#include <third_party/nlohmann/json.hpp>

namespace shield::platform {
namespace {

using Json = nlohmann::json;
using shield::catalog::RemoteCatalog;
using shield::catalog::RemoteContentType;
using shield::catalog::RemoteFileEntry;
using shield::catalog::RemoteTitleMetadata;

std::string NormalizeTitleId(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](const unsigned char character) {
        return !std::isxdigit(character);
    }), value.end());

    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });

    return value;
}

void ApplyMetadataField(const Json &object, const char *key, std::string &target) {
    const auto value_it = object.find(key);
    if((value_it != object.end()) && value_it->is_string()) {
        target = value_it->get<std::string>();
    }
}

void ApplyUnsignedField(const Json &object, const char *key, std::uint64_t &target) {
    const auto value_it = object.find(key);
    if((value_it != object.end()) && value_it->is_number_unsigned()) {
        target = value_it->get<std::uint64_t>();
    }
}

void ApplyUnsignedField(const Json &object, const char *key, std::uint32_t &target) {
    const auto value_it = object.find(key);
    if((value_it != object.end()) && value_it->is_number_unsigned()) {
        target = value_it->get<std::uint32_t>();
    }
}

void ApplyIntegerField(const Json &object, const char *key, int &target) {
    const auto value_it = object.find(key);
    if((value_it != object.end()) && value_it->is_number_integer()) {
        target = value_it->get<int>();
    }
}

std::string UrlDecode(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());

    for(std::size_t index = 0; index < value.size(); index++) {
        const char current = value[index];
        if((current == '%') && ((index + 2) < value.size()) && std::isxdigit(static_cast<unsigned char>(value[index + 1])) && std::isxdigit(static_cast<unsigned char>(value[index + 2]))) {
            const std::string hex = std::string(value.substr(index + 1, 2));
            decoded.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
            index += 2;
        }
        else if(current == '+') {
            decoded.push_back(' ');
        }
        else {
            decoded.push_back(current);
        }
    }

    return decoded;
}

bool IsGDriveSchemeUrl(std::string_view url) {
    return url.rfind("gdrive:", 0) == 0;
}

bool IsGDriveCryptSchemeUrl(std::string_view url) {
    return url.rfind("gdrivecrypt:", 0) == 0;
}

bool IsPrivateGDriveSchemeUrl(std::string_view url) {
    return IsGDriveSchemeUrl(url) || IsGDriveCryptSchemeUrl(url);
}

bool IsPublicGoogleDriveUrl(std::string_view url) {
    return url.find("drive.google.com") != std::string_view::npos;
}

int SourcePriority(const std::string &url) {
    if(IsGDriveSchemeUrl(url)) {
        return 0;
    }
    if(IsGDriveCryptSchemeUrl(url)) {
        return 1;
    }
    if((url.rfind("https://", 0) == 0) || (url.rfind("http://", 0) == 0)) {
        return IsPublicGoogleDriveUrl(url) ? 3 : 2;
    }
    return 4;
}

std::string ExtractFilePortionFromUrl(const std::string &url) {
    // gdrive entries can carry the human file name after '#':
    //   gdrive:FILE_ID#Game%20Name%20[0100...][v0].nsp
    //   gdrivecrypt:FILE_ID#Game%20Name%20[0100...][v0].nsp
    if(IsPrivateGDriveSchemeUrl(url)) {
        const auto hash = url.find('#');
        if(hash != std::string::npos) {
            return UrlDecode(std::string_view(url).substr(hash + 1));
        }
    }

    return url;
}

std::string FindFirstTitleIdRun(const std::string &value) {
    std::string run;
    run.reserve(16);

    for(const unsigned char character : value) {
        if(std::isxdigit(character)) {
            run.push_back(static_cast<char>(std::toupper(character)));
            if(run.size() == 16) {
                return run;
            }
        }
        else {
            run.clear();
        }
    }

    return "";
}

std::string ExtractTitleIdFromUrl(const std::string &url) {
    if(IsPrivateGDriveSchemeUrl(url)) {
        const auto hash = url.find('#');
        if(hash != std::string::npos) {
            const std::string fragment = UrlDecode(std::string_view(url).substr(hash + 1));
            const std::string title_id = FindFirstTitleIdRun(fragment);
            if(!title_id.empty()) {
                return title_id;
            }
        }
    }

    const std::string decoded_url = UrlDecode(url);
    return FindFirstTitleIdRun(decoded_url);
}

std::string ExtractPackageFormatFromUrl(const std::string &url) {
    std::string path = ExtractFilePortionFromUrl(url);
    const std::size_t query_index = path.find_first_of("?#");
    if(query_index != std::string::npos) {
        path = path.substr(0, query_index);
    }

    const std::size_t slash_index = path.find_last_of('/');
    const std::string filename = (slash_index == std::string::npos) ? path : path.substr(slash_index + 1);
    const std::size_t dot_index = filename.find_last_of('.');
    if(dot_index == std::string::npos) {
        return "";
    }

    std::string format = filename.substr(dot_index + 1);
    std::transform(format.begin(), format.end(), format.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return format;
}

std::uint32_t ExtractVersionFromUrl(const std::string &url) {
    const std::string decoded_url = UrlDecode(url);
    const std::size_t marker_index = decoded_url.find("[v");
    if(marker_index == std::string::npos) {
        return 0;
    }

    const std::size_t end_index = decoded_url.find(']', marker_index + 2);
    if(end_index == std::string::npos) {
        return 0;
    }

    const std::string version = decoded_url.substr(marker_index + 2, end_index - (marker_index + 2));
    if(version.empty() || !std::all_of(version.begin(), version.end(), [](const unsigned char character) {
        return std::isdigit(character) != 0;
    })) {
        return 0;
    }

    return static_cast<std::uint32_t>(std::strtoul(version.c_str(), nullptr, 10));
}

std::string ExtractNameFromUrl(const std::string &url) {
    std::string path = ExtractFilePortionFromUrl(url);
    const std::size_t slash_index = path.find_last_of('/');
    std::string filename = (slash_index == std::string::npos) ? path : path.substr(slash_index + 1);
    const std::size_t query_index = filename.find_first_of("?#");
    if(query_index != std::string::npos) {
        filename = filename.substr(0, query_index);
    }

    filename = UrlDecode(filename);

    const std::size_t marker_index = filename.find(" [");
    if(marker_index != std::string::npos) {
        filename = filename.substr(0, marker_index);
    }
    else {
        const std::size_t bracket_index = filename.find('[');
        if(bracket_index != std::string::npos) {
            filename = filename.substr(0, bracket_index);
        }
    }

    const std::size_t dot_index = filename.find_last_of('.');
    if(dot_index != std::string::npos) {
        filename = filename.substr(0, dot_index);
    }

    while(!filename.empty() && std::isspace(static_cast<unsigned char>(filename.back()))) {
        filename.pop_back();
    }

    return filename;
}

bool TryParseHexTitleId(const std::string &value, std::uint64_t &out) {
    const std::string normalized = NormalizeTitleId(value);
    if(normalized.size() != 16) {
        return false;
    }

    out = std::strtoull(normalized.c_str(), nullptr, 16);
    return true;
}

std::string FormatTitleId(std::uint64_t title_id) {
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(title_id));
    return std::string(buffer);
}

RemoteContentType InferContentTypeFromUrl(const std::string &url) {
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });

    if((lower.find("/update/") != std::string::npos) || (lower.find("/updates/") != std::string::npos) || (lower.find("/patch/") != std::string::npos)) {
        return RemoteContentType::Update;
    }
    if((lower.find("/dlc/") != std::string::npos) || (lower.find("/addon/") != std::string::npos) || (lower.find("/add-on/") != std::string::npos)) {
        return RemoteContentType::Dlc;
    }
    if(lower.find("/base/") != std::string::npos) {
        return RemoteContentType::BaseGame;
    }

    return RemoteContentType::Unknown;
}

RemoteContentType InferContentTypeFromTitleId(const std::string &title_id) {
    std::uint64_t parsed_title_id = 0;
    if(!TryParseHexTitleId(title_id, parsed_title_id)) {
        return RemoteContentType::Unknown;
    }

    const std::uint64_t suffix = parsed_title_id & 0xFFFULL;
    if(suffix == 0x800ULL) {
        return RemoteContentType::Update;
    }
    if(suffix == 0x000ULL) {
        return RemoteContentType::BaseGame;
    }

    return RemoteContentType::Dlc;
}

std::string ResolveBaseTitleId(const std::string &title_id, const RemoteContentType content_type) {
    std::uint64_t parsed_title_id = 0;
    if(!TryParseHexTitleId(title_id, parsed_title_id)) {
        return "";
    }

    const std::uint64_t suffix = parsed_title_id & 0xFFFULL;
    switch(content_type) {
        case RemoteContentType::Update:
            if(suffix == 0x800ULL) {
                return FormatTitleId(parsed_title_id - 0x800ULL);
            }
            if(suffix == 0x000ULL) {
                return FormatTitleId(parsed_title_id);
            }
            return FormatTitleId(parsed_title_id & ~0xFFFULL);
        case RemoteContentType::BaseGame:
            return FormatTitleId(parsed_title_id);
        case RemoteContentType::Dlc:
            if(suffix == 0x000ULL) {
                return FormatTitleId(parsed_title_id);
            }
            return FormatTitleId((parsed_title_id ^ 0x1000ULL) & ~0xFFFULL);
        case RemoteContentType::Unknown:
        default:
            return "";
    }
}

void MergeMetadata(RemoteTitleMetadata &target, const RemoteTitleMetadata &source) {
    if(target.id.empty()) {
        target.id = source.id;
    }
    if(target.base_title_id.empty()) {
        target.base_title_id = source.base_title_id;
    }
    if(target.name.empty()) {
        target.name = source.name;
    }
    if(target.file_name.empty()) {
        target.file_name = source.file_name;
    }
    if(target.publisher.empty()) {
        target.publisher = source.publisher;
    }
    if(target.description.empty()) {
        target.description = source.description;
    }
    if(target.intro.empty()) {
        target.intro = source.intro;
    }
    if(target.developer.empty()) {
        target.developer = source.developer;
    }
    if(target.region.empty()) {
        target.region = source.region;
    }
    if(target.icon_url.empty()) {
        target.icon_url = source.icon_url;
    }
    if(target.local_icon_path.empty()) {
        target.local_icon_path = source.local_icon_path;
    }
    if(target.package_format.empty()) {
        target.package_format = source.package_format;
    }
    if(target.categories.empty()) {
        target.categories = source.categories;
    }
    if(target.languages.empty()) {
        target.languages = source.languages;
    }
    if(target.screenshots.empty()) {
        target.screenshots = source.screenshots;
    }
    if(target.version == 0) {
        target.version = source.version;
    }
    if(target.size == 0) {
        target.size = source.size;
    }
    if(target.release_date == 0) {
        target.release_date = source.release_date;
    }
    if(target.critic_score < 0.0) {
        target.critic_score = source.critic_score;
    }
    if(target.review_count < 0) {
        target.review_count = source.review_count;
    }
    if(target.recommendation_source.empty()) {
        target.recommendation_source = source.recommendation_source;
    }
    if(target.content_type == RemoteContentType::Unknown) {
        target.content_type = source.content_type;
    }
    if(target.number_of_players == 0) {
        target.number_of_players = source.number_of_players;
    }
    if(target.rating == 0) {
        target.rating = source.rating;
    }
    target.rank = std::max(target.rank, source.rank);
    target.version = std::max(target.version, source.version);
}

void ApplyUrlFallbackToMetadata(RemoteTitleMetadata &title_metadata, const std::string &url) {
    if(title_metadata.id.empty()) {
        title_metadata.id = NormalizeTitleId(ExtractTitleIdFromUrl(url));
    }
    if(title_metadata.name.empty()) {
        title_metadata.name = ExtractNameFromUrl(url);
    }
    const std::string derived_file_name = ExtractNameFromUrl(url);
    if(!derived_file_name.empty()) {
        title_metadata.file_name = derived_file_name;
    }
    if(title_metadata.package_format.empty()) {
        title_metadata.package_format = ExtractPackageFormatFromUrl(url);
    }
    if(title_metadata.version == 0) {
        title_metadata.version = ExtractVersionFromUrl(url);
    }

    const RemoteContentType url_content_type = InferContentTypeFromUrl(url);
    const auto title_id_content_type = InferContentTypeFromTitleId(title_metadata.id);
    if(title_id_content_type != RemoteContentType::Unknown) {
        title_metadata.content_type = title_id_content_type;
    }
    else if(url_content_type != RemoteContentType::Unknown) {
        title_metadata.content_type = url_content_type;
    }
    else if(title_metadata.content_type == RemoteContentType::Unknown) {
        title_metadata.content_type = title_id_content_type;
    }
    if(title_metadata.base_title_id.empty()) {
        title_metadata.base_title_id = ResolveBaseTitleId(title_metadata.id, title_metadata.content_type);
    }
}

void ApplyObjectToMetadata(const Json &object, RemoteTitleMetadata &title_metadata) {
    ApplyMetadataField(object, "name", title_metadata.name);
    ApplyMetadataField(object, "publisher", title_metadata.publisher);
    ApplyMetadataField(object, "description", title_metadata.description);
    ApplyMetadataField(object, "intro", title_metadata.intro);
    ApplyMetadataField(object, "developer", title_metadata.developer);
    ApplyMetadataField(object, "region", title_metadata.region);
    ApplyMetadataField(object, "iconUrl", title_metadata.icon_url);
    ApplyMetadataField(object, "packageFormat", title_metadata.package_format);
    ApplyUnsignedField(object, "version", title_metadata.version);
    ApplyUnsignedField(object, "size", title_metadata.size);
    ApplyUnsignedField(object, "releaseDate", title_metadata.release_date);
    ApplyIntegerField(object, "rank", title_metadata.rank);
}

const RemoteFileEntry *FindBestFileEntryForTitle(const std::vector<RemoteFileEntry> &files, const std::string &title_id) {
    const std::string normalized_title_id = NormalizeTitleId(title_id);
    if(normalized_title_id.empty()) {
        return nullptr;
    }

    for(const auto &file : files) {
        if(NormalizeTitleId(ExtractTitleIdFromUrl(file.url)) == normalized_title_id) {
            return &file;
        }
    }

    return nullptr;
}

std::optional<RemoteCatalog> ParseJson(const Json &json) {
    if(!json.is_object()) {
        return std::nullopt;
    }

    RemoteCatalog catalog;

    const auto files_it = json.find("files");
    if((files_it != json.end()) && files_it->is_array()) {
        for(const auto &entry : *files_it) {
            if(entry.is_string()) {
                catalog.files.push_back({ entry.get<std::string>(), 0 });
            }
            else if(entry.is_object()) {
                const auto url_it = entry.find("url");
                if((url_it != entry.end()) && url_it->is_string()) {
                    RemoteFileEntry file_entry;
                    file_entry.url = url_it->get<std::string>();

                    const auto size_it = entry.find("size");
                    if((size_it != entry.end()) && size_it->is_number_unsigned()) {
                        file_entry.size = size_it->get<std::uint64_t>();
                    }

                    catalog.files.push_back(std::move(file_entry));
                }

                RemoteTitleMetadata title_metadata;
                const auto id_it = entry.find("id");
                if((id_it != entry.end()) && id_it->is_string()) {
                    title_metadata.id = NormalizeTitleId(id_it->get<std::string>());
                }

                const auto title_id_it = entry.find("titleId");
                if(title_metadata.id.empty() && (title_id_it != entry.end()) && title_id_it->is_string()) {
                    title_metadata.id = NormalizeTitleId(title_id_it->get<std::string>());
                }

                ApplyObjectToMetadata(entry, title_metadata);
                if((url_it != entry.end()) && url_it->is_string()) {
                    ApplyUrlFallbackToMetadata(title_metadata, url_it->get<std::string>());
                }
                if(!title_metadata.id.empty()) {
                    MergeMetadata(catalog.titles_by_id[title_metadata.id], title_metadata);
                }
            }
        }
    }

    const auto directories_it = json.find("directories");
    if((directories_it != json.end()) && directories_it->is_array()) {
        for(const auto &entry : *directories_it) {
            if(entry.is_string()) {
                catalog.directories.push_back(entry.get<std::string>());
            }
        }
    }

    const auto success_it = json.find("success");
    if((success_it != json.end()) && success_it->is_string()) {
        catalog.success_message = success_it->get<std::string>();
    }

    const auto error_it = json.find("error");
    if((error_it != json.end()) && error_it->is_string()) {
        catalog.error_message = error_it->get<std::string>();
    }

    const auto titledb_it = json.find("titledb");
    if((titledb_it != json.end()) && titledb_it->is_object()) {
        for(const auto &[title_id, title_value] : titledb_it->items()) {
            if(!title_value.is_object()) {
                continue;
            }

            RemoteTitleMetadata title_metadata;
            title_metadata.id = NormalizeTitleId(title_id);

            const auto id_it = title_value.find("id");
            if((id_it != title_value.end()) && id_it->is_string()) {
                title_metadata.id = NormalizeTitleId(id_it->get<std::string>());
            }

            ApplyObjectToMetadata(title_value, title_metadata);
            if(title_metadata.content_type == RemoteContentType::Unknown) {
                title_metadata.content_type = InferContentTypeFromTitleId(title_metadata.id);
            }
            if(title_metadata.base_title_id.empty()) {
                title_metadata.base_title_id = ResolveBaseTitleId(title_metadata.id, title_metadata.content_type);
            }

            MergeMetadata(catalog.titles_by_id[title_metadata.id], title_metadata);
        }
    }

    for(auto &[title_id, metadata] : catalog.titles_by_id) {
        if(metadata.id.empty()) {
            metadata.id = title_id;
        }
    }

    const auto recommended_it = json.find("recommended");
    if((recommended_it != json.end()) && recommended_it->is_object()) {
        for(const auto &[raw_title_id, recommended_value] : recommended_it->items()) {
            if(!recommended_value.is_object()) {
                continue;
            }

            const std::string title_id = NormalizeTitleId(raw_title_id);
            if(title_id.size() != 16) {
                continue;
            }

            const RemoteFileEntry *file_entry = FindBestFileEntryForTitle(catalog.files, title_id);
            if(file_entry == nullptr) {
                continue;
            }

            RemoteTitleMetadata recommended_metadata;
            const auto existing_it = catalog.titles_by_id.find(title_id);
            if(existing_it != catalog.titles_by_id.end()) {
                recommended_metadata = existing_it->second;
            }

            recommended_metadata.id = title_id;
            const auto name_it = recommended_value.find("name");
            if(recommended_metadata.name.empty() && (name_it != recommended_value.end()) && name_it->is_string()) {
                recommended_metadata.name = name_it->get<std::string>();
            }
            ApplyUrlFallbackToMetadata(recommended_metadata, file_entry->url);
            if(recommended_metadata.size == 0) {
                recommended_metadata.size = file_entry->size;
            }

            const auto critic_score_it = recommended_value.find("critic_score");
            if((critic_score_it != recommended_value.end()) && critic_score_it->is_number()) {
                recommended_metadata.critic_score = critic_score_it->get<double>();
            }

            const auto review_count_it = recommended_value.find("review_count");
            if((review_count_it != recommended_value.end()) && (review_count_it->is_number_integer() || review_count_it->is_number_unsigned())) {
                recommended_metadata.review_count = review_count_it->get<int>();
            }

            const auto source_it = recommended_value.find("source");
            if((source_it != recommended_value.end()) && source_it->is_string()) {
                recommended_metadata.recommendation_source = source_it->get<std::string>();
            }

            catalog.recommended_titles.push_back(std::move(recommended_metadata));
        }
    }

    // Queue resolution scans catalog.files and picks the first URL matching a
    // Title ID.  Keep hybrid feeds deterministic by preferring normal private
    // gdrive: entries, then encrypted gdrivecrypt:, then regular direct HTTP links,
    // then public Google Drive links.
    std::stable_sort(catalog.files.begin(), catalog.files.end(), [](const RemoteFileEntry &left, const RemoteFileEntry &right) {
        return SourcePriority(left.url) < SourcePriority(right.url);
    });

    return catalog;
}

}

std::optional<RemoteCatalog> CustomIndexParser::ParseString(const std::string &raw) {
    // Try known encrypted envelope formats first. On failure, fall back to
    // plain JSON so existing indexes continue to work unchanged.
    const auto decoded = TryDecodeRemoteJsonPayload(raw, true);
    const std::string &effective = decoded.has_value() ? decoded.value() : raw;

    const Json json = Json::parse(effective, nullptr, false);
    if(json.is_discarded()) {
        return std::nullopt;
    }

    return ParseJson(json);
}

std::optional<RemoteCatalog> CustomIndexParser::ParseFile(const std::string &path) {
    // Read as binary so TryTinfoilDecrypt can inspect the raw bytes.
    std::ifstream stream(path, std::ios::binary);
    if(!stream.good()) {
        return std::nullopt;
    }
    const std::string raw((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());

    const auto decoded = TryDecodeRemoteJsonPayload(raw, true);
    const std::string &effective = decoded.has_value() ? decoded.value() : raw;

    const Json json = Json::parse(effective, nullptr, false);
    if(json.is_discarded()) {
        return std::nullopt;
    }

    return ParseJson(json);
}

}
