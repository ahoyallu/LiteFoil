#include <catalog/UpdateCandidate.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>

namespace shield::catalog {
namespace {

std::string NormalizeTitleId(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](const unsigned char character) {
        return !std::isxdigit(character);
    }), value.end());

    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });

    return value;
}

std::string ComputeUpdateTitleId(const std::string &base_title_id) {
    const std::string normalized = NormalizeTitleId(base_title_id);
    if(normalized.size() != 16) {
        return "";
    }

    const unsigned long long raw = std::strtoull(normalized.c_str(), nullptr, 16);
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llX", raw + 0x800ULL);
    return std::string(buffer);
}

bool HasUpdateTitleIdSuffix(const std::string &title_id) {
    const std::string normalized = NormalizeTitleId(title_id);
    if(normalized.size() != 16) {
        return false;
    }

    const std::uint64_t raw = std::strtoull(normalized.c_str(), nullptr, 16);
    return (raw & 0xFFFULL) == 0x800ULL;
}

u32 GetInstalledComparableVersion(const InstalledTitle &installed_title, const RemoteTitleMetadata & /*remote_title*/) {
    // content_version = max(patch_version, application_version).  This is
    // the effective runtime version of the title on the console, and it is
    // the only value that yields correct comparisons for every remote
    // content_type: any remote package (base or update) older than the
    // highest installed version is a downgrade and must be rejected.
    //
    // Using the per-type field (e.g. application_content_version for
    // BaseGame) breaks when a patch is already installed because NCM reports
    // the base application version as 0 once patched, making any non-zero
    // remote version look like a valid upgrade.
    return installed_title.content_version;
}

}

std::vector<UpdateCandidate> UpdatePlanner::Build(const std::vector<InstalledTitle> &installed_titles, const RemoteCatalog &remote_catalog) {
    std::vector<UpdateCandidate> candidates;
    candidates.reserve(installed_titles.size());

    for(const auto &installed_title : installed_titles) {
        const std::string installed_id = NormalizeTitleId(installed_title.title_id_hex);
        const std::string installed_update_id = ComputeUpdateTitleId(installed_id);
        const RemoteTitleMetadata *best_match = nullptr;

        for(const auto &[remote_id, remote_title] : remote_catalog.titles_by_id) {
            const std::string normalized_remote_id = NormalizeTitleId(remote_id);
            const std::string normalized_metadata_id = NormalizeTitleId(remote_title.id);
            const std::string normalized_base_id = NormalizeTitleId(remote_title.base_title_id);
            const bool direct_match = (normalized_remote_id == installed_id) || (normalized_metadata_id == installed_id);
            const bool direct_update_match = (normalized_remote_id == installed_update_id) || (normalized_metadata_id == installed_update_id);
            const bool remote_has_update_suffix = HasUpdateTitleIdSuffix(normalized_remote_id) || HasUpdateTitleIdSuffix(normalized_metadata_id);
            const bool patch_shape_match = direct_update_match || ((normalized_base_id == installed_id) && remote_has_update_suffix);
            const bool is_update_like = direct_update_match || (remote_title.content_type == RemoteContentType::Update && remote_has_update_suffix);
            const bool is_base_like = (remote_title.content_type == RemoteContentType::BaseGame) || direct_match;
            const bool is_candidate = (is_update_like && patch_shape_match) || (is_base_like && direct_match);
            if(!is_candidate || (remote_title.version <= GetInstalledComparableVersion(installed_title, remote_title))) {
                continue;
            }

            if((best_match == nullptr) || (remote_title.version > best_match->version)) {
                best_match = std::addressof(remote_title);
            }
        }

        if(best_match != nullptr) {
            candidates.push_back({ installed_title, *best_match });
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const UpdateCandidate &left, const UpdateCandidate &right) {
        if(left.remote.version != right.remote.version) {
            return left.remote.version > right.remote.version;
        }

        return left.installed.name < right.installed.name;
    });

    return candidates;
}

}
