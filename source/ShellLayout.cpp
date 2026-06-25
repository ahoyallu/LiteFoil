#include <ui/ShellLayout.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unordered_set>
#include <vector>

#include <switch.h>

#include <AppVersion.hpp>
#include <platform/SystemStatus.hpp>

#ifndef LITEFOIL_APP_VERSION
#define LITEFOIL_APP_VERSION "dev"
#endif

namespace shield::ui {
namespace {

using shield::app::AppSection;
using shield::app::AppConfig;
using shield::catalog::InstalledTitle;
using shield::catalog::RemoteContentType;
using shield::catalog::RemoteCatalogState;
using shield::catalog::RemoteTitleMetadata;
using shield::catalog::UpdateCandidate;
using shield::ui::ShellCardContent;

constexpr pu::ui::Color kShellBackground = { 18, 20, 24, 255 };
constexpr pu::ui::Color kSidebarBackground = { 28, 31, 39, 255 };
constexpr pu::ui::Color kSidebarSelected = { 245, 246, 250, 255 };
constexpr pu::ui::Color kBrandRed = { 220, 32, 38, 255 };
constexpr pu::ui::Color kSoftText = { 202, 209, 219, 255 };
constexpr pu::ui::Color kMutedText = { 148, 155, 168, 255 };
constexpr pu::ui::Color kDarkText = { 24, 26, 30, 255 };
constexpr pu::ui::Color kHeroText = { 252, 252, 252, 255 };
constexpr pu::ui::Color kHeroSubtext = { 230, 232, 236, 255 };
constexpr pu::ui::Color kStatusTrack = { 72, 77, 88, 255 };
constexpr pu::ui::Color kStatusFill = { 128, 232, 216, 255 };
constexpr pu::ui::Color kStatusOffline = { 108, 113, 124, 255 };
constexpr pu::ui::Color kStatusOnline = { 128, 232, 216, 255 };
constexpr pu::ui::Color kCardFocusOutline = { 244, 245, 248, 255 };
// Single card-accent colour shared by every list view (queue, installed,
// new games, updates, DLCs, settings). Dark theme = wine; light theme =
// soft pink surface with a uniform salmon badge (see kCardBadgeLight).
constexpr pu::ui::Color kCardAccentDark  = { 0x64, 0x0B, 0x05, 0xFF };
constexpr pu::ui::Color kCardAccentLight = { 0xF3, 0xCF, 0xCD, 0xFF };
constexpr pu::ui::Color kCardBadgeLight  = { 0xF5, 0xBC, 0xB0, 0xFF };
constexpr s32 kGridColumns = 4;
constexpr s32 kGridCardWidth = 336;
constexpr s32 kGridCardHeight = 336;

struct ThemePalette {
    pu::ui::Color shell_background;
    pu::ui::Color sidebar_background;
    pu::ui::Color sidebar_brand_background;
    pu::ui::Color top_bar_background;
    pu::ui::Color app_brand_text;
    pu::ui::Color app_brand_subtext;    
    pu::ui::Color sidebar_selected;
    pu::ui::Color selected_text;
    pu::ui::Color normal_text;
    pu::ui::Color subtle_text;
    pu::ui::Color hero_text;
    pu::ui::Color hero_subtext;
    pu::ui::Color hero_background;
    pu::ui::Color hero_badge_background;
    pu::ui::Color progress_track;
    pu::ui::Color progress_fill;
    pu::ui::Color offline_status;
    pu::ui::Color online_status;
    pu::ui::Color queue_panel;
    pu::ui::Color queue_badge_background;
    pu::ui::Color queue_artwork_background;
    pu::ui::Color card_artwork_background;
    pu::ui::Color card_details_background;
    pu::ui::Color card_badge_background;
    pu::ui::Color focus_outline;
};

std::string TrimWhitespace(std::string value);
std::string TrimForDisplay(const std::string &value, const std::size_t max_length);
std::string ToLowerAscii(std::string value);
std::unordered_set<std::string> BuildInstalledTitleIdSet(const std::vector<InstalledTitle> &installed_titles);

std::uint32_t ExtractVersionFromUrl(const std::string &url) {
    const std::size_t marker_index = url.find("[v");
    if(marker_index == std::string::npos) {
        return 0;
    }

    const std::size_t end_index = url.find(']', marker_index + 2);
    if(end_index == std::string::npos) {
        return 0;
    }

    const std::string version = url.substr(marker_index + 2, end_index - (marker_index + 2));
    if(version.empty() || !std::all_of(version.begin(), version.end(), [](const unsigned char character) {
        return std::isdigit(character) != 0;
    })) {
        return 0;
    }

    return static_cast<std::uint32_t>(std::strtoul(version.c_str(), nullptr, 10));
}

bool UrlContainsTitleId(const std::string &url, const std::string &title_id) {
    return std::search(url.begin(), url.end(), title_id.begin(), title_id.end(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }
    ) != url.end();
}

int SourcePriority(const std::string &url) {
    if(url.rfind("gdrive:", 0) == 0) {
        return 0;
    }
    if(url.rfind("gdrivecrypt:", 0) == 0) {
        return 1;
    }
    if((url.rfind("https://", 0) == 0) || (url.rfind("http://", 0) == 0)) {
        return url.find("drive.google.com") == std::string::npos ? 2 : 3;
    }
    return 4;
}

std::string FindBestFileUrlForTitle(const std::vector<shield::catalog::RemoteFileEntry> &files, const std::string &title_id) {
    const shield::catalog::RemoteFileEntry *best_file = nullptr;
    std::uint32_t best_version = 0;
    int best_priority = 100;

    for(const auto &file : files) {
        if(!UrlContainsTitleId(file.url, title_id)) {
            continue;
        }

        const std::uint32_t file_version = ExtractVersionFromUrl(file.url);
        const int priority = SourcePriority(file.url);
        if((best_file == nullptr) || (file_version > best_version) || ((file_version == best_version) && (priority < best_priority))) {
            best_file = std::addressof(file);
            best_version = file_version;
            best_priority = priority;
        }
    }

    return best_file == nullptr ? "" : best_file->url;
}

std::string BuildAppDisplayName(const std::string &app_name) {
    constexpr const char *version = LITEFOIL_APP_VERSION;
    if((version == nullptr) || (version[0] == '\0')) {
        return app_name;
    }

    std::string display_version = version;
    const std::size_t first_dot = display_version.find('.');
    if(first_dot == std::string::npos) {
        display_version += ".0.0";
    }
    else if(display_version.find('.', first_dot + 1) == std::string::npos) {
        display_version += ".0";
    }
    return app_name + " v" + display_version;
}

std::string BuildQueueItemIdentity(const shield::catalog::QueueItem &item) {
    constexpr char kSeparator = '\x1F';
    return item.title_id + kSeparator
        + item.base_title_id + kSeparator
        + item.name + kSeparator
        + item.source_url + kSeparator
        + item.package_format + kSeparator
        + std::to_string(item.size);
}

void ApplyStableClampText(const pu::ui::elm::TextBlock::Ref &text_block, const std::string &value, const s32 clamp_width, const std::size_t trim_length, const std::size_t restart_padding) {
    if(text_block == nullptr) {
        return;
    }

    const std::string trimmed = TrimWhitespace(value);
    if(trimmed.empty()) {
        text_block->SetText("");
        text_block->SetClampWidth(std::max(4096, clamp_width * 4));
        text_block->SetClampDelay(0);
        return;
    }

    const bool should_marquee = (trimmed.size() >= trim_length) && (clamp_width > 0);
    if(!should_marquee) {
        text_block->SetClampWidth(std::max(4096, clamp_width * 4));
        text_block->SetText(TrimForDisplay(trimmed, trim_length));
        text_block->SetClampDelay(0);
        return;
    }

    const std::size_t padding_count = std::max<std::size_t>(restart_padding, trim_length + 22);
    text_block->SetClampWidth(clamp_width);
    text_block->SetText(trimmed + std::string(padding_count, ' '));
    text_block->SetClampDelay(3200);
}

std::string NormalizeThemeMode(std::string theme_mode) {
    std::transform(theme_mode.begin(), theme_mode.end(), theme_mode.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });

    if((theme_mode == "light") || (theme_mode == "dark")) {
        return theme_mode;
    }

    // "auto" or unrecognised: resolve from the console colour set once and cache.
    static std::string s_auto_theme;
    static bool s_detected = false;
    if(!s_detected) {
        s_detected = true;
        if(R_SUCCEEDED(setsysInitialize())) {
            ColorSetId color_set_id = ColorSetId_Dark;
            setsysGetColorSetId(&color_set_id);
            setsysExit();
            s_auto_theme = (color_set_id == ColorSetId_Light) ? "light" : "dark";
        } else {
            s_auto_theme = "dark";
        }
    }
    return s_auto_theme;
}

ThemePalette ResolveThemePalette(const AppConfig &config) {
    const std::string theme_mode = NormalizeThemeMode(config.theme);

    if(theme_mode == "light") {
        return {
            { 255, 255, 255, 255 },
            { 240, 240, 240, 255 },
            { 226, 34, 44, 255 },
            { 226, 34, 44, 255 },
            { 255, 252, 252, 255 },
            { 255, 241, 243, 255 },
            { 220, 220, 220, 255 },
            { 20, 24, 30, 255 },
            { 24, 29, 36, 255 },
            { 82, 90, 101, 255 },
            { 24, 29, 36, 255 },
            { 82, 90, 101, 255 },
            { 231, 235, 241, 255 },
            { 226, 34, 44, 26 },
            { 214, 219, 227, 255 },
            { 74, 170, 145, 255 },
            { 171, 177, 188, 255 },
            { 74, 170, 145, 255 },
            { 255, 255, 255, 255 },
            { 232, 236, 242, 255 },
            { 229, 233, 240, 255 },
            { 254, 254, 255, 238 },
            { 238, 241, 246, 255 },
            { 84, 161, 255, 255 }
        };
    }

    return {
        kShellBackground,
        kSidebarBackground,
        kBrandRed,
        kBrandRed,
        kHeroText,
        kHeroSubtext,
        kSidebarSelected,
        kDarkText,
        kHeroText,
        kMutedText,
        kHeroText,
        kHeroSubtext,
        { 184, 45, 58, 255 },
        { 255, 255, 255, 50 },
        kStatusTrack,
        kStatusFill,
        kStatusOffline,
        kStatusOnline,
        { 42, 46, 56, 255 },
        { 255, 255, 255, 48 },
        { 255, 255, 255, 18 },
        { 255, 255, 255, 24 },
        { 16, 18, 24, 168 },
        { 255, 255, 255, 44 },
        kCardFocusOutline
    };
}

pu::ui::Color BlendColor(const pu::ui::Color &left, const pu::ui::Color &right, const float ratio) {
    const float clamped_ratio = std::clamp(ratio, 0.0f, 1.0f);
    const auto blend_channel = [clamped_ratio](const std::uint8_t a, const std::uint8_t b) -> std::uint8_t {
        return static_cast<std::uint8_t>((static_cast<float>(a) * (1.0f - clamped_ratio)) + (static_cast<float>(b) * clamped_ratio));
    };

    return {
        blend_channel(left.r, right.r),
        blend_channel(left.g, right.g),
        blend_channel(left.b, right.b),
        blend_channel(left.a, right.a)
    };
}

std::string BuildHostLabel(std::string value) {
    value = TrimWhitespace(value);
    if(value.empty()) {
        return "";
    }

    const auto protocol = value.find("://");
    if(protocol != std::string::npos) {
        value = value.substr(protocol + 3);
    }

    const auto slash = value.find('/');
    if(slash != std::string::npos) {
        value = value.substr(0, slash);
    }

    return value;
}

std::string BuildCountLabel(const shield::i18n::I18n &translator, const std::string &prefix_key, const std::size_t count, const std::string &suffix_key) {
    return translator.Get(prefix_key) + std::to_string(count) + translator.Get(suffix_key);
}

std::string GetStorageLabel(const shield::i18n::I18n &translator, const u8 storage_id) {
    switch(storage_id) {
        case NcmStorageId_SdCard:
            return translator.Get("installed.storage.sdCard");
        case NcmStorageId_BuiltInUser:
            return translator.Get("installed.storage.builtInUser");
        case NcmStorageId_GameCard:
            return translator.Get("installed.storage.gameCard");
        case NcmStorageId_BuiltInSystem:
            return translator.Get("installed.storage.builtInSystem");
        default:
            return translator.Get("installed.storage.unknown");
    }
}

std::string GetNavigationKey(const AppSection section) {
    switch(section) {
        case AppSection::Installed:
            return "nav.installed";
        case AppSection::Recommended:
            return "nav.recommended";
        case AppSection::NewGames:
            return "nav.newGames";
        case AppSection::Updates:
            return "nav.updates";
        case AppSection::Dlcs:
            return "nav.dlcs";
        case AppSection::Queue:
            return "nav.queue";
        case AppSection::Settings:
        default:
            return "nav.settings";
    }
}

std::string GetSectionTitleKey(const AppSection section) {
    switch(section) {
        case AppSection::Installed:
            return "section.installed.title";
        case AppSection::Recommended:
            return "section.recommended.title";
        case AppSection::NewGames:
            return "section.newGames.title";
        case AppSection::Updates:
            return "section.updates.title";
        case AppSection::Dlcs:
            return "section.dlcs.title";
        case AppSection::Queue:
            return "section.queue.title";
        case AppSection::Settings:
        default:
            return "section.settings.title";
    }
}

std::string GetSectionSubtitleKey(const AppSection section) {
    switch(section) {
        case AppSection::Installed:
            return "section.installed.subtitle";
        case AppSection::Recommended:
            return "section.recommended.subtitle";
        case AppSection::NewGames:
            return "section.newGames.subtitle";
        case AppSection::Updates:
            return "section.updates.subtitle";
        case AppSection::Dlcs:
            return "section.dlcs.subtitle";
        case AppSection::Queue:
            return "section.queue.subtitle";
        case AppSection::Settings:
        default:
            return "section.settings.subtitle";
    }
}

std::string GetSectionPreviewKey(const AppSection section) {
    switch(section) {
        case AppSection::Installed:
            return "dialog.installedPreview";
        case AppSection::Recommended:
            return "dialog.recommendedPreview";
        case AppSection::NewGames:
            return "dialog.newGamesPreview";
        case AppSection::Updates:
            return "dialog.updatesPreview";
        case AppSection::Dlcs:
            return "dialog.dlcsPreview";
        case AppSection::Queue:
            return "dialog.queuePreview";
        case AppSection::Settings:
        default:
            return "dialog.settingsPreview";
    }
}

std::string NormalizeTitleIdForCompare(const std::string &title_id) {
    return ToLowerAscii(TrimWhitespace(title_id));
}

bool IsRemoteTitleInstalled(const RemoteTitleMetadata &title, const std::unordered_set<std::string> &installed_ids) {
    const std::string id = NormalizeTitleIdForCompare(title.id);
    const std::string base_id = NormalizeTitleIdForCompare(title.base_title_id.empty() ? title.id : title.base_title_id);
    return (!id.empty() && (installed_ids.find(id) != installed_ids.end()))
        || (!base_id.empty() && (installed_ids.find(base_id) != installed_ids.end()));
}

bool HasVisibleRecommendedTitles(const RemoteCatalogState &remote_catalog_state, const std::vector<InstalledTitle> &installed_titles) {
    if(!remote_catalog_state.loaded || remote_catalog_state.catalog.recommended_titles.empty()) {
        return false;
    }

    const auto installed_ids = BuildInstalledTitleIdSet(installed_titles);
    return std::any_of(remote_catalog_state.catalog.recommended_titles.begin(), remote_catalog_state.catalog.recommended_titles.end(),
        [&installed_ids](const RemoteTitleMetadata &title) {
            return !IsRemoteTitleInstalled(title, installed_ids);
        });
}

bool IsSectionVisible(const AppSection section, const RemoteCatalogState &remote_catalog_state, const std::vector<InstalledTitle> &installed_titles) {
    if(section == AppSection::Recommended) {
        return HasVisibleRecommendedTitles(remote_catalog_state, installed_titles);
    }
    return true;
}

std::vector<AppSection> BuildVisibleSections(const RemoteCatalogState &remote_catalog_state, const std::vector<InstalledTitle> &installed_titles) {
    std::vector<AppSection> sections;
    sections.reserve(shield::app::kAllSections.size());
    for(const auto section : shield::app::kAllSections) {
        if(IsSectionVisible(section, remote_catalog_state, installed_titles)) {
            sections.push_back(section);
        }
    }
    return sections;
}

std::string BuildStorageStatusLabel(const shield::i18n::I18n &translator, const shield::platform::StorageStatus &storage_status) {
    if(!storage_status.available) {
        return translator.Get("status.storageUnavailable");
    }

    return shield::platform::SystemStatus::FormatStorageAmount(storage_status.free_bytes);
}

float BuildStorageFillRatio(const shield::platform::StorageStatus &storage_status) {
    if(!storage_status.available || (storage_status.total_bytes == 0)) {
        return 0.0f;
    }

    const double used_ratio = 1.0 - (static_cast<double>(storage_status.free_bytes) / static_cast<double>(storage_status.total_bytes));
    return static_cast<float>(std::clamp(used_ratio, 0.0, 1.0));
}

int BuildNetworkLevel(const shield::platform::SystemOverview &system_overview) {
    using shield::platform::NetworkStatus;

    if(!system_overview.network_available || (system_overview.network_status == NetworkStatus::Offline)) {
        return 0;
    }

    if(system_overview.network_status == NetworkStatus::Ethernet) {
        return 4;
    }

    return std::clamp(static_cast<int>(system_overview.wifi_strength), 1, 3);
}

std::string BuildNetworkLabel(const shield::i18n::I18n &translator, const shield::platform::SystemOverview &system_overview) {
    using shield::platform::NetworkStatus;

    if(system_overview.network_status == NetworkStatus::Ethernet) {
        return translator.Get("status.networkEthernet");
    }

    return translator.Get("status.networkWifi");
}

bool UseEthernetIcon(const shield::platform::SystemOverview &system_overview) {
    return system_overview.network_status == shield::platform::NetworkStatus::Ethernet;
}

std::string NormalizeSortMode(std::string sort_mode) {
    std::transform(sort_mode.begin(), sort_mode.end(), sort_mode.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });

    if((sort_mode == "recent") || (sort_mode == "recommended") || (sort_mode == "a-z") || (sort_mode == "z-a")
            || (sort_mode == "asc") || (sort_mode == "desc") || (sort_mode == "old")) {
        return sort_mode;
    }

    return "recent";
}

std::string NormalizeRecommendedSortMode(std::string sort_mode) {
    std::transform(sort_mode.begin(), sort_mode.end(), sort_mode.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });

    if((sort_mode == "name") || (sort_mode == "critic_score") || (sort_mode == "review_count")) {
        return sort_mode;
    }

    return "critic_score";
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });

    return value;
}

std::string TrimWhitespace(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if(first == std::string::npos) {
        return "";
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string NormalizeSearchQuery(const std::string &query) {
    return ToLowerAscii(TrimWhitespace(query));
}

std::string Utf8Glyph(const char8_t *glyph) {
    return std::string(reinterpret_cast<const char *>(glyph));
}

std::string GetButtonGlyph(const std::string &key) {
    if(key == "L") return Utf8Glyph(u8"\uE0A4");
    if(key == "R") return Utf8Glyph(u8"\uE0A5");
    if(key == "ZL") return Utf8Glyph(u8"\uE0A6");
    if(key == "ZR") return Utf8Glyph(u8"\uE0A7");
    if(key == "SL") return Utf8Glyph(u8"\uE0A8");
    if(key == "SR") return Utf8Glyph(u8"\uE0A9");
    if(key == "DLEFT") return Utf8Glyph(u8"\uE07B");
    if(key == "DUP") return Utf8Glyph(u8"\uE079");
    if(key == "DRIGHT") return Utf8Glyph(u8"\uE07C");
    if(key == "DDOWN") return Utf8Glyph(u8"\uE07A");
    if(key == "A") return Utf8Glyph(u8"\uE0A0");
    if(key == "B") return Utf8Glyph(u8"\uE0A1");
    if(key == "X") return Utf8Glyph(u8"\uE0A2");
    if(key == "Y") return Utf8Glyph(u8"\uE0A3");
    if(key == "LS") return Utf8Glyph(u8"\uE08A");
    if(key == "RS") return Utf8Glyph(u8"\uE08B");
    if(key == "MINUS") return Utf8Glyph(u8"\uE0B6");
    if(key == "PLUS") return Utf8Glyph(u8"\uE0B5");
    return key;
}

std::string BuildButtonHint(const std::string &key, const std::string &label) {
    if(label.empty()) {
        return "";
    }
    return GetButtonGlyph(key) + " " + label;
}

void AppendDetailLine(std::ostringstream &body, const std::string &label, const std::string &value) {
    if(value.empty()) {
        return;
    }

    if(body.tellp() > 0) {
        body << "\n";
    }

    body << label << ": " << value;
}

void AppendDetailBlock(std::ostringstream &body, const std::string &label, const std::string &value) {
    if(value.empty()) {
        return;
    }

    if(body.tellp() > 0) {
        body << "\n\n";
    }

    body << label << ":\n" << value;
}

std::string JoinList(const std::vector<std::string> &values, const std::size_t max_items = 5) {
    std::ostringstream stream;
    std::size_t count = 0;
    for(const auto &value : values) {
        if(value.empty()) {
            continue;
        }
        if(count > 0) {
            stream << ", ";
        }
        stream << value;
        count++;
        if(count >= max_items) {
            break;
        }
    }
    return stream.str();
}

std::string BuildScreenshotPreview(const std::vector<std::string> &screenshots, const std::size_t max_items = 3) {
    std::ostringstream stream;
    std::size_t count = 0;
    for(const auto &entry : screenshots) {
        if(entry.empty()) {
            continue;
        }

        if(count > 0) {
            stream << "\n";
        }
        stream << entry;
        count++;
        if(count >= max_items) {
            break;
        }
    }

    if(screenshots.size() > count) {
        if(count > 0) {
            stream << "\n";
        }
        stream << "+" << (screenshots.size() - count);
    }

    return stream.str();
}

bool MatchesSearchQuery(const std::string &search_query, const std::initializer_list<std::string> &candidates) {
    if(search_query.empty()) {
        return true;
    }

    for(const auto &candidate : candidates) {
        if(candidate.empty()) {
            continue;
        }

        if(ToLowerAscii(candidate).find(search_query) != std::string::npos) {
            return true;
        }
    }

    return false;
}

std::string FormatPlayerCount(const int number_of_players) {
    if(number_of_players <= 0) {
        return "";
    }

    return std::to_string(number_of_players);
}

std::string FormatRating(const int rating) {
    if(rating <= 0) {
        return "";
    }

    return std::to_string(rating);
}

std::string BuildSortModeLabel(const shield::i18n::I18n &translator, const AppConfig &config) {
    const std::string sort_mode = NormalizeSortMode(config.sort_mode);
    if(sort_mode == "recommended") return translator.Get("settings.sort.recommended");
    if(sort_mode == "a-z")         return translator.Get("settings.sort.az");
    if(sort_mode == "z-a")         return translator.Get("settings.sort.za");
    if(sort_mode == "asc")         return translator.Get("settings.sort.asc");
    if(sort_mode == "desc")        return translator.Get("settings.sort.desc");
    if(sort_mode == "old")         return translator.Get("settings.sort.old");
    return translator.Get("settings.sort.recent");
}

std::string BuildRecommendedSortModeLabel(const shield::i18n::I18n &translator, const AppConfig &config) {
    const std::string sort_mode = NormalizeRecommendedSortMode(config.recommended_sort_mode);
    if(sort_mode == "name") return translator.Get("settings.sort.recommendedName");
    if(sort_mode == "review_count") return translator.Get("settings.sort.recommendedReviewCount");
    return translator.Get("settings.sort.recommendedCriticScore");
}

std::string BuildModeSortAndSearchLabel(const shield::i18n::I18n &translator, const AppConfig &config, const bool, const std::string &search_query, const AppSection section) {
    std::string label = (section == AppSection::Recommended)
        ? BuildRecommendedSortModeLabel(translator, config)
        : BuildSortModeLabel(translator, config);
    if(!search_query.empty()) {
        label += " | " + translator.Get("common.searchLabel") + ": " + search_query;
    }
    return label;
}

std::string BuildQuickFilterLabel(const shield::i18n::I18n &translator, const AppSection section, const std::string &filter) {
    if(filter.empty() || (filter == "all")) {
        return translator.Get("filter.all");
    }

    if(section == AppSection::Installed) {
        if(filter == "sd") {
            return translator.Get("filter.storageSd");
        }
        if(filter == "nand") {
            return translator.Get("filter.storageNand");
        }
    }
    else if(section == AppSection::NewGames) {
        if(filter == "nsp") {
            return "NSP";
        }
        if(filter == "nsz") {
            return "NSZ";
        }
        if(filter == "xci") {
            return "XCI";
        }
        if(filter == "xcz") {
            return "XCZ";
        }
    }
    else if(section == AppSection::Updates) {
        if(filter == "queued") {
            return translator.Get("filter.queued");
        }
        if(filter == "ready") {
            return translator.Get("filter.ready");
        }
    }
    else if(section == AppSection::Dlcs) {
        if(filter == "queued") {
            return translator.Get("filter.queued");
        }
        if(filter == "ready") {
            return translator.Get("filter.ready");
        }
    }
    else if(section == AppSection::Queue) {
        if(filter == "active") {
            return translator.Get("filter.active");
        }
        if(filter == "paused") {
            return translator.Get("filter.paused");
        }
        if(filter == "failed") {
            return translator.Get("filter.failed");
        }
        if(filter == "done") {
            return translator.Get("filter.done");
        }
    }

    return filter;
}

std::string BuildHeaderModeSortSearchAndFilterLabel(const shield::i18n::I18n &translator, const AppConfig &config, const bool grid_view, const std::string &search_query, const AppSection section, const std::string &filter) {
    std::string label = BuildModeSortAndSearchLabel(translator, config, grid_view, search_query, section);
    const std::string filter_label = BuildQuickFilterLabel(translator, section, filter);
    if(!filter_label.empty() && (filter != "all")) {
        label += " | " + translator.Get("common.filterLabel") + ": " + filter_label;
    }
    return label;
}

std::string BuildSettingsLanguageValue(const shield::i18n::I18n &translator, const AppConfig &config) {
    if(config.language == "pt-BR") {
        return translator.Get("settings.language.pt");
    }
    if(config.language == "en-US") {
        return translator.Get("settings.language.en");
    }
    if(config.language == "es-ES") {
        return translator.Get("settings.language.es");
    }

    return translator.Get("settings.language.auto");
}

std::string BuildSettingsThemeValue(const shield::i18n::I18n &translator, const AppConfig &config) {
    if(config.theme == "light") return translator.Get("settings.theme.light");
    if(config.theme == "dark")  return translator.Get("settings.theme.dark");
    return translator.Get("settings.theme.auto");
}

std::string BuildSettingsInstallationModelValue(const shield::i18n::I18n &translator, const AppConfig &config) {
    if(config.installation_model == "stream") {
        return translator.Get("settings.installationModel.stream");
    }

    return translator.Get("settings.installationModel.direct");
}

std::string BuildSettingsConnectionValue(const shield::i18n::I18n &translator, const AppConfig &config, const RemoteCatalogState &remote_catalog_state) {
    const std::string catalog_name = !config.catalog_title.empty()
        ? config.catalog_title
        : (!remote_catalog_state.source_title.empty() ? remote_catalog_state.source_title : BuildHostLabel(config.catalog_url));
    if(!catalog_name.empty()) {
        return catalog_name;
    }

    return translator.Get("cards.settings.4.footer");
}

std::string BuildSettingsUnsignedValue(const shield::i18n::I18n &translator, const AppConfig &config) {
    return config.allow_unsigned_sources
        ? translator.Get("settings.common.enabled")
        : translator.Get("settings.common.disabled");
}

int CompareReleaseDatesKnownFirst(const std::uint32_t left, const std::uint32_t right, const bool newest_first) {
    const bool left_known = left > 0;
    const bool right_known = right > 0;
    if(left_known != right_known) {
        return left_known ? -1 : 1;
    }
    if(!left_known || (left == right)) {
        return 0;
    }

    if(newest_first) {
        return (left > right) ? -1 : 1;
    }

    return (left < right) ? -1 : 1;
}

std::string RemoteSortName(const RemoteTitleMetadata &title) {
    return title.name.empty() ? title.id : title.name;
}

int CompareRemoteNameThenId(const RemoteTitleMetadata &left, const RemoteTitleMetadata &right) {
    const std::string left_name = RemoteSortName(left);
    const std::string right_name = RemoteSortName(right);
    if(left_name != right_name) {
        return (left_name < right_name) ? -1 : 1;
    }
    if(left.id != right.id) {
        return (left.id < right.id) ? -1 : 1;
    }
    return 0;
}


std::vector<RemoteTitleMetadata> CollectSortedRemoteTitles(const RemoteCatalogState &remote_catalog_state, const AppConfig &config) {
    std::vector<RemoteTitleMetadata> titles;
    titles.reserve(remote_catalog_state.catalog.titles_by_id.size());

    for(const auto &[title_id, metadata] : remote_catalog_state.catalog.titles_by_id) {
        if(metadata.id.empty() && metadata.name.empty()) {
            continue;
        }

        RemoteTitleMetadata entry = metadata;
        if(entry.id.empty()) {
            entry.id = title_id;
        }
        titles.push_back(std::move(entry));
    }

    const std::string sort_mode = NormalizeSortMode(config.sort_mode);
    std::sort(titles.begin(), titles.end(), [sort_mode](const RemoteTitleMetadata &left, const RemoteTitleMetadata &right) {
        const int left_quality = (!left.name.empty() ? 4 : 0) + (!left.icon_url.empty() ? 2 : 0) + (left.release_date > 0 ? 1 : 0);
        const int right_quality = (!right.name.empty() ? 4 : 0) + (!right.icon_url.empty() ? 2 : 0) + (right.release_date > 0 ? 1 : 0);

        if(sort_mode == "recommended") {
            if(left_quality != right_quality) {
                return left_quality > right_quality;
            }
            if(left.rank != right.rank) {
                return left.rank > right.rank;
            }
            if(left.release_date != right.release_date) {
                return left.release_date > right.release_date;
            }
        }
        else if(sort_mode == "a-z") {
            const int name_compare = CompareRemoteNameThenId(left, right);
            if(name_compare != 0) {
                return name_compare < 0;
            }
        }
        else if(sort_mode == "z-a") {
            const int name_compare = CompareRemoteNameThenId(left, right);
            if(name_compare != 0) {
                return name_compare > 0;
            }
        }
        else if(sort_mode == "asc") {
            if(left.size != right.size) {
                return left.size < right.size;
            }
        }
        else if(sort_mode == "desc") {
            if(left.size != right.size) {
                return left.size > right.size;
            }
        }
        else if(sort_mode == "old") {
            const int date_compare = CompareReleaseDatesKnownFirst(left.release_date, right.release_date, false);
            if(date_compare != 0) {
                return date_compare < 0;
            }
        }
        else {
            const int date_compare = CompareReleaseDatesKnownFirst(left.release_date, right.release_date, true);
            if(date_compare != 0) {
                return date_compare < 0;
            }
            if(left.rank != right.rank) {
                return left.rank > right.rank;
            }
            if(left_quality != right_quality) {
                return left_quality > right_quality;
            }
        }

        if(left.name != right.name) {
            if(left.name.empty() != right.name.empty()) {
                return !left.name.empty();
            }
            return left.name < right.name;
        }

        return left.id < right.id;
    });

    return titles;
}

std::string RecommendedSortName(const RemoteTitleMetadata &title) {
    return ToLowerAscii(title.name.empty() ? title.id : title.name);
}

bool HasCriticScore(const RemoteTitleMetadata &title) {
    return title.critic_score >= 0.0;
}

bool HasReviewCount(const RemoteTitleMetadata &title) {
    return title.review_count >= 0;
}

int CompareRecommendedNameThenId(const RemoteTitleMetadata &left, const RemoteTitleMetadata &right) {
    const std::string left_name = RecommendedSortName(left);
    const std::string right_name = RecommendedSortName(right);
    if(left_name != right_name) {
        return (left_name < right_name) ? -1 : 1;
    }
    if(left.id != right.id) {
        return (left.id < right.id) ? -1 : 1;
    }
    return 0;
}

std::vector<RemoteTitleMetadata> CollectSortedRecommendedTitles(const RemoteCatalogState &remote_catalog_state, const AppConfig &config, const std::unordered_set<std::string> &installed_ids) {
    std::vector<RemoteTitleMetadata> titles = remote_catalog_state.catalog.recommended_titles;
    titles.erase(std::remove_if(titles.begin(), titles.end(), [&installed_ids](const RemoteTitleMetadata &title) {
        return IsRemoteTitleInstalled(title, installed_ids);
    }), titles.end());

    const std::string sort_mode = NormalizeRecommendedSortMode(config.recommended_sort_mode);

    std::sort(titles.begin(), titles.end(), [sort_mode](const RemoteTitleMetadata &left, const RemoteTitleMetadata &right) {
        if(sort_mode == "name") {
            const int name_compare = CompareRecommendedNameThenId(left, right);
            if(name_compare != 0) return name_compare < 0;
            if(HasCriticScore(left) != HasCriticScore(right)) return HasCriticScore(left);
            if(left.critic_score != right.critic_score) return left.critic_score > right.critic_score;
            if(HasReviewCount(left) != HasReviewCount(right)) return HasReviewCount(left);
            if(left.review_count != right.review_count) return left.review_count > right.review_count;
            return left.id < right.id;
        }

        if(sort_mode == "review_count") {
            if(HasReviewCount(left) != HasReviewCount(right)) return HasReviewCount(left);
            if(left.review_count != right.review_count) return left.review_count > right.review_count;
            if(HasCriticScore(left) != HasCriticScore(right)) return HasCriticScore(left);
            if(left.critic_score != right.critic_score) return left.critic_score > right.critic_score;
            const int name_compare = CompareRecommendedNameThenId(left, right);
            if(name_compare != 0) return name_compare < 0;
            return left.id < right.id;
        }

        if(HasCriticScore(left) != HasCriticScore(right)) return HasCriticScore(left);
        if(left.critic_score != right.critic_score) return left.critic_score > right.critic_score;
        if(HasReviewCount(left) != HasReviewCount(right)) return HasReviewCount(left);
        if(left.review_count != right.review_count) return left.review_count > right.review_count;
        const int name_compare = CompareRecommendedNameThenId(left, right);
        if(name_compare != 0) return name_compare < 0;
        return left.id < right.id;
    });

    return titles;
}

std::unordered_set<std::string> BuildInstalledTitleIdSet(const std::vector<InstalledTitle> &installed_titles) {
    std::unordered_set<std::string> installed_ids;
    installed_ids.reserve(installed_titles.size());

    for(const auto &title : installed_titles) {
        if(!title.title_id_hex.empty()) {
            installed_ids.insert(NormalizeTitleIdForCompare(title.title_id_hex));
        }
    }

    return installed_ids;
}

std::unordered_set<std::string> BuildInstalledDlcIdSet(const std::vector<InstalledTitle> &installed_titles) {
    std::unordered_set<std::string> installed_dlc_ids;
    installed_dlc_ids.reserve(installed_titles.size());

    for(const auto &title : installed_titles) {
        for(const auto &dlc_id : title.add_on_title_ids) {
            if(!dlc_id.empty()) {
                installed_dlc_ids.insert(NormalizeTitleIdForCompare(dlc_id));
            }
        }
    }

    return installed_dlc_ids;
}

std::vector<InstalledTitle> CollectSortedInstalledTitles(const std::vector<InstalledTitle> &installed_titles, const AppConfig &config) {
    std::vector<InstalledTitle> sorted_titles = installed_titles;
    const std::string sort_mode = NormalizeSortMode(config.sort_mode);

    std::sort(sorted_titles.begin(), sorted_titles.end(), [sort_mode](const InstalledTitle &left, const InstalledTitle &right) {
        if(sort_mode == "a-z") return ToLowerAscii(left.name) < ToLowerAscii(right.name);
        if(sort_mode == "z-a") return ToLowerAscii(left.name) > ToLowerAscii(right.name);
        if(sort_mode == "asc" || sort_mode == "old") {
            if(left.content_version != right.content_version) return left.content_version < right.content_version;
            return ToLowerAscii(left.name) < ToLowerAscii(right.name);
        }
        if(sort_mode == "desc") {
            if(left.content_version != right.content_version) return left.content_version > right.content_version;
            return ToLowerAscii(left.name) < ToLowerAscii(right.name);
        }
        if(sort_mode == "recommended") {
            if(left.has_icon != right.has_icon) return left.has_icon > right.has_icon;
        }
        if(left.content_version != right.content_version) return left.content_version > right.content_version;
        return ToLowerAscii(left.name) < ToLowerAscii(right.name);
    });

    return sorted_titles;
}

std::vector<UpdateCandidate> CollectSortedUpdateCandidates(const std::vector<UpdateCandidate> &update_candidates, const AppConfig &config) {
    std::vector<UpdateCandidate> sorted_candidates = update_candidates;
    const std::string sort_mode = NormalizeSortMode(config.sort_mode);

    std::sort(sorted_candidates.begin(), sorted_candidates.end(), [sort_mode](const UpdateCandidate &left, const UpdateCandidate &right) {
        const std::string left_name = ToLowerAscii(left.remote.name.empty() ? left.installed.name : left.remote.name);
        const std::string right_name = ToLowerAscii(right.remote.name.empty() ? right.installed.name : right.remote.name);

        if(sort_mode == "a-z") return left_name < right_name;
        if(sort_mode == "z-a") return left_name > right_name;
        if(sort_mode == "asc" || sort_mode == "old") {
            const int date_compare = CompareReleaseDatesKnownFirst(left.remote.release_date, right.remote.release_date, false);
            if(date_compare != 0) return date_compare < 0;
            return left_name < right_name;
        }
        if(sort_mode == "desc") {
            const int date_compare = CompareReleaseDatesKnownFirst(left.remote.release_date, right.remote.release_date, true);
            if(date_compare != 0) return date_compare < 0;
            return left_name < right_name;
        }
        if(sort_mode == "recommended") {
            if(left.remote.rank != right.remote.rank) return left.remote.rank > right.remote.rank;
            if(left.remote.version != right.remote.version) return left.remote.version > right.remote.version;
            return left_name < right_name;
        }

        const int date_compare = CompareReleaseDatesKnownFirst(left.remote.release_date, right.remote.release_date, true);
        if(date_compare != 0) {
            return date_compare < 0;
        }
        if(left.remote.version != right.remote.version) {
            return left.remote.version > right.remote.version;
        }
        return left_name < right_name;
    });

    return sorted_candidates;
}

bool MatchesInstalledQuickFilter(const InstalledTitle &title, const std::string &filter) {
    if(filter.empty() || (filter == "all")) {
        return true;
    }
    if(filter == "sd") {
        return title.storage_id == NcmStorageId_SdCard;
    }
    if(filter == "nand") {
        return (title.storage_id == NcmStorageId_BuiltInUser) || (title.storage_id == NcmStorageId_BuiltInSystem);
    }
    return true;
}

bool MatchesNewGamesQuickFilter(const RemoteTitleMetadata &title, const std::string &filter) {
    if(filter.empty() || (filter == "all")) {
        return true;
    }
    return ToLowerAscii(title.package_format) == filter;
}

bool MatchesDlcQuickFilter(const RemoteTitleMetadata &title, const std::vector<shield::catalog::QueueItem> &queue_items, const std::string &filter) {
    if(filter.empty() || (filter == "all")) {
        return true;
    }

    const auto queued = std::find_if(queue_items.begin(), queue_items.end(), [&title](const shield::catalog::QueueItem &item) {
        return item.title_id == title.id;
    }) != queue_items.end();

    if(filter == "queued") {
        return queued;
    }
    if(filter == "ready") {
        return !queued;
    }
    return true;
}

bool MatchesUpdateQuickFilter(const UpdateCandidate &candidate, const std::vector<shield::catalog::QueueItem> &queue_items, const std::string &filter) {
    if(filter.empty() || (filter == "all")) {
        return true;
    }

    const auto queued = std::find_if(queue_items.begin(), queue_items.end(), [&candidate](const shield::catalog::QueueItem &item) {
        return item.title_id == candidate.remote.id;
    }) != queue_items.end();

    if(filter == "queued") {
        return queued;
    }
    if(filter == "ready") {
        return !queued;
    }
    return true;
}

std::string BuildRemoteSubtitle(const shield::i18n::I18n &translator, const RemoteTitleMetadata &title);
std::string BuildRemoteFooter(const shield::i18n::I18n &translator, const RemoteTitleMetadata &title);

std::string FormatCriticScore(const double critic_score) {
    if(critic_score < 0.0) {
        return "";
    }

    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.1f", critic_score);
    return std::string(buffer);
}

ShellCardContent BuildRemoteCard(const shield::i18n::I18n &translator, const RemoteTitleMetadata &title, const pu::ui::Color &color, const bool dlc_badge) {
    return {
        title.package_format.empty()
            ? (dlc_badge ? translator.Get("catalog.badgeDlc") : (title.region.empty() ? translator.Get("catalog.badgeRemote") : title.region))
            : title.package_format,
        (dlc_badge && !title.file_name.empty()) ? title.file_name : (title.name.empty() ? title.id : title.name),
        BuildRemoteSubtitle(translator, title),
        BuildRemoteFooter(translator, title),
        color,
        title.local_icon_path
    };
}

ShellCardContent BuildRecommendedCard(const shield::i18n::I18n &translator, const RemoteTitleMetadata &title, const pu::ui::Color &color) {
    std::ostringstream subtitle;
    if(!title.publisher.empty()) {
        subtitle << title.publisher;
    }
    else {
        subtitle << translator.Get("catalog.titleIdPrefix") << title.id;
    }

    if(!title.recommendation_source.empty()) {
        subtitle << " | " << title.recommendation_source;
    }

    std::ostringstream footer;
    const std::string score = FormatCriticScore(title.critic_score);
    if(!score.empty()) {
        footer << translator.Get("recommended.criticScore") << ": " << score;
    }
    if(title.review_count >= 0) {
        if(footer.tellp() > 0) footer << " | ";
        footer << translator.Get("recommended.reviewCount") << ": " << title.review_count;
    }
    const std::string remote_footer = ((title.package_format.empty() && (title.version == 0) && (title.size == 0))
        ? ""
        : BuildRemoteFooter(translator, title));
    if(!remote_footer.empty()) {
        if(footer.tellp() > 0) footer << " | ";
        footer << remote_footer;
    }

    return {
        title.recommendation_source.empty() ? translator.Get("catalog.badgeRecommended") : title.recommendation_source,
        title.name.empty() ? title.id : title.name,
        subtitle.str(),
        footer.str(),
        color,
        title.local_icon_path
    };
}

std::string BuildReleaseDateLabel(const shield::i18n::I18n &translator, const RemoteTitleMetadata &title) {
    if(title.release_date == 0) {
        return "";
    }

    const std::string raw = std::to_string(title.release_date);
    if(raw.size() != 8) {
        return translator.Get("catalog.releasePrefix") + raw;
    }

    return translator.Get("catalog.releasePrefix") + raw.substr(6, 2) + "/" + raw.substr(4, 2) + "/" + raw.substr(0, 4);
}

std::string BuildReleaseDateValue(const RemoteTitleMetadata &title) {
    if(title.release_date == 0) {
        return "";
    }

    const std::string raw = std::to_string(title.release_date);
    if(raw.size() != 8) {
        return raw;
    }

    return raw.substr(6, 2) + "/" + raw.substr(4, 2) + "/" + raw.substr(0, 4);
}

std::string BuildReleaseDateValue(const std::uint32_t release_date) {
    if(release_date == 0) return "";
    const std::string raw = std::to_string(release_date);
    if(raw.size() != 8) return raw;
    return raw.substr(6, 2) + "/" + raw.substr(4, 2) + "/" + raw.substr(0, 4);
}

std::string BuildQueueProgressLine(const shield::i18n::I18n &translator, const shield::catalog::QueueItem &item);

bool MatchesSearchQuery(const InstalledTitle &title, const std::string &search_query) {
    return MatchesSearchQuery(search_query, {
        title.name,
        title.author,
        title.developer,
        title.title_id_hex,
        title.display_version,
        JoinList(title.categories),
        JoinList(title.languages)
    });
}

bool MatchesSearchQuery(const RemoteTitleMetadata &title, const std::string &search_query) {
    return MatchesSearchQuery(search_query, {
        title.name,
        title.file_name,
        title.publisher,
        title.developer,
        title.id,
        title.base_title_id,
        title.package_format,
        title.recommendation_source,
        FormatCriticScore(title.critic_score),
        title.review_count >= 0 ? std::to_string(title.review_count) : "",
        JoinList(title.categories),
        JoinList(title.languages)
    });
}

bool MatchesSearchQuery(const UpdateCandidate &candidate, const std::string &search_query) {
    return MatchesSearchQuery(search_query, {
        candidate.remote.name,
        candidate.installed.name,
        candidate.remote.publisher,
        candidate.installed.author,
        candidate.remote.id,
        candidate.remote.base_title_id,
        candidate.installed.title_id_hex,
        candidate.remote.package_format
    });
}

std::string BuildInstalledInfoBody(const shield::i18n::I18n &translator, const InstalledTitle &title) {
    std::ostringstream body;
    AppendDetailLine(body, translator.Get("info.titleId"), title.title_id_hex);
    AppendDetailLine(body, translator.Get("info.storage"), GetStorageLabel(translator, title.storage_id));
    AppendDetailLine(body, translator.Get("info.version"), title.display_version.empty() ? translator.Get("installed.versionUnknown") : title.display_version);
    if(title.content_version > 0) {
        AppendDetailLine(body, translator.Get("info.metaVersion"), std::to_string(title.content_version));
    }
    AppendDetailLine(body, translator.Get("info.publisher"), title.author);
    AppendDetailLine(body, translator.Get("info.developer"), title.developer);
    AppendDetailLine(body, translator.Get("info.release"), BuildReleaseDateValue(title.release_date));
    if(title.size > 0) {
        AppendDetailLine(body, translator.Get("info.size"), shield::platform::SystemStatus::FormatStorageAmount(title.size));
    }
    AppendDetailLine(body, translator.Get("info.players"), FormatPlayerCount(title.number_of_players));
    AppendDetailLine(body, translator.Get("info.rating"), FormatRating(title.rating));
    AppendDetailLine(body, translator.Get("info.categories"), JoinList(title.categories));
    AppendDetailLine(body, translator.Get("info.languages"), JoinList(title.languages));
    AppendDetailLine(body, translator.Get("info.screenshots"), title.screenshots.empty() ? "" : std::to_string(title.screenshots.size()));
    AppendDetailBlock(body, translator.Get("info.intro"), title.intro);
    AppendDetailBlock(body, translator.Get("info.description"), title.description);
    AppendDetailBlock(body, translator.Get("info.screenshotUrls"), BuildScreenshotPreview(title.screenshots));

    return body.str();
}

std::string BuildRemoteInfoBody(const shield::i18n::I18n &translator, const RemoteTitleMetadata &title) {
    std::ostringstream body;
    AppendDetailLine(body, translator.Get("info.titleId"), title.id);
    AppendDetailLine(body, translator.Get("info.baseTitleId"), title.base_title_id);
    AppendDetailLine(body, translator.Get("info.packageFormat"), title.package_format);
    AppendDetailLine(body, translator.Get("info.publisher"), title.publisher);
    AppendDetailLine(body, translator.Get("info.developer"), title.developer);
    AppendDetailLine(body, translator.Get("info.release"), BuildReleaseDateValue(title));
    if(title.version > 0) {
        AppendDetailLine(body, translator.Get("info.version"), std::to_string(title.version));
    }
    if(title.size > 0) {
        AppendDetailLine(body, translator.Get("info.size"), shield::platform::SystemStatus::FormatStorageAmount(title.size));
    }
    AppendDetailLine(body, translator.Get("info.players"), FormatPlayerCount(title.number_of_players));
    AppendDetailLine(body, translator.Get("info.rating"), FormatRating(title.rating));
    AppendDetailLine(body, translator.Get("info.categories"), JoinList(title.categories));
    AppendDetailLine(body, translator.Get("info.languages"), JoinList(title.languages));
    AppendDetailLine(body, translator.Get("info.screenshots"), title.screenshots.empty() ? "" : std::to_string(title.screenshots.size()));
    AppendDetailBlock(body, translator.Get("info.intro"), title.intro);
    AppendDetailBlock(body, translator.Get("info.description"), title.description);
    AppendDetailBlock(body, translator.Get("info.screenshotUrls"), BuildScreenshotPreview(title.screenshots));

    return body.str();
}

std::string BuildRecommendedInfoBody(const shield::i18n::I18n &translator, const RemoteTitleMetadata &title) {
    std::ostringstream body;
    AppendDetailLine(body, translator.Get("info.titleId"), title.id);
    AppendDetailLine(body, translator.Get("info.recommendationSource"), title.recommendation_source);
    AppendDetailLine(body, translator.Get("info.criticScore"), FormatCriticScore(title.critic_score));
    if(title.review_count >= 0) {
        AppendDetailLine(body, translator.Get("info.reviewCount"), std::to_string(title.review_count));
    }
    AppendDetailLine(body, translator.Get("info.baseTitleId"), title.base_title_id);
    AppendDetailLine(body, translator.Get("info.packageFormat"), title.package_format);
    AppendDetailLine(body, translator.Get("info.publisher"), title.publisher);
    AppendDetailLine(body, translator.Get("info.developer"), title.developer);
    AppendDetailLine(body, translator.Get("info.release"), BuildReleaseDateValue(title));
    if(title.version > 0) {
        AppendDetailLine(body, translator.Get("info.version"), std::to_string(title.version));
    }
    if(title.size > 0) {
        AppendDetailLine(body, translator.Get("info.size"), shield::platform::SystemStatus::FormatStorageAmount(title.size));
    }
    AppendDetailLine(body, translator.Get("info.categories"), JoinList(title.categories));
    AppendDetailLine(body, translator.Get("info.languages"), JoinList(title.languages));
    AppendDetailBlock(body, translator.Get("info.intro"), title.intro);
    AppendDetailBlock(body, translator.Get("info.description"), title.description);
    return body.str();
}

std::string BuildUpdateInfoBody(const shield::i18n::I18n &translator, const UpdateCandidate &candidate) {
    std::ostringstream body;
    AppendDetailLine(body, translator.Get("info.titleId"), candidate.remote.id);
    AppendDetailLine(body, translator.Get("info.baseTitleId"), candidate.remote.base_title_id.empty() ? candidate.installed.title_id_hex : candidate.remote.base_title_id);
    AppendDetailLine(body, translator.Get("info.installedVersion"), candidate.installed.display_version.empty() ? translator.Get("installed.versionUnknown") : candidate.installed.display_version);
    AppendDetailLine(body, translator.Get("info.remoteVersion"), candidate.remote.version > 0 ? std::to_string(candidate.remote.version) : "");
    AppendDetailLine(body, translator.Get("info.packageFormat"), candidate.remote.package_format);
    AppendDetailLine(body, translator.Get("info.publisher"), candidate.remote.publisher.empty() ? candidate.installed.author : candidate.remote.publisher);
    AppendDetailLine(body, translator.Get("info.developer"), candidate.remote.developer.empty() ? candidate.installed.developer : candidate.remote.developer);
    AppendDetailLine(body, translator.Get("info.release"), BuildReleaseDateValue(candidate.remote));
    if(candidate.remote.size > 0) {
        AppendDetailLine(body, translator.Get("info.size"), shield::platform::SystemStatus::FormatStorageAmount(candidate.remote.size));
    }
    AppendDetailLine(body, translator.Get("info.categories"), JoinList(candidate.remote.categories));
    AppendDetailLine(body, translator.Get("info.languages"), JoinList(candidate.remote.languages));
    AppendDetailLine(body, translator.Get("info.screenshots"), candidate.remote.screenshots.empty() ? "" : std::to_string(candidate.remote.screenshots.size()));
    AppendDetailBlock(body, translator.Get("info.intro"), candidate.remote.intro);
    AppendDetailBlock(body, translator.Get("info.description"), candidate.remote.description);
    AppendDetailBlock(body, translator.Get("info.screenshotUrls"), BuildScreenshotPreview(candidate.remote.screenshots));

    return body.str();
}

std::string BuildQueueInfoBody(const shield::i18n::I18n &translator, const shield::catalog::QueueItem &item) {
    std::ostringstream body;
    AppendDetailLine(body, translator.Get("info.titleId"), item.title_id);
    AppendDetailLine(body, translator.Get("info.baseTitleId"), item.base_title_id);
    AppendDetailLine(body, translator.Get("info.packageFormat"), item.package_format);
    AppendDetailLine(body, translator.Get("info.location"), item.target_location);
    AppendDetailLine(body, translator.Get("info.progress"), BuildQueueProgressLine(translator, item));
    AppendDetailLine(body, translator.Get("info.retry"), std::to_string(item.retry_count) + "/" + std::to_string(item.retry_limit));
    AppendDetailLine(body, translator.Get("info.installationModel"), translator.Get(item.installation_model == "stream" ? "settings.installationModel.stream" : "settings.installationModel.direct"));
    if(item.installation_model != "stream") {
        AppendDetailLine(body, translator.Get("info.deleteAfter"), item.keep_download ? translator.Get("queue.option.keep") : translator.Get("queue.option.delete"));
    }
    AppendDetailLine(body, translator.Get("info.integrity"), item.verify_integrity ? translator.Get("settings.common.enabled") : translator.Get("settings.common.disabled"));
    AppendDetailLine(body, translator.Get("info.autoStart"), item.auto_start ? translator.Get("settings.common.enabled") : translator.Get("settings.common.disabled"));
    AppendDetailLine(body, translator.Get("info.convertStandardCrypto"), item.convert_standard_crypto ? translator.Get("settings.common.enabled") : translator.Get("settings.common.disabled"));
    AppendDetailLine(body, translator.Get("info.reinstallNcas"), item.reinstall_ncas ? translator.Get("settings.common.enabled") : translator.Get("settings.common.disabled"));
    AppendDetailLine(body, translator.Get("info.includeAllDlcs"), item.include_all_dlcs ? translator.Get("settings.common.enabled") : translator.Get("settings.common.disabled"));
    AppendDetailLine(body, translator.Get("info.includeLatestUpdate"), item.include_latest_update ? translator.Get("settings.common.enabled") : translator.Get("settings.common.disabled"));
    AppendDetailLine(body, translator.Get("info.localPath"), item.local_path);
    AppendDetailLine(body, translator.Get("info.lastError"), item.last_error);
    return body.str();
}

std::string BuildRemoteSubtitle(const shield::i18n::I18n &translator, const RemoteTitleMetadata &title) {
    if(!title.publisher.empty()) {
        return title.publisher;
    }

    const std::string release_label = BuildReleaseDateLabel(translator, title);
    if(!release_label.empty()) {
        return release_label;
    }

    return translator.Get("catalog.titleIdPrefix") + title.id;
}

std::string BuildRemoteFooter(const shield::i18n::I18n &translator, const RemoteTitleMetadata &title) {
    std::ostringstream footer;
    if(!title.package_format.empty()) {
        footer << title.package_format;
    }

    if(title.version > 0) {
        if(footer.tellp() > 0) {
            footer << " | ";
        }
        footer << translator.Get("catalog.versionPrefix") << title.version;
    }

    if(title.size > 0) {
        if(footer.tellp() > 0) {
            footer << " | ";
        }
        footer << shield::platform::SystemStatus::FormatStorageAmount(title.size);
    }

    if(footer.tellp() == 0) {
        footer << translator.Get("catalog.versionUnknown");
    }

    return footer.str();
}

std::string BuildUpdateSubtitle(const shield::i18n::I18n &translator, const UpdateCandidate &candidate) {
    if(!candidate.installed.display_version.empty()) {
        return translator.Get("installed.displayVersionPrefix") + candidate.installed.display_version;
    }

    return translator.Get("catalog.titleIdPrefix") + candidate.installed.title_id_hex;
}

std::string BuildUpdateFooter(const shield::i18n::I18n &translator, const UpdateCandidate &candidate) {
    // Compare against the effective installed version (max of patch and
    // base app).  Using per-type fields breaks when a patch is already
    // installed because NCM reports the base application version as 0,
    // which made every non-zero remote version look like a valid upgrade.
    const std::uint32_t installed_comparable = candidate.installed.content_version;

    std::ostringstream footer;
    footer << translator.Get("catalog.updateCurrentPrefix") << installed_comparable
           << " \xE2\x86\x92 "  // " → " (U+2192)
           << translator.Get("catalog.updateAvailablePrefix") << candidate.remote.version;

    if(!candidate.remote.package_format.empty()) {
        footer << " | " << candidate.remote.package_format;
    }

    if(candidate.remote.size > 0) {
        footer << " | " << shield::platform::SystemStatus::FormatStorageAmount(candidate.remote.size);
    }

    return footer.str();
}

std::string TrimForDisplay(const std::string &value, const std::size_t max_length) {
    if((value.size() <= max_length) || (max_length <= 3)) {
        return value;
    }

    return value.substr(0, max_length - 3) + "...";
}

std::string GetQueueContentTypeLabel(const shield::i18n::I18n &translator, const std::string &title_id) {
    if(title_id.size() < 16) return "";
    const std::string last3 = title_id.substr(13, 3);
    if(last3 == "000") return translator.Get("queue.type.base");
    if(last3 == "800") return translator.Get("queue.type.update");
    return translator.Get("queue.type.dlc");
}

std::string FormatEta(const double bytes_per_second, const std::uint64_t bytes_done, const std::uint64_t bytes_total) {
    if(bytes_per_second <= 0.0 || bytes_total <= bytes_done || bytes_total == 0) return "";
    const auto remaining = bytes_total - bytes_done;
    const auto eta_secs = static_cast<std::uint64_t>(static_cast<double>(remaining) / bytes_per_second);
    const auto mm = static_cast<unsigned>(eta_secs / 60);
    const auto ss = static_cast<unsigned>(eta_secs % 60);
    char buf[12];
    snprintf(buf, sizeof(buf), "%02u:%02u", mm % 100u, ss);
    return std::string(buf);
}

float BuildQueueProgressRatio(const shield::catalog::QueueItem &item) {
    if(item.bytes_total > 0) {
        const double ratio = static_cast<double>(item.bytes_done) / static_cast<double>(item.bytes_total);
        return static_cast<float>(std::clamp(ratio, 0.0, 1.0));
    }

    if(item.state == shield::catalog::QueueItemState::Completed) {
        return 1.0f;
    }

    return 0.0f;
}

std::string BuildQueueProgressLine(const shield::i18n::I18n &translator, const shield::catalog::QueueItem &item) {
    std::ostringstream line;
    const float ratio = BuildQueueProgressRatio(item);
    if(item.bytes_total > 0) {
        line << shield::platform::SystemStatus::FormatStorageAmount(item.bytes_done)
             << " / "
             << shield::platform::SystemStatus::FormatStorageAmount(item.bytes_total)
             << " ("
             << static_cast<int>(ratio * 100.0f)
             << "%)";
    }
    else if(item.bytes_done > 0) {
        line << shield::platform::SystemStatus::FormatStorageAmount(item.bytes_done);
    }
    else if(item.state == shield::catalog::QueueItemState::Completed) {
        line << "100%";
    }

    if(item.package_format.size() > 0) {
        if(line.tellp() > 0) {
            line << " | ";
        }
        line << item.package_format;
    }

    if(item.size > 0) {
        if(line.tellp() > 0) {
            line << " | ";
        }
        line << shield::platform::SystemStatus::FormatStorageAmount(item.size);
    }

    if(item.bytes_per_second > 0.0) {
        if(line.tellp() > 0) {
            line << " | ";
        }
        line << shield::platform::SystemStatus::FormatStorageAmount(static_cast<std::uint64_t>(item.bytes_per_second))
             << "/s";
    }

    if(line.tellp() == 0) {
        line << translator.Get("queue.footer.pending");
    }

    return line.str();
}

std::string BuildQueueStateBadge(const shield::i18n::I18n &translator, const shield::catalog::QueueItemState state) {
    using shield::catalog::QueueItemState;

    switch(state) {
        case QueueItemState::Downloading:
            return translator.Get("queue.badge.downloading");
        case QueueItemState::Paused:
            return translator.Get("queue.badge.paused");
        case QueueItemState::Installing:
            return translator.Get("queue.badge.installing");
        case QueueItemState::Completed:
            return translator.Get("queue.badge.completed");
        case QueueItemState::Canceled:
            return translator.Get("queue.badge.canceled");
        case QueueItemState::Failed:
            return translator.Get("queue.badge.failed");
        case QueueItemState::Queued:
        default:
            return translator.Get("queue.badge.queued");
    }
}

std::vector<ShellCardContent> BuildQueueCards(const shield::i18n::I18n &translator, const std::vector<shield::catalog::QueueItem> &queue_items) {
    if(queue_items.empty()) {
        return {};
    }

    const std::array<pu::ui::Color, 1> palette = {{ kCardAccentDark }};

    std::vector<ShellCardContent> cards;
    cards.reserve(queue_items.size());
    for(std::size_t index = 0; index < queue_items.size(); index++) {
        const auto &item = queue_items[index];

        std::ostringstream title_line;
        title_line << (item.name.empty() ? item.title_id : item.name);
        title_line << " | " << item.title_id;
        const std::string type_label = GetQueueContentTypeLabel(translator, item.title_id);
        if(!type_label.empty()) {
            title_line << " | " << type_label;
        }

        std::ostringstream subtitle_line;
        if(!item.package_format.empty()) {
            subtitle_line << item.package_format;
        }
        subtitle_line << " | " << item.target_location;
        subtitle_line << " | " << translator.Get(item.installation_model == "stream" ? "settings.installationModel.stream" : "settings.installationModel.direct");
        if(item.installation_model != "stream") {
            subtitle_line << " | " << (item.keep_download ? translator.Get("queue.option.keep") : translator.Get("queue.option.delete"));
        }

        const float ratio = BuildQueueProgressRatio(item);
        if(item.bytes_total > 0) {
            subtitle_line << " | "
                << shield::platform::SystemStatus::FormatStorageAmount(item.bytes_done)
                << " / "
                << shield::platform::SystemStatus::FormatStorageAmount(item.bytes_total)
                << " (" << static_cast<int>(ratio * 100.0f) << "%)";
        } else if(item.state == shield::catalog::QueueItemState::Completed) {
            subtitle_line << " | 100%";
        }

        if(item.bytes_per_second > 0.0) {
            subtitle_line << " | "
                << shield::platform::SystemStatus::FormatStorageAmount(static_cast<std::uint64_t>(item.bytes_per_second))
                << "/s";
        }

        const std::string eta = FormatEta(item.bytes_per_second, item.bytes_done, item.bytes_total);
        if(!eta.empty()) {
            subtitle_line << " | " << eta;
        }

        cards.push_back({
            BuildQueueStateBadge(translator, item.state),
            title_line.str(),
            subtitle_line.str(),
            "",
            palette[index % palette.size()]
        });
    }

    return cards;
}

std::vector<ShellCardContent> BuildInstalledCards(const shield::i18n::I18n &translator, const std::vector<InstalledTitle> &installed_titles) {
    if(installed_titles.empty()) {
        return {};
    }

    const std::array<pu::ui::Color, 1> palette = {{ kCardAccentDark }};

    std::vector<ShellCardContent> cards;
    cards.reserve(installed_titles.size());
    for(std::size_t index = 0; index < installed_titles.size(); index++) {
        const auto &title = installed_titles[index];
        std::ostringstream footer;
        if(!title.display_version.empty()) {
            footer << translator.Get("installed.displayVersionPrefix") << title.display_version;
        }
        else {
            footer << translator.Get("installed.versionUnknown");
        }

        if(title.content_version > 0) {
            footer << " | " << translator.Get("installed.metaVersionPrefix") << title.content_version;
        }

        cards.push_back({
            GetStorageLabel(translator, title.storage_id),
            title.name,
            title.author.empty() ? (translator.Get("installed.titleIdPrefix") + title.title_id_hex) : title.author,
            footer.str(),
            palette[index % palette.size()],
            {}
        });
    }

    return cards;
}

std::vector<ShellCardContent> BuildUpdateCards(const shield::i18n::I18n &translator, const std::vector<UpdateCandidate> &update_candidates) {
    const std::array<pu::ui::Color, 1> palette = {{ kCardAccentDark }};

    if(update_candidates.empty()) {
        return {};
    }

    std::vector<ShellCardContent> cards;
    cards.reserve(update_candidates.size());
    for(std::size_t index = 0; index < update_candidates.size(); index++) {
        const auto &candidate = update_candidates[index];
        cards.push_back({
            translator.Get("catalog.badgeUpdate"),
            candidate.remote.name.empty() ? candidate.installed.name : candidate.remote.name,
            BuildUpdateSubtitle(translator, candidate),
            BuildUpdateFooter(translator, candidate),
            palette[index % palette.size()],
            candidate.remote.local_icon_path
        });
    }

    return cards;
}

std::vector<ShellCardContent> BuildSettingsCards(const shield::i18n::I18n &translator, const AppConfig &config, const RemoteCatalogState &remote_catalog_state, bool forwarder_activated) {
    constexpr std::array<pu::ui::Color, 1> pal = {{ kCardAccentDark }};
    const std::string fwd_badge    = forwarder_activated ? translator.Get("cards.settings.forwarder.activated") : translator.Get("cards.settings.forwarder.badge");
    const std::string fwd_subtitle = forwarder_activated ? translator.Get("cards.settings.forwarder.activatedSubtitle") : translator.Get("cards.settings.forwarder.subtitle");
    return {
        { BuildSettingsLanguageValue(translator, config),                               translator.Get("cards.settings.1.title"), translator.Get("cards.settings.1.subtitle"), "", pal[0], "" },
        { BuildSettingsThemeValue(translator, config),                                  translator.Get("cards.settings.2.title"), translator.Get("cards.settings.2.subtitle"), "", pal[0], "" },
        { BuildSettingsInstallationModelValue(translator, config),                      translator.Get("cards.settings.installationModel.title"), translator.Get("cards.settings.installationModel.subtitle"), "", pal[0], "" },
        { BuildSettingsConnectionValue(translator, config, remote_catalog_state),       translator.Get("cards.settings.4.title"), translator.Get("cards.settings.4.subtitle"), "", pal[0], "" },
        { BuildSettingsUnsignedValue(translator, config),                               translator.Get("cards.settings.5.title"), translator.Get("cards.settings.5.subtitle"), "", pal[0], "" },
        { fwd_badge,                                                                    translator.Get("cards.settings.forwarder.title"), fwd_subtitle, "", pal[0], "" }
    };
}

}

ShellLayout::ShellLayout(const shield::app::AppConfig &config, const shield::i18n::I18n &translator, std::vector<InstalledTitle> installed_titles, std::vector<shield::catalog::QueueItem> queue_items, const RemoteCatalogState &remote_catalog_state, const shield::platform::SystemOverview &system_overview, const bool applet_mode)
    : Layout(), config_(config), translator_(translator), installed_titles_(std::move(installed_titles)), queue_items_(std::move(queue_items)), remote_catalog_state_(remote_catalog_state), system_overview_(system_overview), applet_mode_(applet_mode), selected_section_(AppSection::Installed) {
    this->SetBackgroundColor(ResolveThemePalette(this->config_).shell_background);
    this->quick_filters_.fill("all");
    this->BuildStaticLayout();
    this->RebuildCardCache();
    this->ApplyThemePalette();
    this->AddRenderCallback([this]() {
        this->RefreshClock();
    });

    // Keep navigation logic close to the layout so the app shell stays easy to reason about.
    this->SetOnInput([this](const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) {
        if(this->shutting_down_) return;
        if(this->startup_gate_active_) return;
        constexpr std::uint32_t kNavHoldDelay = 22;
        constexpr std::uint32_t kNavHoldRepeat = 5;
        const bool held_up   = (keys_held & HidNpadButton_Up)   != 0;
        const bool held_down = (keys_held & HidNpadButton_Down) != 0;
        if(held_up || held_down) {
            if((keys_down & HidNpadButton_Up) || (keys_down & HidNpadButton_Down)) {
                this->nav_hold_frames_ = 0;
            } else {
                ++this->nav_hold_frames_;
            }
        } else {
            this->nav_hold_frames_ = 0;
        }
        const bool do_nav = (this->nav_hold_frames_ == 0) ||
            (this->nav_hold_frames_ >= kNavHoldDelay &&
             ((this->nav_hold_frames_ - kNavHoldDelay) % kNavHoldRepeat == 0));

        if((this->selected_section_ == AppSection::Queue) && this->queue_detail_mode_) {
            if(do_nav && held_up) {
                this->MoveCardSelection(-1);
                this->RefreshQueueDetailPage();
                this->RefreshHeader();
                return;
            }
            if(do_nav && held_down) {
                this->MoveCardSelection(1);
                this->RefreshQueueDetailPage();
                this->RefreshHeader();
                return;
            }
            if(keys_down & HidNpadButton_A) {
                this->RunQueueControlAction(QueueControlAction::PauseResume);
                return;
            }
            if(keys_down & HidNpadButton_B) {
                this->RunQueueControlAction(QueueControlAction::PauseResumeAll);
                return;
            }
            if(keys_down & HidNpadButton_X) {
                this->RunQueueControlAction(QueueControlAction::Cancel);
                return;
            }
            if(keys_down & HidNpadButton_Y) {
                this->RunQueueControlAction(QueueControlAction::CancelAll);
                return;
            }
            if(keys_down & HidNpadButton_R) {
                this->RunQueueControlAction(QueueControlAction::ClearItem);
                return;
            }
            if(keys_down & HidNpadButton_ZR) {
                this->RunQueueControlAction(QueueControlAction::ClearAll);
                return;
            }
            if(keys_down & HidNpadButton_L) {
                this->RunQueueControlAction(QueueControlAction::MoveUp);
                return;
            }
            if(keys_down & HidNpadButton_ZL) {
                this->RunQueueControlAction(QueueControlAction::MoveDown);
                return;
            }
        }

        const bool queue_card_controls = (this->selected_section_ == AppSection::Queue)
            && (this->focus_zone_ == FocusZone::Cards)
            && !this->cached_queue_items_.empty();
        if(queue_card_controls) {
            if(keys_down & HidNpadButton_A) {
                this->RunQueueControlAction(QueueControlAction::PauseResume);
                return;
            }
            if(keys_down & HidNpadButton_B) {
                this->RunQueueControlAction(QueueControlAction::PauseResumeAll);
                return;
            }
            if(keys_down & HidNpadButton_X) {
                this->RunQueueControlAction(QueueControlAction::Cancel);
                return;
            }
            if(keys_down & HidNpadButton_Y) {
                this->RunQueueControlAction(QueueControlAction::CancelAll);
                return;
            }
            if(keys_down & HidNpadButton_R) {
                this->RunQueueControlAction(QueueControlAction::ClearItem);
                return;
            }
            if(keys_down & HidNpadButton_ZR) {
                this->RunQueueControlAction(QueueControlAction::ClearAll);
                return;
            }
            if(keys_down & HidNpadButton_L) {
                this->RunQueueControlAction(QueueControlAction::MoveUp);
                return;
            }
            if(keys_down & HidNpadButton_ZL) {
                this->RunQueueControlAction(QueueControlAction::MoveDown);
                return;
            }
        }

        if(do_nav && held_up) {
            if(this->focus_zone_ == FocusZone::Cards) {
                this->MoveCardSelection(this->IsGridModeEnabled() ? -this->GetGridStep() : -1);
            }
            else {
                this->MoveSelection(-1);
            }
        }
        else if(do_nav && held_down) {
            if(this->focus_zone_ == FocusZone::Cards) {
                this->MoveCardSelection(this->IsGridModeEnabled() ? this->GetGridStep() : 1);
            }
            else {
                this->MoveSelection(1);
            }
        }
        else if(keys_down & HidNpadButton_Left) {
            if((this->focus_zone_ == FocusZone::Cards) && this->IsGridModeEnabled() && ((this->selected_card_index_ % kGridColumns) > 0)) {
                this->MoveCardSelection(-1);
            }
            else {
                this->MoveFocusHorizontal(-1);
            }
        }
        else if(keys_down & HidNpadButton_Right) {
            const bool can_move_right = (this->focus_zone_ == FocusZone::Cards)
                && this->IsGridModeEnabled()
                && (((this->selected_card_index_ % kGridColumns) + 1) < kGridColumns)
                && ((this->selected_card_index_ + 1) < this->GetVisibleCardCount());
            if(can_move_right) {
                this->MoveCardSelection(1);
            }
            else if(this->focus_zone_ != FocusZone::Cards) {
                this->MoveFocusHorizontal(1);
            }
        }
        else if(keys_down & HidNpadButton_B) {
            if(this->focus_zone_ == FocusZone::Cards) {
                this->focus_zone_ = FocusZone::Sidebar;
                this->RefreshNavigation();
                this->RefreshCards();
                this->RefreshFooter();
            }
        }
        else if((keys_down & HidNpadButton_ZL) && (this->selected_section_ != AppSection::Queue)) {
            this->CycleSortMode();
        }
        else if(keys_down & HidNpadButton_R) {
            if((this->selected_section_ == AppSection::Recommended)
                || (this->selected_section_ == AppSection::NewGames)
                || (this->selected_section_ == AppSection::Updates)
                || (this->selected_section_ == AppSection::Dlcs)) {
                this->BeginSearch();
            }
        }
        else if(keys_down & HidNpadButton_Y) {
            if((this->selected_section_ != AppSection::Queue) && (this->selected_section_ != AppSection::Recommended)) {
                this->CycleQuickFilter();
            }
        }
        else if(keys_down & HidNpadButton_X) {
            if(((this->selected_section_ == AppSection::Updates) || (this->selected_section_ == AppSection::Dlcs))
                && (this->focus_zone_ == FocusZone::Cards)) {
                this->QueueAllVisibleItems();
            }
        }
        else if(keys_down & HidNpadButton_A) {
            this->ActivateSelection();
        }

        if(!touch_pos.IsEmpty()) {
            if(!this->touch_active_) {
                this->touch_active_ = true;
                this->touch_moved_ = false;
                this->touch_start_x_ = touch_pos.x;
                this->touch_start_y_ = touch_pos.y;
                this->touch_scroll_anchor_y_ = touch_pos.y;
            }
            else {
                int delta_x = static_cast<int>(touch_pos.x) - this->touch_start_x_;
                int delta_y = static_cast<int>(touch_pos.y) - this->touch_start_y_;
                if(delta_x < 0) {
                    delta_x = -delta_x;
                }
                if(delta_y < 0) {
                    delta_y = -delta_y;
                }

                if((delta_x > 12) || (delta_y > 12)) {
                    this->touch_moved_ = true;
                }

                const bool touch_over_cards = this->touch_start_x_ >= 420;
                if((this->focus_zone_ == FocusZone::Cards) || touch_over_cards) {
                    this->focus_zone_ = FocusZone::Cards;
                    const int scroll_delta = static_cast<int>(touch_pos.y) - this->touch_scroll_anchor_y_;
                    const int threshold = this->IsGridModeEnabled() ? 90 : 54;
                    if(scroll_delta >= threshold) {
                        this->MoveCardSelection(this->IsGridModeEnabled() ? -this->GetGridStep() : -1);
                        this->touch_scroll_anchor_y_ = touch_pos.y;
                        this->touch_moved_ = true;
                    }
                    else if(scroll_delta <= -threshold) {
                        this->MoveCardSelection(this->IsGridModeEnabled() ? this->GetGridStep() : 1);
                        this->touch_scroll_anchor_y_ = touch_pos.y;
                        this->touch_moved_ = true;
                    }
                }
            }
        }
        else if(this->touch_active_ && ((keys_up & pu::ui::TouchPseudoKey) || !this->touch_moved_)) {
            this->touch_active_ = false;
            this->touch_moved_ = false;
            this->HandleTouch(pu::ui::TouchPoint(this->touch_start_x_, this->touch_start_y_));
        }
    });

    this->RefreshAll();
}

void ShellLayout::SetDialogCallback(std::function<void(const std::string &, const std::string &)> callback) {
    this->dialog_callback_ = std::move(callback);
}

void ShellLayout::SetConfigChangedCallback(std::function<void(const shield::app::AppConfig &)> callback) {
    this->config_changed_callback_ = std::move(callback);
}

void ShellLayout::SetQueueActionCallback(std::function<void(const shield::catalog::QueueItem &)> callback) {
    this->queue_action_callback_ = std::move(callback);
}

void ShellLayout::SetQueueBulkActionCallback(std::function<void(const std::vector<shield::catalog::QueueItem> &)> callback) {
    this->queue_bulk_action_callback_ = std::move(callback);
}

void ShellLayout::SetQueueControlCallback(std::function<void(QueueControlAction, const std::string &)> callback) {
    this->queue_control_callback_ = std::move(callback);
}

void ShellLayout::SetForwarderInstallCallback(std::function<void()> callback) {
    this->forwarder_install_callback_ = std::move(callback);
}

void ShellLayout::SetShuttingDown(const bool val) {
    this->shutting_down_ = val;
    if(val) {
        this->ApplyStartupGate(true, this->translator_.Get("shutdown.title"), this->translator_.Get("shutdown.detail"), 0.0f, false);
    }
    else if(this->startup_gate_active_) {
        this->ApplyStartupGate(false, "", "", 0.0f);
    }
}

void ShellLayout::SetForwarderActivated(bool activated) {
    if(this->forwarder_activated_ == activated) {
        return;
    }
    this->forwarder_activated_ = activated;
    if(this->selected_section_ == shield::app::AppSection::Settings) {
        this->RebuildCardCache();
        this->RefreshCards();
    }
}

void ShellLayout::ApplyStartupGate(const bool active, const std::string &title, const std::string &detail, const float ratio, const bool show_progress) {
    this->startup_gate_active_ = active;
    this->startup_gate_title_ = title;
    this->startup_gate_detail_ = detail;
    this->startup_gate_ratio_ = ratio;
    this->startup_gate_show_progress_ = show_progress;
    this->RefreshStartupGate();
}

shield::app::AppSection ShellLayout::GetSelectedSection() const {
    return this->selected_section_;
}

void ShellLayout::BuildStaticLayout() {
    const auto palette = ResolveThemePalette(this->config_);
    this->sidebar_background_ = pu::ui::elm::Rectangle::New(0, 0, 360, 1080, palette.sidebar_background);
    this->sidebar_brand_background_ = pu::ui::elm::Rectangle::New(0, 0, 360, 112, palette.sidebar_brand_background);
    this->top_bar_ = pu::ui::elm::Rectangle::New(360, 0, 1560, 22, palette.top_bar_background);
    this->hero_background_ = pu::ui::elm::Rectangle::New(420, 190, 1440, 290, palette.hero_background, 34);
    this->hero_badge_background_ = pu::ui::elm::Rectangle::New(478, 236, 210, 54, palette.hero_badge_background, 18);
    this->status_card_bg_ = pu::ui::elm::Rectangle::New(420, 188, 1440, 72, pu::ui::Color{ 0, 0, 0, 0 }, 18);

    this->app_title_ = pu::ui::elm::TextBlock::New(36, 26, BuildAppDisplayName(this->translator_.Get("app.name")));
    this->app_subtitle_ = pu::ui::elm::TextBlock::New(36, 62, this->translator_.Get("app.tagline"));
    this->sidebar_status_label_ = pu::ui::elm::TextBlock::New(54, 755, "");
    this->header_title_ = pu::ui::elm::TextBlock::New(420, 64, "");
    this->header_subtitle_ = pu::ui::elm::TextBlock::New(420, 110, "");
    this->header_clock_ = pu::ui::elm::TextBlock::New(1832, 40, "");
    this->header_network_label_ = pu::ui::elm::TextBlock::New(1742, 40, "");
    this->hero_badge_ = pu::ui::elm::TextBlock::New(510, 246, "");
    this->hero_title_ = pu::ui::elm::TextBlock::New(478, 298, "");
    this->hero_subtitle_ = pu::ui::elm::TextBlock::New(478, 354, "");
    this->hero_body_ = pu::ui::elm::TextBlock::New(478, 410, "");
    this->task_progress_track_ = pu::ui::elm::Rectangle::New(420, 998, 1440, 12, palette.progress_track, 6);
    this->task_progress_fill_ = pu::ui::elm::Rectangle::New(420, 998, 0, 12, palette.progress_fill, 6);
    this->task_progress_title_ = pu::ui::elm::TextBlock::New(420, 968, "");
    this->task_progress_detail_ = pu::ui::elm::TextBlock::New(420, 1014, "");
    this->footer_hints_ = pu::ui::elm::TextBlock::New(420, 1044, "");
    this->queue_detail_panel_ = pu::ui::elm::Rectangle::New(420, 188, 1440, 684, palette.queue_panel, 34);
    this->queue_detail_badge_background_ = pu::ui::elm::Rectangle::New(466, 228, 178, 48, palette.queue_badge_background, 16);
    this->queue_detail_badge_ = pu::ui::elm::TextBlock::New(492, 236, "");
    this->queue_detail_artwork_background_ = pu::ui::elm::Rectangle::New(466, 300, 300, 300, palette.queue_artwork_background, 22);
    this->queue_detail_artwork_ = pu::ui::elm::Image::New(466, 300, pu::sdl2::TextureHandle::New(nullptr));
    this->queue_detail_title_ = pu::ui::elm::TextBlock::New(812, 228, "");
    this->queue_detail_subtitle_ = pu::ui::elm::TextBlock::New(812, 288, "");
    this->queue_detail_status_ = pu::ui::elm::TextBlock::New(812, 350, "");
    this->queue_detail_progress_track_ = pu::ui::elm::Rectangle::New(812, 430, 930, 14, kStatusTrack, 7);
    this->queue_detail_progress_fill_ = pu::ui::elm::Rectangle::New(812, 430, 0, 14, kStatusFill, 7);
    this->queue_detail_progress_ = pu::ui::elm::TextBlock::New(812, 458, "");
    this->queue_detail_primary_action_ = pu::ui::elm::TextBlock::New(812, 536, "");
    this->queue_detail_secondary_action_ = pu::ui::elm::TextBlock::New(812, 584, "");
    this->queue_detail_body_ = pu::ui::elm::TextBlock::New(466, 646, "");
    this->startup_gate_background_ = pu::ui::elm::Rectangle::New(360, 0, 1560, 1080, palette.shell_background);
    this->startup_gate_sidebar_background_ = pu::ui::elm::Rectangle::New(0, 112, 360, 968, palette.shell_background);
    this->startup_gate_title_text_ = pu::ui::elm::TextBlock::New(420, 430, "");
    this->startup_gate_detail_text_ = pu::ui::elm::TextBlock::New(420, 494, "");
    this->startup_gate_progress_track_ = pu::ui::elm::Rectangle::New(420, 574, 1080, 14, palette.progress_track, 7);
    this->startup_gate_progress_fill_ = pu::ui::elm::Rectangle::New(420, 574, 0, 14, palette.progress_fill, 7);

    this->app_title_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
    this->app_title_->SetColor(palette.app_brand_text);
    this->app_title_->SetClampWidth(286);
    this->app_title_->SetClampDelay(2400);
    this->app_subtitle_->SetColor(palette.app_brand_subtext);
    this->app_subtitle_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
    this->app_subtitle_->SetClampWidth(286);
    this->app_subtitle_->SetClampDelay(2400);
    this->sidebar_status_label_->SetColor(palette.subtle_text);
    this->sidebar_status_label_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Medium));
    this->sidebar_status_label_->SetClampWidth(286);
    this->sidebar_status_label_->SetClampDelay(0);
    this->header_title_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Medium));
    this->header_title_->SetColor(palette.hero_text);
    this->header_title_->SetClampWidth(820);
    this->header_title_->SetClampDelay(2200);
    this->header_subtitle_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
    this->header_subtitle_->SetColor(palette.subtle_text);
    this->header_subtitle_->SetClampWidth(980);
    this->header_subtitle_->SetClampDelay(2200);
    this->header_clock_->SetFont(pu::ui::MakeDefaultFontName(22));
    this->header_clock_->SetColor(palette.hero_text);
    this->header_network_label_->SetFont(pu::ui::MakeDefaultFontName(22));
    this->header_network_label_->SetColor(palette.hero_text);
    this->header_network_label_->SetClampWidth(72);
    this->header_network_label_->SetClampDelay(0);
    this->hero_badge_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
    this->hero_badge_->SetColor(palette.hero_text);
    this->hero_title_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Medium));
    this->hero_title_->SetColor(palette.hero_text);
    this->hero_title_->SetClampWidth(1020);
    this->hero_title_->SetClampDelay(2200);
    this->hero_subtitle_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
    this->hero_subtitle_->SetColor(palette.hero_subtext);
    this->hero_subtitle_->SetClampWidth(1020);
    this->hero_subtitle_->SetClampDelay(2200);
    this->hero_body_->SetFont(pu::ui::MakeDefaultFontName(22));
    this->hero_body_->SetColor(palette.hero_subtext);
    this->hero_body_->SetClampWidth(1100);
    this->hero_body_->SetClampDelay(2200);
    this->task_progress_title_->SetFont(pu::ui::MakeDefaultFontName(22));
    this->task_progress_title_->SetColor(palette.normal_text);
    this->task_progress_title_->SetClampWidth(1440);
    this->task_progress_title_->SetClampDelay(2200);
    this->task_progress_detail_->SetFont(pu::ui::MakeDefaultFontName(22));
    this->task_progress_detail_->SetColor(palette.subtle_text);
    this->task_progress_detail_->SetClampWidth(1440);
    this->task_progress_detail_->SetClampDelay(2200);
    this->footer_hints_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
    this->footer_hints_->SetColor((NormalizeThemeMode(this->config_.theme) == "light") ? palette.normal_text : palette.hero_text);
    this->queue_detail_badge_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
    this->queue_detail_badge_->SetColor(palette.hero_text);
    this->queue_detail_title_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Medium));
    this->queue_detail_title_->SetColor(palette.hero_text);
    this->queue_detail_title_->SetClampWidth(930);
    this->queue_detail_title_->SetClampDelay(0);
    this->queue_detail_subtitle_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
    this->queue_detail_subtitle_->SetColor(palette.normal_text);
    this->queue_detail_subtitle_->SetClampWidth(930);
    this->queue_detail_subtitle_->SetClampDelay(0);
    this->queue_detail_status_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
    this->queue_detail_status_->SetColor(palette.hero_text);
    this->queue_detail_status_->SetClampWidth(930);
    this->queue_detail_status_->SetClampDelay(0);
    this->queue_detail_progress_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
    this->queue_detail_progress_->SetColor(palette.normal_text);
    this->queue_detail_progress_->SetClampWidth(930);
    this->queue_detail_progress_->SetClampDelay(0);
    this->queue_detail_primary_action_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
    this->queue_detail_primary_action_->SetColor(palette.hero_text);
    this->queue_detail_primary_action_->SetClampWidth(930);
    this->queue_detail_primary_action_->SetClampDelay(0);
    this->queue_detail_secondary_action_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
    this->queue_detail_secondary_action_->SetColor(palette.subtle_text);
    this->queue_detail_secondary_action_->SetClampWidth(930);
    this->queue_detail_secondary_action_->SetClampDelay(0);
    this->queue_detail_body_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
    this->queue_detail_body_->SetColor(palette.normal_text);
    this->queue_detail_body_->SetClampWidth(1330);
    this->queue_detail_body_->SetClampDelay(0);
    this->queue_detail_artwork_->SetWidth(300);
    this->queue_detail_artwork_->SetHeight(300);
    this->startup_gate_title_text_->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Medium));
    this->startup_gate_title_text_->SetColor(palette.hero_text);
    this->startup_gate_title_text_->SetClampWidth(1080);
    this->startup_gate_title_text_->SetClampDelay(0);
    this->startup_gate_detail_text_->SetFont(pu::ui::MakeDefaultFontName(22));
    this->startup_gate_detail_text_->SetColor(palette.hero_subtext);
    this->startup_gate_detail_text_->SetClampWidth(1080);
    this->startup_gate_detail_text_->SetClampDelay(2200);

    this->Add(this->sidebar_background_);
    this->Add(this->sidebar_brand_background_);
    this->Add(this->top_bar_);
    this->Add(this->hero_background_);
    this->Add(this->hero_badge_background_);
    this->Add(this->status_card_bg_);
    this->Add(this->app_title_);
    this->Add(this->app_subtitle_);
    this->Add(this->sidebar_status_label_);
    this->Add(this->header_title_);
    this->Add(this->header_subtitle_);
    this->Add(this->header_clock_);
    this->Add(this->header_network_label_);
    this->Add(this->hero_badge_);
    this->Add(this->hero_title_);
    this->Add(this->hero_subtitle_);
    this->Add(this->hero_body_);
    this->Add(this->task_progress_track_);
    this->Add(this->task_progress_fill_);
    this->Add(this->task_progress_title_);
    this->Add(this->task_progress_detail_);
    this->Add(this->footer_hints_);
    this->Add(this->queue_detail_panel_);
    this->Add(this->queue_detail_badge_background_);
    this->Add(this->queue_detail_badge_);
    this->Add(this->queue_detail_artwork_background_);
    this->Add(this->queue_detail_artwork_);
    this->Add(this->queue_detail_title_);
    this->Add(this->queue_detail_subtitle_);
    this->Add(this->queue_detail_status_);
    this->Add(this->queue_detail_progress_track_);
    this->Add(this->queue_detail_progress_fill_);
    this->Add(this->queue_detail_progress_);
    this->Add(this->queue_detail_primary_action_);
    this->Add(this->queue_detail_secondary_action_);
    this->Add(this->queue_detail_body_);
    this->hero_background_->SetVisible(false);
    this->hero_badge_background_->SetVisible(false);
    this->status_card_bg_->SetVisible(false);
    this->hero_badge_->SetVisible(false);
    this->hero_title_->SetVisible(false);
    this->hero_subtitle_->SetVisible(false);
    this->hero_body_->SetVisible(false);
    this->task_progress_track_->SetVisible(false);
    this->task_progress_fill_->SetVisible(false);
    this->task_progress_title_->SetVisible(false);
    this->task_progress_detail_->SetVisible(false);
    this->queue_detail_panel_->SetVisible(false);
    this->queue_detail_badge_background_->SetVisible(false);
    this->queue_detail_badge_->SetVisible(false);
    this->queue_detail_artwork_background_->SetVisible(false);
    this->queue_detail_artwork_->SetVisible(false);
    this->queue_detail_status_->SetVisible(false);
    this->queue_detail_title_->SetVisible(false);
    this->queue_detail_subtitle_->SetVisible(false);
    this->queue_detail_progress_track_->SetVisible(false);
    this->queue_detail_progress_fill_->SetVisible(false);
    this->queue_detail_progress_->SetVisible(false);
    this->queue_detail_primary_action_->SetVisible(false);
    this->queue_detail_secondary_action_->SetVisible(false);
    this->queue_detail_body_->SetVisible(false);
    this->startup_gate_background_->SetVisible(false);
    this->startup_gate_sidebar_background_->SetVisible(false);
    this->startup_gate_title_text_->SetVisible(false);
    this->startup_gate_detail_text_->SetVisible(false);
    this->startup_gate_progress_track_->SetVisible(false);
    this->startup_gate_progress_fill_->SetVisible(false);

    // Combined label "<name> <free>" on a single line above a wider progress
    // bar. The separate `value` widget is kept (hidden) to preserve the
    // StorageWidgets contract; the combined text is rendered in `label`.
    constexpr s32 kStorageWidth = 230;
    constexpr s32 kStorageBarHeight = 9;
    auto make_storage_widgets = [this, palette, kStorageWidth, kStorageBarHeight](const s32 base_x, const std::string &label_key) {
        auto label = pu::ui::elm::TextBlock::New(base_x, 40, this->translator_.Get(label_key));
        auto track = pu::ui::elm::Rectangle::New(base_x, 72, kStorageWidth, kStorageBarHeight, palette.progress_track, 4);
        auto fill  = pu::ui::elm::Rectangle::New(base_x, 72, 0, kStorageBarHeight, palette.progress_fill, 4);
        auto value = pu::ui::elm::TextBlock::New(base_x, 40, "");

        label->SetFont(pu::ui::MakeDefaultFontName(22));
        label->SetColor(palette.hero_text);
        label->SetClampWidth(kStorageWidth);
        label->SetClampDelay(0);
        value->SetVisible(false);

        this->Add(label);
        this->Add(track);
        this->Add(fill);
        this->Add(value);

        return StorageWidgets{ label, track, fill, value, label_key };
    };

    this->nand_widgets_ = make_storage_widgets(1250, "status.nand");
    this->sd_widgets_   = make_storage_widgets(1494, "status.sdCard");

    this->network_dot_ = pu::ui::elm::Rectangle::New(1746, 84, 10, 10, palette.offline_status, 5);
    this->Add(this->network_dot_);

    for(std::size_t index = 0; index < this->network_bars_.size(); index++) {
        const s32 bar_height = 7 + static_cast<s32>(index) * 5;
        const s32 base_x = 1764 + static_cast<s32>(index) * 12;
        const s32 base_y = 95 - bar_height;
        auto bar = pu::ui::elm::Rectangle::New(base_x, base_y, 8, bar_height, palette.offline_status, 3);
        this->network_bars_[index] = bar;
        this->Add(bar);
    }

    // Simple wired-network glyph: short cable stem + plug head + two pins.
    this->ethernet_icon_segments_[0] = pu::ui::elm::Rectangle::New(1748, 79, 28, 8, palette.offline_status, 3);
    this->ethernet_icon_segments_[1] = pu::ui::elm::Rectangle::New(1774, 74, 16, 18, palette.offline_status, 3);
    this->ethernet_icon_segments_[2] = pu::ui::elm::Rectangle::New(1777, 69, 4, 8, palette.offline_status, 2);
    this->ethernet_icon_segments_[3] = pu::ui::elm::Rectangle::New(1783, 69, 4, 8, palette.offline_status, 2);
    this->ethernet_icon_segments_[4] = pu::ui::elm::Rectangle::New(1758, 87, 7, 7, palette.offline_status, 2);
    this->ethernet_icon_segments_[5] = pu::ui::elm::Rectangle::New(1767, 87, 7, 7, palette.offline_status, 2);
    for(auto &segment : this->ethernet_icon_segments_) {
        segment->SetVisible(false);
        this->Add(segment);
    }

    for(std::size_t index = 0; index < shield::app::kAllSections.size(); index++) {
        const auto item_y = 175 + static_cast<s32>(index) * 96;

        auto selection = pu::ui::elm::Rectangle::New(34, item_y, 292, 78, palette.sidebar_selected, 22);
        auto accent = pu::ui::elm::Rectangle::New(34, item_y + 8, 10, 62, kBrandRed, 5);
        auto label = pu::ui::elm::TextBlock::New(58, item_y + 22, "");

        label->SetFont(pu::ui::GetDefaultFont(pu::ui::DefaultFontSize::Small));
        label->SetClampWidth(250);
        label->SetClampDelay(2200);

        this->nav_widgets_[index] = { selection, accent, label };
        this->Add(selection);
        this->Add(accent);
        this->Add(label);
    }

    for(std::size_t index = 0; index < this->card_widgets_.size(); index++) {
        auto focus_outline    = pu::ui::elm::Rectangle::New(412, 182, kGridCardWidth + 16, kGridCardHeight + 16, palette.focus_outline, 30);
        auto surface          = pu::ui::elm::Rectangle::New(420, 190, kGridCardWidth, kGridCardHeight, palette.queue_panel, 26);
        auto badge_background = pu::ui::elm::Rectangle::New(628, 222, 170, 44, palette.card_badge_background, 15);
        auto badge            = pu::ui::elm::TextBlock::New(654, 230, "");
        auto title            = pu::ui::elm::TextBlock::New(446, 454, "");
        auto subtitle         = pu::ui::elm::TextBlock::New(446, 486, "");
        auto footer           = pu::ui::elm::TextBlock::New(446, 506, "");

        badge->SetFont(pu::ui::MakeDefaultFontName(22));
        badge->SetColor(kHeroText);
        title->SetFont(pu::ui::MakeDefaultFontName(22));
        title->SetColor(kHeroText);
        title->SetClampWidth(292);
        title->SetClampDelay(2200);
        subtitle->SetFont(pu::ui::MakeDefaultFontName(22));
        subtitle->SetColor(kHeroSubtext);
        subtitle->SetClampWidth(292);
        subtitle->SetClampDelay(2200);
        footer->SetFont(pu::ui::MakeDefaultFontName(22));
        footer->SetColor({ 245, 240, 205, 255 });
        footer->SetClampWidth(292);
        footer->SetClampDelay(2200);

        this->card_widgets_[index] = CardWidgets{ focus_outline, surface, badge_background, badge, title, subtitle, footer };
        this->Add(focus_outline);
        this->Add(surface);
        this->Add(badge_background);
        this->Add(badge);
        this->Add(title);
        this->Add(subtitle);
        this->Add(footer);
    }

    this->Add(this->startup_gate_background_);
    this->Add(this->startup_gate_sidebar_background_);
    this->Add(this->startup_gate_title_text_);
    this->Add(this->startup_gate_detail_text_);
    this->Add(this->startup_gate_progress_track_);
    this->Add(this->startup_gate_progress_fill_);
}

void ShellLayout::RefreshAll() {
    this->ApplyThemePalette();
    this->app_title_->SetText(BuildAppDisplayName(this->translator_.Get("app.name")));
    this->app_subtitle_->SetText(this->translator_.Get("app.tagline"));
    this->RefreshNavigation();
    this->RefreshHeader();
    this->RefreshSystemOverview();
    this->RefreshHero();
    this->RefreshCards();
    this->RefreshTaskProgress();
    this->RefreshFooter();
    this->RefreshClock();
    this->RefreshStartupGate();
}

void ShellLayout::ApplyThemePalette() {
    const auto palette = ResolveThemePalette(this->config_);

    this->SetBackgroundColor(palette.shell_background);
    this->sidebar_background_->SetColor(palette.sidebar_background);
    this->sidebar_brand_background_->SetColor(palette.sidebar_brand_background);
    this->top_bar_->SetColor(palette.top_bar_background);
    this->hero_background_->SetColor(palette.hero_background);
    this->hero_badge_background_->SetColor(palette.hero_badge_background);
    this->task_progress_track_->SetColor(palette.progress_track);
    this->task_progress_fill_->SetColor(palette.progress_fill);
    this->queue_detail_panel_->SetColor(palette.queue_panel);
    this->queue_detail_badge_background_->SetColor(palette.queue_badge_background);
    this->queue_detail_artwork_background_->SetColor(palette.queue_artwork_background);
    this->queue_detail_progress_track_->SetColor(palette.progress_track);
    this->queue_detail_progress_fill_->SetColor(palette.progress_fill);
    this->startup_gate_background_->SetColor(palette.shell_background);
    this->startup_gate_sidebar_background_->SetColor(palette.shell_background);
    this->startup_gate_progress_track_->SetColor(palette.progress_track);
    this->startup_gate_progress_fill_->SetColor(palette.progress_fill);

    this->app_title_->SetColor(palette.app_brand_text);
    this->app_subtitle_->SetColor(palette.app_brand_subtext);
    this->sidebar_status_label_->SetColor(palette.subtle_text);
    this->header_title_->SetColor(palette.hero_text);
    this->header_subtitle_->SetColor(palette.subtle_text);
    this->header_clock_->SetColor(palette.hero_text);
    this->header_network_label_->SetColor(palette.hero_text);
    this->hero_badge_->SetColor(palette.hero_text);
    this->hero_title_->SetColor(palette.hero_text);
    this->hero_subtitle_->SetColor(palette.hero_subtext);
    this->hero_body_->SetColor(palette.hero_subtext);
    this->task_progress_title_->SetColor(palette.normal_text);
    this->task_progress_detail_->SetColor(palette.subtle_text);
    this->footer_hints_->SetColor((NormalizeThemeMode(this->config_.theme) == "light") ? palette.normal_text : palette.hero_text);
    this->queue_detail_badge_->SetColor(palette.hero_text);
    this->queue_detail_title_->SetColor(palette.hero_text);
    this->queue_detail_subtitle_->SetColor(palette.normal_text);
    this->queue_detail_status_->SetColor(palette.hero_text);
    this->queue_detail_progress_->SetColor(palette.normal_text);
    this->queue_detail_primary_action_->SetColor(palette.hero_text);
    this->queue_detail_secondary_action_->SetColor(palette.subtle_text);
    this->queue_detail_body_->SetColor(palette.normal_text);
    this->startup_gate_title_text_->SetColor(palette.hero_text);
    this->startup_gate_detail_text_->SetColor(palette.hero_subtext);

    for(auto &widgets : this->nav_widgets_) {
        widgets.selection_background->SetColor(palette.sidebar_selected);
        widgets.accent_bar->SetColor(palette.sidebar_brand_background);
    }

    auto refresh_storage = [&](StorageWidgets &widgets) {
        widgets.label->SetColor(palette.hero_text);
        widgets.value->SetColor(palette.normal_text);
        widgets.track->SetColor(palette.progress_track);
    };

    refresh_storage(this->nand_widgets_);
    refresh_storage(this->sd_widgets_);

    this->network_dot_->SetColor(palette.offline_status);
    for(auto &segment : this->ethernet_icon_segments_) {
        segment->SetColor(palette.offline_status);
    }

    for(auto &widgets : this->card_widgets_) {
        widgets.focus_outline->SetColor(palette.focus_outline);
        const bool light_theme = NormalizeThemeMode(this->config_.theme) == "light";
        widgets.badge->SetColor(light_theme ? palette.normal_text : palette.hero_text);
        widgets.title->SetColor(light_theme ? palette.normal_text : palette.hero_text);
        widgets.subtitle->SetColor(light_theme ? palette.subtle_text : palette.hero_subtext);
        widgets.footer->SetColor(light_theme
            ? BlendColor(palette.normal_text, palette.subtle_text, 0.42f)
            : BlendColor(palette.hero_subtext, palette.hero_text, 0.18f));
    }
}

void ShellLayout::RefreshStartupGate() {
    const bool visible = this->startup_gate_active_;
    this->startup_gate_background_->SetVisible(visible);
    this->startup_gate_sidebar_background_->SetVisible(visible);
    this->startup_gate_title_text_->SetVisible(visible);
    this->startup_gate_detail_text_->SetVisible(visible);
    this->startup_gate_progress_track_->SetVisible(visible && this->startup_gate_show_progress_);
    this->startup_gate_progress_fill_->SetVisible(visible && this->startup_gate_show_progress_);
    if(!visible) {
        return;
    }

    const float ratio = std::clamp(this->startup_gate_ratio_, 0.0f, 1.0f);
    this->startup_gate_title_text_->SetText(this->startup_gate_title_);
    this->startup_gate_detail_text_->SetText(this->startup_gate_detail_);
    this->startup_gate_progress_fill_->SetWidth(static_cast<s32>(1080.0f * ratio));
}

void ShellLayout::RefreshNavigation() {
    const auto palette = ResolveThemePalette(this->config_);
    for(auto &widgets : this->nav_widgets_) {
        widgets.selection_background->SetVisible(false);
        widgets.accent_bar->SetVisible(false);
        widgets.label->SetVisible(false);
    }

    const auto visible_sections = BuildVisibleSections(this->remote_catalog_state_, this->installed_titles_);
    for(std::size_t visible_index = 0; visible_index < visible_sections.size(); visible_index++) {
        const auto section = visible_sections[visible_index];
        const auto index = shield::app::ToIndex(section);
        const bool is_selected = section == this->selected_section_;
        auto &widgets = this->nav_widgets_[index];
        const auto item_y = 175 + static_cast<s32>(visible_index) * 96;

        widgets.selection_background->SetY(item_y);
        widgets.accent_bar->SetY(item_y + 8);
        widgets.label->SetY(item_y + 22);
        widgets.selection_background->SetVisible(is_selected);
        widgets.accent_bar->SetVisible(is_selected);
        widgets.label->SetVisible(true);
        widgets.label->SetText(this->translator_.Get(GetNavigationKey(section)));
        widgets.label->SetColor(is_selected ? palette.selected_text : palette.normal_text);
    }

    this->sidebar_status_label_->SetVisible(this->applet_mode_);
    if(this->applet_mode_) {
        this->sidebar_status_label_->SetText(this->translator_.Get("status.appletMode"));
    }
}

bool ShellLayout::IsGridModeEnabled() const {
    return false;
}

void ShellLayout::RefreshHeader() {
    if((this->selected_section_ == AppSection::Queue) && this->queue_detail_mode_ && (this->selected_card_index_ < this->cached_queue_items_.size())) {
        const auto &item = this->cached_queue_items_[this->selected_card_index_];
        ApplyStableClampText(this->header_title_, this->translator_.Get("section.queue.detailTitle"), 820, 34, 42);
        ApplyStableClampText(this->header_subtitle_,
            (item.name.empty() ? item.title_id : item.name) + "  |  "
                + std::to_string(this->selected_card_index_ + 1) + "/"
                + std::to_string(this->cached_queue_items_.size()),
            980, 68, 82);
        return;
    }

    ApplyStableClampText(this->header_title_, this->translator_.Get(GetSectionTitleKey(this->selected_section_)), 820, 34, 42);
    const std::string active_filter = this->quick_filters_[shield::app::ToIndex(this->selected_section_)];
    const std::string mode_label = BuildHeaderModeSortSearchAndFilterLabel(this->translator_, this->config_, this->IsGridModeEnabled(), this->search_query_, this->selected_section_, active_filter);

    if(this->selected_section_ == AppSection::Installed) {
        ApplyStableClampText(this->header_subtitle_,
            BuildCountLabel(this->translator_, "installed.headerCountPrefix", this->cached_installed_titles_.size(), "installed.headerCountSuffix") + "  |  " + mode_label,
            980, 78, 92);
        return;
    }

    if((this->selected_section_ == AppSection::NewGames) && this->remote_catalog_state_.loaded) {
        ApplyStableClampText(this->header_subtitle_,
            BuildCountLabel(this->translator_, "catalog.headerRemotePrefix", this->cached_remote_titles_.size(), "catalog.headerRemoteSuffix") + "  |  " + mode_label,
            980, 78, 92);
        return;
    }

    if((this->selected_section_ == AppSection::Recommended) && this->remote_catalog_state_.loaded) {
        ApplyStableClampText(this->header_subtitle_,
            BuildCountLabel(this->translator_, "catalog.headerRecommendedPrefix", this->cached_remote_titles_.size(), "catalog.headerRecommendedSuffix") + "  |  " + mode_label,
            980, 78, 92);
        return;
    }

    if((this->selected_section_ == AppSection::Updates) && this->remote_catalog_state_.loaded) {
        ApplyStableClampText(this->header_subtitle_,
            BuildCountLabel(this->translator_, "catalog.headerUpdatePrefix", this->cached_update_candidates_.size(), "catalog.headerUpdateSuffix") + "  |  " + mode_label,
            980, 78, 92);
        return;
    }

    if((this->selected_section_ == AppSection::Dlcs) && this->remote_catalog_state_.loaded) {
        ApplyStableClampText(this->header_subtitle_,
            BuildCountLabel(this->translator_, "catalog.headerDlcPrefix", this->cached_remote_titles_.size(), "catalog.headerDlcSuffix") + "  |  " + mode_label,
            980, 78, 92);
        return;
    }

    if(this->selected_section_ == AppSection::Queue) {
        ApplyStableClampText(this->header_subtitle_,
            BuildCountLabel(this->translator_, "catalog.headerQueuePrefix", this->cached_queue_items_.size(), "catalog.headerQueueSuffix"),
            980, 78, 92);
        return;
    }

    ApplyStableClampText(this->header_subtitle_, this->translator_.Get(GetSectionSubtitleKey(this->selected_section_)) + "  |  " + mode_label, 980, 78, 92);
}

void ShellLayout::RefreshSystemOverview() {
    const auto palette = ResolveThemePalette(this->config_);
    const auto refresh_storage = [this, &palette](StorageWidgets &widgets, const shield::platform::StorageStatus &storage_status) {
        const std::string name_part  = this->translator_.Get(widgets.label_key);
        const std::string value_part = BuildStorageStatusLabel(this->translator_, storage_status);
        widgets.label->SetText(name_part + " " + value_part);

        const float fill_ratio = BuildStorageFillRatio(storage_status);
        const s32 track_width = widgets.track->GetWidth();
        widgets.fill->SetWidth(static_cast<s32>(track_width * fill_ratio));
        widgets.fill->SetColor(storage_status.available ? palette.progress_fill : palette.offline_status);
    };

    refresh_storage(this->nand_widgets_, this->system_overview_.nand);
    refresh_storage(this->sd_widgets_, this->system_overview_.sd_card);

    this->header_network_label_->SetText(BuildNetworkLabel(this->translator_, this->system_overview_));

    const int network_level = BuildNetworkLevel(this->system_overview_);
    const pu::ui::Color active_color = network_level > 0 ? palette.online_status : palette.offline_status;
    const bool ethernet_mode = UseEthernetIcon(this->system_overview_);

    this->network_dot_->SetVisible(!ethernet_mode);
    for(std::size_t index = 0; index < this->network_bars_.size(); index++) {
        const bool is_active = static_cast<int>(index + 1) <= network_level;
        this->network_bars_[index]->SetVisible(!ethernet_mode);
        this->network_bars_[index]->SetColor(is_active ? palette.online_status : palette.offline_status);
    }

    for(auto &segment : this->ethernet_icon_segments_) {
        segment->SetVisible(ethernet_mode);
        segment->SetColor(active_color);
    }

    if(!ethernet_mode) {
        this->network_dot_->SetColor(active_color);
    }
}

void ShellLayout::RefreshHero() {
    this->hero_background_->SetVisible(false);
    this->hero_badge_background_->SetVisible(false);
    this->hero_badge_->SetVisible(false);
    this->hero_title_->SetVisible(false);
    this->hero_subtitle_->SetVisible(false);
    this->hero_body_->SetVisible(false);
    this->status_card_bg_->SetVisible(false);

    const bool is_remote_section = (this->selected_section_ == AppSection::Recommended)
        || (this->selected_section_ == AppSection::NewGames)
        || (this->selected_section_ == AppSection::Updates)
        || (this->selected_section_ == AppSection::Dlcs);

    if(!is_remote_section || this->remote_catalog_state_.loaded
       || this->remote_catalog_state_.status_message.empty()) {
        return;
    }

    const auto palette = ResolveThemePalette(this->config_);

    // Status card mirrors the single accent colour used by list cards.
    const pu::ui::Color card_color = (NormalizeThemeMode(this->config_.theme) == "light")
        ? kCardAccentLight
        : kCardAccentDark;

    // Match the list-card geometry exactly (same constants as RefreshCards).
    constexpr s32 base_x = 420;
    constexpr s32 base_y = 188;
    constexpr s32 width  = 1440;
    constexpr s32 height = 72;

    this->status_card_bg_->SetX(base_x);
    this->status_card_bg_->SetY(base_y);
    this->status_card_bg_->SetWidth(width);
    this->status_card_bg_->SetHeight(height);
    this->status_card_bg_->SetColor(card_color);
    this->status_card_bg_->SetVisible(true);

    // Text sits at the title slot inside the card.
    this->hero_body_->SetX(base_x + 20);
    this->hero_body_->SetY(base_y + 7);
    this->hero_body_->SetColor(palette.hero_text);
    this->hero_body_->SetText(this->remote_catalog_state_.status_message);
    this->hero_body_->SetVisible(true);
}

void ShellLayout::InvalidateSectionCaches() {
    this->cached_remote_titles_.clear();
    this->cached_recommended_titles_.clear();
    this->cached_new_game_titles_.clear();
    this->cached_dlc_titles_.clear();
    this->cached_sorted_installed_titles_.clear();
    this->cached_sorted_update_candidates_.clear();
    this->cached_installed_titles_.clear();
    this->cached_update_candidates_.clear();
    this->cached_queue_items_.clear();
    this->remote_section_cache_valid_ = false;
    this->local_section_cache_valid_ = false;
}

void ShellLayout::EnsureRemoteSectionCaches() {
    if(this->remote_section_cache_valid_) {
        return;
    }

    this->cached_new_game_titles_.clear();
    this->cached_recommended_titles_.clear();
    this->cached_dlc_titles_.clear();

    if(this->remote_catalog_state_.loaded) {
        const auto all_titles = CollectSortedRemoteTitles(this->remote_catalog_state_, this->config_);
        const auto installed_ids = BuildInstalledTitleIdSet(this->installed_titles_);
        const auto installed_dlc_ids = BuildInstalledDlcIdSet(this->installed_titles_);
        this->cached_recommended_titles_ = CollectSortedRecommendedTitles(this->remote_catalog_state_, this->config_, installed_ids);

        this->cached_new_game_titles_.reserve(all_titles.size());
        this->cached_dlc_titles_.reserve(all_titles.size());

        for(const auto &title : all_titles) {
            const bool is_installed = IsRemoteTitleInstalled(title, installed_ids);

            if(title.content_type == RemoteContentType::BaseGame) {
                if(!is_installed) {
                    this->cached_new_game_titles_.push_back(title);
                }
                continue;
            }

            if(title.content_type == RemoteContentType::Dlc) {
                const std::string dlc_id = NormalizeTitleIdForCompare(title.id);
                if(is_installed && (dlc_id.empty() || (installed_dlc_ids.find(dlc_id) == installed_dlc_ids.end()))) {
                    this->cached_dlc_titles_.push_back(title);
                }
            }
        }
    }

    this->remote_section_cache_valid_ = true;
}

void ShellLayout::EnsureLocalSectionCaches() {
    if(this->local_section_cache_valid_) {
        return;
    }

    this->cached_sorted_installed_titles_ = CollectSortedInstalledTitles(this->installed_titles_, this->config_);
    this->cached_sorted_update_candidates_ = CollectSortedUpdateCandidates(this->remote_catalog_state_.update_candidates, this->config_);
    this->local_section_cache_valid_ = true;
}

void ShellLayout::RebuildCardCache() {
    this->cached_remote_titles_.clear();
    this->cached_installed_titles_.clear();
    this->cached_update_candidates_.clear();
    this->cached_queue_items_.clear();
    const std::string normalized_search = NormalizeSearchQuery(this->search_query_);
    const std::string active_filter = this->quick_filters_[shield::app::ToIndex(this->selected_section_)];

    if((this->selected_section_ == AppSection::Recommended) || (this->selected_section_ == AppSection::NewGames) || (this->selected_section_ == AppSection::Dlcs)) {
        this->EnsureRemoteSectionCaches();
        const auto &source_titles = (this->selected_section_ == AppSection::Recommended)
            ? this->cached_recommended_titles_
            : (this->selected_section_ == AppSection::NewGames)
            ? this->cached_new_game_titles_
            : this->cached_dlc_titles_;
        this->cached_remote_titles_.reserve(source_titles.size());
        for(const auto &title : source_titles) {
            if(!normalized_search.empty() && !MatchesSearchQuery(title, normalized_search)) {
                continue;
            }
            if((this->selected_section_ == AppSection::NewGames) && !MatchesNewGamesQuickFilter(title, active_filter)) {
                continue;
            }
            if((this->selected_section_ == AppSection::Dlcs) && !MatchesDlcQuickFilter(title, this->queue_items_, active_filter)) {
                continue;
            }
            this->cached_remote_titles_.push_back(title);
        }

        this->cached_cards_.clear();
    }
    else if(this->selected_section_ == AppSection::Queue) {
        (void)normalized_search;
        (void)active_filter;
        this->cached_queue_items_ = this->queue_items_;
        this->cached_cards_ = BuildQueueCards(this->translator_, this->cached_queue_items_);
    }
    else if(this->selected_section_ == AppSection::Installed) {
        this->EnsureLocalSectionCaches();
        this->cached_installed_titles_.reserve(this->cached_sorted_installed_titles_.size());
        for(const auto &title : this->cached_sorted_installed_titles_) {
            if(!normalized_search.empty() && !MatchesSearchQuery(title, normalized_search)) {
                continue;
            }
            if(!MatchesInstalledQuickFilter(title, active_filter)) {
                continue;
            }
            this->cached_installed_titles_.push_back(title);
        }
        this->cached_cards_ = BuildInstalledCards(this->translator_, this->cached_installed_titles_);
    }
    else if(this->selected_section_ == AppSection::Updates) {
        this->EnsureLocalSectionCaches();
        this->cached_update_candidates_.reserve(this->cached_sorted_update_candidates_.size());
        for(const auto &candidate : this->cached_sorted_update_candidates_) {
            if(!normalized_search.empty() && !MatchesSearchQuery(candidate, normalized_search)) {
                continue;
            }
            if(!MatchesUpdateQuickFilter(candidate, this->queue_items_, active_filter)) {
                continue;
            }
            this->cached_update_candidates_.push_back(candidate);
        }
        this->cached_cards_ = BuildUpdateCards(this->translator_, this->cached_update_candidates_);
    }
    else {
        this->cached_cards_ = BuildSettingsCards(this->translator_, this->config_, this->remote_catalog_state_, this->forwarder_activated_);
    }

    if(this->GetVisibleCardCount() == 0) {
        this->selected_card_index_ = 0;
        this->card_view_offset_ = 0;
        return;
    }

    if(this->selected_card_index_ >= this->GetVisibleCardCount()) {
        this->selected_card_index_ = this->GetVisibleCardCount() - 1;
    }

    this->EnsureCardSelectionVisible();
}

std::size_t ShellLayout::GetVisibleCardCount() const {
    if(!this->cached_remote_titles_.empty()) {
        return this->cached_remote_titles_.size();
    }

    return this->cached_cards_.size();
}

std::size_t ShellLayout::GetCardPageSlotCount() const {
    if(this->selected_section_ == AppSection::Queue) {
        return this->task_progress_.active ? 9 : 10;
    }
    return 10;
}

void ShellLayout::EnsureCardSelectionVisible() {
    const std::size_t card_count = this->GetVisibleCardCount();
    if(card_count == 0) {
        this->selected_card_index_ = 0;
        this->card_view_offset_ = 0;
        return;
    }

    const std::size_t page_size = this->GetCardPageSlotCount();
    if(this->IsGridModeEnabled()) {
        this->card_view_offset_ = (this->selected_card_index_ / page_size) * page_size;
    }
    else {
        if(this->selected_card_index_ < this->card_view_offset_) {
            this->card_view_offset_ = this->selected_card_index_;
        }
        else if(this->selected_card_index_ >= (this->card_view_offset_ + page_size)) {
            this->card_view_offset_ = this->selected_card_index_ - page_size + 1;
        }
    }

    if(this->card_view_offset_ >= card_count) {
        this->card_view_offset_ = card_count - 1;
    }
}

pu::sdl2::TextureHandle::Ref ShellLayout::LoadCardTexture(const std::string &path) {
    (void)path;
    return nullptr;
}

void ShellLayout::PruneImageTextureCache(const std::unordered_set<std::string> &keep_paths) {
    (void)keep_paths;
    this->image_texture_cache_.clear();
}

void ShellLayout::RefreshCards() {
    const auto palette = ResolveThemePalette(this->config_);
    const bool grid_mode = false;
    if((this->selected_section_ == AppSection::Queue) && this->queue_detail_mode_) {
        for(auto &widgets : this->card_widgets_) {
            widgets.focus_outline->SetVisible(false);
            widgets.surface->SetVisible(false);
            widgets.badge_background->SetVisible(false);
            widgets.badge->SetVisible(false);
            widgets.title->SetVisible(false);
            widgets.subtitle->SetVisible(false);
            widgets.footer->SetVisible(false);
        }

        this->RefreshQueueDetailPage();
        return;
    }

    this->queue_detail_panel_->SetVisible(false);
    this->queue_detail_badge_background_->SetVisible(false);
    this->queue_detail_badge_->SetVisible(false);
    this->queue_detail_artwork_background_->SetVisible(false);
    this->queue_detail_artwork_->SetVisible(false);
    this->queue_detail_title_->SetVisible(false);
    this->queue_detail_subtitle_->SetVisible(false);
    this->queue_detail_status_->SetVisible(false);
    this->queue_detail_progress_track_->SetVisible(false);
    this->queue_detail_progress_fill_->SetVisible(false);
    this->queue_detail_progress_->SetVisible(false);
    this->queue_detail_primary_action_->SetVisible(false);
    this->queue_detail_secondary_action_->SetVisible(false);
    this->queue_detail_body_->SetVisible(false);

    this->EnsureCardSelectionVisible();
    const ShellCardContent empty_card = { "", "", "", "", pu::ui::Color{ 80, 85, 95, 255 } };
    const std::array<pu::ui::Color, 1> new_games_palette = {{ kCardAccentDark }};
    const bool uses_remote_window = !this->cached_remote_titles_.empty();

    for(std::size_t index = 0; index < this->card_widgets_.size(); index++) {
        auto &widgets = this->card_widgets_[index];
        const bool slot_visible = index < this->GetCardPageSlotCount();
        const std::size_t global_index = this->card_view_offset_ + index;
        const bool has_card = slot_visible && (global_index < this->GetVisibleCardCount());
        ShellCardContent card = empty_card;
        if(has_card) {
            if(uses_remote_window) {
                const auto &remote_title = this->cached_remote_titles_[global_index];
                card = (this->selected_section_ == AppSection::Recommended)
                    ? BuildRecommendedCard(this->translator_, remote_title, new_games_palette[global_index % new_games_palette.size()])
                    : BuildRemoteCard(this->translator_, remote_title, new_games_palette[global_index % new_games_palette.size()], this->selected_section_ == AppSection::Dlcs);
            }
            else {
                card = this->cached_cards_[global_index];
            }
        }

        const bool hide_badge = false;
        const s32 base_x = 420;
        const s32 list_step = 80;
        const s32 base_y = 188 + static_cast<s32>(index) * list_step;
        const s32 width = 1440;
        const s32 height = 72;
        const s32 badge_width = 244;
        const s32 badge_x = base_x + width - badge_width - 18;
        const s32 badge_y = base_y + (height - 28) / 2;
        const s32 title_x = base_x + 20;
        const s32 subtitle_x = title_x;
        const s32 footer_x = title_x;
        const s32 title_y = base_y + 7;
        const s32 subtitle_y = base_y + 38;
        const s32 footer_y = base_y + 57;
        const s32 title_width = badge_x - title_x - 26;
        const bool is_card_focused = (this->focus_zone_ == FocusZone::Cards)
            && has_card
            && (global_index == this->selected_card_index_);

        std::string display_title = card.title;
        std::string display_subtitle = card.subtitle;
        std::string display_footer = card.footer;
        constexpr std::size_t title_limit = 96;
        constexpr std::size_t subtitle_limit = 128;
        constexpr std::size_t footer_limit = 0;
        if(!display_footer.empty()) {
            if(!display_subtitle.empty()) {
                display_subtitle += " | ";
            }
            display_subtitle += display_footer;
            display_footer.clear();
        }

        widgets.focus_outline->SetVisible(has_card && is_card_focused);
        widgets.surface->SetVisible(has_card);
        widgets.badge_background->SetVisible(has_card && !hide_badge && !card.badge.empty());
        widgets.badge->SetVisible(has_card && !hide_badge && !card.badge.empty());
        widgets.title->SetVisible(has_card);
        widgets.subtitle->SetVisible(has_card);
        widgets.footer->SetVisible(has_card && !display_footer.empty());

        if(!has_card) {
            if(index < this->card_dialog_titles_.size()) {
                this->card_dialog_titles_[index].clear();
                this->card_dialog_bodies_[index].clear();
            }
            continue;
        }

        widgets.focus_outline->SetX(base_x - 8);
        widgets.focus_outline->SetY(base_y - 8);
        widgets.focus_outline->SetWidth(width + 16);
        widgets.focus_outline->SetHeight(height + 16);

        widgets.surface->SetX(base_x);
        widgets.surface->SetY(base_y);
        widgets.surface->SetWidth(width);
        widgets.surface->SetHeight(height);
        pu::ui::Color surface_color = card.color;
        if((NormalizeThemeMode(this->config_.theme) == "light") && !grid_mode) {
            // Single flat card colour for light theme; focus only slightly darkens.
            surface_color = is_card_focused
                ? BlendColor(kCardAccentLight, palette.focus_outline, 0.10f)
                : kCardAccentLight;
        }
        else if(is_card_focused && !grid_mode) {
            surface_color = BlendColor(card.color, palette.focus_outline, 0.12f);
        }
        widgets.surface->SetColor(surface_color);

        widgets.badge_background->SetX(badge_x);
        widgets.badge_background->SetY(badge_y);
        widgets.badge_background->SetWidth(badge_width);
        if(NormalizeThemeMode(this->config_.theme) == "light") {
            // Uniform salmon badge across every section in the light theme.
            widgets.badge_background->SetColor(kCardBadgeLight);
        }
        else {
            widgets.badge_background->SetColor(palette.card_badge_background);
        }

        widgets.badge->SetX(badge_x + 20);
        widgets.badge->SetY(badge_y + 8);
        widgets.badge->SetClampWidth(badge_width - 30);
        widgets.badge->SetClampDelay(0);
        widgets.badge->SetText(card.badge);

        widgets.title->SetX(title_x);
        widgets.title->SetY(title_y);
        ApplyStableClampText(widgets.title, display_title, title_width, title_limit, title_limit + 10);

        widgets.subtitle->SetX(subtitle_x);
        widgets.subtitle->SetY(subtitle_y);
        ApplyStableClampText(widgets.subtitle, display_subtitle, title_width, subtitle_limit, subtitle_limit + 12);

        widgets.footer->SetX(footer_x);
        widgets.footer->SetY(footer_y);
        ApplyStableClampText(widgets.footer, display_footer, title_width, footer_limit, footer_limit + 12);

        if(index < this->card_dialog_titles_.size()) {
            this->card_dialog_titles_[index] = card.title;
            if(this->selected_section_ == AppSection::Installed) {
                this->card_dialog_bodies_[index] = (global_index < this->cached_installed_titles_.size())
                    ? BuildInstalledInfoBody(this->translator_, this->cached_installed_titles_[global_index])
                    : (card.subtitle + "\n" + card.footer);
            }
            else if(this->selected_section_ == AppSection::Updates) {
                this->card_dialog_bodies_[index] = (global_index < this->cached_update_candidates_.size())
                    ? BuildUpdateInfoBody(this->translator_, this->cached_update_candidates_[global_index])
                    : (card.subtitle + "\n" + card.footer);
            }
            else if(uses_remote_window) {
                this->card_dialog_bodies_[index] = (global_index < this->cached_remote_titles_.size())
                    ? ((this->selected_section_ == AppSection::Recommended)
                        ? BuildRecommendedInfoBody(this->translator_, this->cached_remote_titles_[global_index])
                        : BuildRemoteInfoBody(this->translator_, this->cached_remote_titles_[global_index]))
                    : (card.subtitle + "\n" + card.footer);
            }
            else if(this->selected_section_ == AppSection::Queue) {
                this->card_dialog_bodies_[index] = (global_index < this->cached_queue_items_.size())
                    ? BuildQueueInfoBody(this->translator_, this->cached_queue_items_[global_index])
                    : (card.subtitle + "\n" + card.footer);
            }
            else {
                this->card_dialog_bodies_[index] = card.subtitle + "\n" + card.footer;
            }
        }
    }
}

void ShellLayout::RefreshQueueDetailPage() {
    const bool has_item = (this->selected_section_ == AppSection::Queue)
        && this->queue_detail_mode_
        && (this->selected_card_index_ < this->cached_queue_items_.size());

    this->queue_detail_panel_->SetVisible(has_item);
    this->queue_detail_badge_background_->SetVisible(has_item);
    this->queue_detail_badge_->SetVisible(has_item);
    this->queue_detail_artwork_background_->SetVisible(has_item);
    this->queue_detail_title_->SetVisible(has_item);
    this->queue_detail_subtitle_->SetVisible(has_item);
    this->queue_detail_status_->SetVisible(has_item);
    this->queue_detail_progress_track_->SetVisible(has_item);
    this->queue_detail_progress_fill_->SetVisible(has_item);
    this->queue_detail_progress_->SetVisible(has_item);
    this->queue_detail_primary_action_->SetVisible(has_item);
    this->queue_detail_secondary_action_->SetVisible(has_item);
    this->queue_detail_body_->SetVisible(has_item);
    this->queue_detail_artwork_->SetVisible(false);
    this->queue_detail_artwork_background_->SetVisible(false);

    if(!has_item) {
        return;
    }

    const auto &item = this->cached_queue_items_[this->selected_card_index_];

    const float ratio = (item.bytes_total > 0)
        ? std::clamp(static_cast<float>(item.bytes_done) / static_cast<float>(item.bytes_total), 0.0f, 1.0f)
        : 0.0f;
    const auto displayed_state = item.state;
    this->queue_detail_badge_->SetText(BuildQueueStateBadge(this->translator_, displayed_state));
    this->queue_detail_title_->SetText(item.name.empty() ? item.title_id : item.name);
    this->queue_detail_subtitle_->SetText(item.subtitle.empty()
        ? (this->translator_.Get("catalog.titleIdPrefix") + item.title_id)
        : item.subtitle);
    this->queue_detail_status_->SetText(this->translator_.Get("queue.detail.status") + ": " + BuildQueueProgressLine(this->translator_, item));
    this->queue_detail_progress_fill_->SetWidth(static_cast<s32>(930.0f * ratio));
    this->queue_detail_progress_->SetText(this->translator_.Get("queue.detail.progress") + ": " + BuildQueueProgressLine(this->translator_, item));

    std::string primary_label = this->translator_.Get("queue.detail.primary.start");
    if(displayed_state == shield::catalog::QueueItemState::Downloading) {
        primary_label = this->translator_.Get("queue.detail.primary.pause");
    }
    else if(displayed_state == shield::catalog::QueueItemState::Paused) {
        primary_label = this->translator_.Get("queue.detail.primary.resume");
    }
    else if(displayed_state == shield::catalog::QueueItemState::Failed) {
        primary_label = this->translator_.Get("queue.detail.primary.restart");
    }

    this->queue_detail_primary_action_->SetText(
        BuildButtonHint("A", primary_label)
        + "   " + BuildButtonHint("B", this->translator_.Get("footer.pauseResumeAll")));
    this->queue_detail_secondary_action_->SetText(
        BuildButtonHint("X", this->translator_.Get("queue.detail.secondary.cancel"))
        + "   " + BuildButtonHint("Y",  this->translator_.Get("footer.cancelAll"))
        + "   " + BuildButtonHint("R",  this->translator_.Get("footer.clearItem"))
        + "   " + BuildButtonHint("ZR", this->translator_.Get("footer.clearAll"))
        + "   " + BuildButtonHint("L", this->translator_.Get("footer.moveUp"))
        + "   " + BuildButtonHint("ZL", this->translator_.Get("footer.moveDown")));
    this->queue_detail_body_->SetText(BuildQueueInfoBody(this->translator_, item));
}

void ShellLayout::ToggleQueueDetailMode(const bool enabled) {
    if(this->selected_section_ != AppSection::Queue) {
        this->queue_detail_mode_ = false;
        return;
    }

    // Keep the toggle local to the queue page. Section changes or data refreshes can
    // safely reset it without affecting the rest of the shell.
    this->queue_detail_mode_ = enabled && !this->cached_queue_items_.empty();
    this->RefreshHeader();
    this->RefreshCards();
    this->RefreshTaskProgress();
    this->RefreshFooter();
}

void ShellLayout::RunQueueControlAction(const QueueControlAction action) {
    if((this->selected_section_ != AppSection::Queue) || (this->selected_card_index_ >= this->cached_queue_items_.size())) {
        return;
    }

    if(this->queue_control_callback_ == nullptr) {
        return;
    }

    const std::string selected_identity = BuildQueueItemIdentity(this->cached_queue_items_[this->selected_card_index_]);
    this->queue_control_callback_(action, selected_identity);

    if((action == QueueControlAction::MoveUp) || (action == QueueControlAction::MoveDown)) {
        for(std::size_t index = 0; index < this->cached_queue_items_.size(); index++) {
            if(BuildQueueItemIdentity(this->cached_queue_items_[index]) == selected_identity) {
                this->selected_card_index_ = index;
                this->EnsureCardSelectionVisible();
                this->RefreshCards();
                this->RefreshHeader();
                if(this->queue_detail_mode_) {
                    this->RefreshQueueDetailPage();
                }
                break;
            }
        }
        if(this->selected_card_index_ >= this->cached_queue_items_.size()) {
            this->selected_card_index_ = this->cached_queue_items_.empty() ? 0 : this->cached_queue_items_.size() - 1;
            this->EnsureCardSelectionVisible();
            this->RefreshCards();
            this->RefreshHeader();
        }
    }
}

void ShellLayout::RefreshTaskProgress() {
    const bool visible = this->task_progress_.active
        && (this->selected_section_ == AppSection::Queue)
        && !this->queue_detail_mode_;
    this->task_progress_track_->SetVisible(visible);
    this->task_progress_fill_->SetVisible(visible);
    this->task_progress_title_->SetVisible(visible);
    this->task_progress_detail_->SetVisible(visible);
    if(!visible) {
        return;
    }

    const float ratio = std::clamp(this->task_progress_.ratio, 0.0f, 1.0f);
    ApplyStableClampText(this->task_progress_title_, this->task_progress_.title, 1440, 78, 96);
    ApplyStableClampText(this->task_progress_detail_, this->task_progress_.detail, 1440, 116, 140);
    this->task_progress_fill_->SetWidth(static_cast<s32>(1440.0f * ratio));
}

void ShellLayout::RefreshFooter() {
    const bool progress_queue = this->task_progress_.active && (this->selected_section_ == AppSection::Queue);

    std::vector<std::string> hints;
    const auto add = [&hints](const std::string &h) { if(!h.empty()) hints.push_back(h); };

    if(this->selected_section_ != AppSection::Queue) {
        add(BuildButtonHint("ZL", this->translator_.Get("footer.sort")));
        if(this->selected_section_ != AppSection::Recommended) {
            add(BuildButtonHint("Y",  this->translator_.Get("footer.filter")));
        }
        if((this->selected_section_ == AppSection::Recommended)
            || (this->selected_section_ == AppSection::NewGames)
            || (this->selected_section_ == AppSection::Updates)
            || (this->selected_section_ == AppSection::Dlcs)) {
            add(BuildButtonHint("R",  this->translator_.Get("footer.search")));
        }
    }

    if(this->selected_section_ == AppSection::Settings) {
        add(BuildButtonHint("A", this->translator_.Get("footer.edit")));
    }
    else if((this->selected_section_ == AppSection::Queue) && !this->cached_queue_items_.empty()) {
        add(BuildButtonHint("A",  this->translator_.Get("footer.pauseResume")));
        add(BuildButtonHint("B",  this->translator_.Get("footer.pauseResumeAll")));
        add(BuildButtonHint("X",  this->translator_.Get("footer.cancel")));
        add(BuildButtonHint("Y",  this->translator_.Get("footer.cancelAll")));
        add(BuildButtonHint("R",  this->translator_.Get("footer.clearItem")));
        add(BuildButtonHint("ZR", this->translator_.Get("footer.clearAll")));
        add(BuildButtonHint("L",  this->translator_.Get("footer.moveUp")));
        add(BuildButtonHint("ZL", this->translator_.Get("footer.moveDown")));
    }
    else if((this->selected_section_ == AppSection::Recommended)
         || (this->selected_section_ == AppSection::NewGames)
         || (this->selected_section_ == AppSection::Updates)
         || (this->selected_section_ == AppSection::Dlcs)) {
        add(BuildButtonHint("A", this->translator_.Get("footer.queuePrimary")));
        if((this->selected_section_ == AppSection::Updates) || (this->selected_section_ == AppSection::Dlcs)) {
            add(BuildButtonHint("X", this->translator_.Get("footer.updateAll")));
        }
    }

    add(BuildButtonHint("PLUS", this->translator_.Get("footer.exit")));

    std::string footer;
    for(const auto &h : hints) {
        if(!footer.empty()) footer += (this->selected_section_ == AppSection::Queue ? "  " : "   ");
        footer += h;
    }
    this->footer_hints_->SetText(footer);
    this->footer_hints_->SetY(progress_queue ? 1048 : 1044);
}

void ShellLayout::RefreshClock() {
    if(!this->config_.show_clock) {
        this->header_clock_->SetVisible(false);
        return;
    }

    const std::string current_clock = shield::platform::SystemStatus::BuildClockLabel();
    if(this->clock_cache_ != current_clock) {
        this->clock_cache_ = current_clock;
        this->header_clock_->SetText(current_clock);
    }

    this->header_clock_->SetVisible(true);
}

void ShellLayout::MoveSelection(const int delta) {
    const auto visible_sections = BuildVisibleSections(this->remote_catalog_state_, this->installed_titles_);
    if(visible_sections.empty()) {
        return;
    }

    int current_index = 0;
    for(std::size_t index = 0; index < visible_sections.size(); index++) {
        if(visible_sections[index] == this->selected_section_) {
            current_index = static_cast<int>(index);
            break;
        }
    }

    const int total = static_cast<int>(visible_sections.size());
    const int next_index = (current_index + delta + total) % total;

    this->SetSelectedSection(visible_sections[static_cast<std::size_t>(next_index)]);
}

void ShellLayout::MoveCardSelection(const int delta) {
    const std::size_t card_count = this->GetVisibleCardCount();
    if(card_count == 0) {
        return;
    }

    const int next_index = std::clamp(static_cast<int>(this->selected_card_index_) + delta, 0, static_cast<int>(card_count - 1));
    this->selected_card_index_ = static_cast<std::size_t>(next_index);
    this->EnsureCardSelectionVisible();
    this->RefreshCards();
}

void ShellLayout::MoveFocusHorizontal(const int delta) {
    if(delta > 0) {
        this->focus_zone_ = FocusZone::Cards;
    }
    else if(delta < 0) {
        this->focus_zone_ = FocusZone::Sidebar;
    }

    this->RefreshNavigation();
    this->RefreshCards();
}

int ShellLayout::GetGridStep() const {
    return kGridColumns;
}

void ShellLayout::ActivateSelection() {
    if(this->focus_zone_ == FocusZone::Cards) {
        if(this->selected_section_ == AppSection::Settings) {
            this->CycleSettingsOption();
            return;
        }

        if(this->selected_section_ == AppSection::Queue) {
            this->RunQueueControlAction(QueueControlAction::PauseResume);
            return;
        }

        if((this->selected_section_ == AppSection::Recommended)
            || (this->selected_section_ == AppSection::NewGames)
            || (this->selected_section_ == AppSection::Updates)
            || (this->selected_section_ == AppSection::Dlcs)) {
            shield::catalog::QueueItem queue_item;
            if(this->queue_action_callback_ && this->TryBuildQueueItemForSelection(queue_item)) {
                this->queue_action_callback_(queue_item);
            }
            return;
        }

        return;
    }

    this->focus_zone_ = FocusZone::Cards;
    this->RefreshNavigation();
    this->RefreshCards();
    this->RefreshFooter();
}

void ShellLayout::CycleSortMode() {
    if(this->selected_section_ == AppSection::Recommended) {
        static const std::array<const char *, 3> kRecommendedSortModes = {
            "critic_score",
            "review_count",
            "name"
        };

        std::size_t index = 0;
        const std::string current = NormalizeRecommendedSortMode(this->config_.recommended_sort_mode);
        for(std::size_t candidate = 0; candidate < kRecommendedSortModes.size(); candidate++) {
            if(current == kRecommendedSortModes[candidate]) {
                index = candidate;
                break;
            }
        }

        this->config_.recommended_sort_mode = kRecommendedSortModes[(index + 1) % kRecommendedSortModes.size()];
        this->PersistConfig();
        this->InvalidateSectionCaches();
        this->RebuildCardCache();
        this->RefreshHeader();
        this->RefreshHero();
        this->RefreshCards();
        return;
    }

    static const std::array<const char *, 7> kSortModes = {
        "recent",
        "old",
        "a-z",
        "z-a",
        "asc",
        "desc",
        "recommended"
    };

    std::size_t index = 0;
    const std::string current = NormalizeSortMode(this->config_.sort_mode);
    for(std::size_t candidate = 0; candidate < kSortModes.size(); candidate++) {
        if(current == kSortModes[candidate]) {
            index = candidate;
            break;
        }
    }

    this->config_.sort_mode = kSortModes[(index + 1) % kSortModes.size()];
    this->PersistConfig();
    this->InvalidateSectionCaches();
    this->RebuildCardCache();
    this->RefreshHeader();
    this->RefreshHero();
    this->RefreshCards();
}

void ShellLayout::CycleQuickFilter() {
    auto &filter = this->quick_filters_[shield::app::ToIndex(this->selected_section_)];

    auto advance = [&filter](const std::vector<std::string> &values) {
        auto current = std::find(values.begin(), values.end(), filter);
        if(current == values.end()) {
            filter = values.front();
            return;
        }

        ++current;
        filter = (current == values.end()) ? values.front() : *current;
    };

    switch(this->selected_section_) {
        case AppSection::Installed:
            advance({ "all", "sd", "nand" });
            break;
        case AppSection::Recommended:
            filter = "all";
            break;
        case AppSection::NewGames:
            advance({ "all", "nsp", "nsz", "xci", "xcz" });
            break;
        case AppSection::Updates:
            advance({ "all", "ready", "queued" });
            break;
        case AppSection::Dlcs:
            advance({ "all", "ready", "queued" });
            break;
        case AppSection::Queue:
            advance({ "all", "active", "paused", "failed", "done" });
            break;
        case AppSection::Settings:
        default:
            filter = "all";
            break;
    }

    this->selected_card_index_ = 0;
    this->card_view_offset_ = 0;
    this->RebuildCardCache();
    this->RefreshAll();
}

void ShellLayout::CycleSettingsOption() {
    if((this->selected_section_ != AppSection::Settings) || (this->selected_card_index_ >= this->cached_cards_.size())) {
        return;
    }

    switch(this->selected_card_index_) {
        case 0:
            if(this->config_.language == "auto") {
                this->config_.language = "pt-BR";
            }
            else if(this->config_.language == "pt-BR") {
                this->config_.language = "en-US";
            }
            else if(this->config_.language == "en-US") {
                this->config_.language = "es-ES";
            }
            else {
                this->config_.language = "auto";
            }

            this->translator_.Load(shield::platform::SystemStatus::DetectPreferredLanguageTag(this->config_.language));
            break;
        case 1:
            if((this->config_.theme == "auto") || this->config_.theme.empty()) {
                this->config_.theme = "light";
            }
            else if(this->config_.theme == "light") {
                this->config_.theme = "dark";
            }
            else if(this->config_.theme == "dark") {
                this->config_.theme = "auto";
            }
            else {
                this->config_.theme = "auto";
            }
            break;
        case 2:
            this->config_.installation_model = (this->config_.installation_model == "stream") ? "direct" : "stream";
            break;
        case 3:
            this->EditConnections();
            return;
        case 4:
            this->config_.allow_unsigned_sources = !this->config_.allow_unsigned_sources;
            break;
        case 5:
            if(!this->forwarder_activated_ && this->forwarder_install_callback_) {
                this->forwarder_install_callback_();
            }
            return;
        default:
            break;
    }

    this->PersistConfig();
    this->RebuildCardCache();
    this->RefreshAll();
}

bool ShellLayout::PromptTextInput(const std::string &guide_text, std::string &value, const bool password, const std::size_t max_length) const {
    SwkbdConfig keyboard;
    if(R_FAILED(swkbdCreate(&keyboard, 0))) {
        if(this->dialog_callback_) {
            this->dialog_callback_(this->translator_.Get("dialog.settingsConnectionTitle"), this->translator_.Get("dialog.searchUnavailable"));
        }
        return false;
    }

    if(password) {
        swkbdConfigMakePresetPassword(&keyboard);
        swkbdConfigSetPasswordFlag(&keyboard, 1);
    }
    else {
        swkbdConfigMakePresetDefault(&keyboard);
    }

    swkbdConfigSetGuideText(&keyboard, guide_text.c_str());
    swkbdConfigSetInitialText(&keyboard, value.c_str());
    swkbdConfigSetStringLenMax(&keyboard, static_cast<u32>(std::max<std::size_t>(max_length, 1)));

    std::vector<char> output(max_length + 1, '\0');
    const Result result = swkbdShow(&keyboard, output.data(), output.size());
    swkbdClose(&keyboard);
    if(R_FAILED(result)) {
        return false;
    }

    value = TrimWhitespace(output.data());
    return true;
}

void ShellLayout::EditConnections() {
    auto edited = this->config_;
    bool changed = false;

    changed = this->PromptTextInput(this->translator_.Get("dialog.connection.catalogTitleGuide"), edited.catalog_title) || changed;
    changed = this->PromptTextInput(this->translator_.Get("dialog.connection.catalogUrlGuide"), edited.catalog_url) || changed;
    changed = this->PromptTextInput(this->translator_.Get("dialog.connection.catalogUsernameGuide"), edited.catalog_username) || changed;
    changed = this->PromptTextInput(this->translator_.Get("dialog.connection.catalogPasswordGuide"), edited.catalog_password, true) || changed;

    if(!changed) {
        return;
    }

    this->config_ = std::move(edited);
    const bool saved = this->PersistConfig(!this->applet_mode_);
    if(!saved) {
        if(this->dialog_callback_) {
            this->dialog_callback_(this->translator_.Get("dialog.settingsConnectionTitle"), this->translator_.Get("dialog.searchUnavailable"));
        }
        return;
    }
    if(this->applet_mode_ || !this->config_changed_callback_) {
        this->RebuildCardCache();
        this->RefreshAll();
    }

    if(this->dialog_callback_) {
        this->dialog_callback_(this->translator_.Get("dialog.settingsConnectionTitle"), this->translator_.Get("dialog.connectionSaved"));
    }
}

void ShellLayout::BeginSearch() {
    SwkbdConfig keyboard;
    if(R_FAILED(swkbdCreate(&keyboard, 0))) {
        if(this->dialog_callback_) {
            this->dialog_callback_(this->translator_.Get("dialog.searchTitle"), this->translator_.Get("dialog.searchUnavailable"));
        }
        return;
    }

    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetGuideText(&keyboard, this->translator_.Get("dialog.searchGuide").c_str());
    swkbdConfigSetInitialText(&keyboard, this->search_query_.c_str());

    char output[256] = {};
    const Result result = swkbdShow(&keyboard, output, sizeof(output));
    swkbdClose(&keyboard);
    if(R_FAILED(result)) {
        return;
    }

    this->search_query_ = TrimWhitespace(output);
    this->selected_card_index_ = 0;
    this->card_view_offset_ = 0;
    this->RebuildCardCache();
    this->RefreshAll();
}

void ShellLayout::QueueAllVisibleItems() {
    std::vector<shield::catalog::QueueItem> items_to_queue;

    if((this->queue_action_callback_ == nullptr) && (this->queue_bulk_action_callback_ == nullptr)) {
        return;
    }

    if(this->selected_section_ == AppSection::Updates) {
        for(const auto &candidate : this->cached_update_candidates_) {
            shield::catalog::QueueItem queue_item;
            if(this->TryBuildQueueItemForUpdateCandidate(candidate, queue_item)) {
                items_to_queue.push_back(std::move(queue_item));
            }
        }
    }

    else if(this->selected_section_ == AppSection::Dlcs) {
        for(const auto &title : this->cached_remote_titles_) {
            shield::catalog::QueueItem queue_item;
            if(this->TryBuildQueueItemForRemoteTitle(title, queue_item, false, false)) {
                items_to_queue.push_back(std::move(queue_item));
            }
        }
    }

    if(items_to_queue.empty()) {
        return;
    }

    if(this->queue_bulk_action_callback_ != nullptr) {
        this->queue_bulk_action_callback_(items_to_queue);
        return;
    }

    for(const auto &item : items_to_queue) {
        this->queue_action_callback_(item);
    }
}

void ShellLayout::ShowCurrentSelectionInfo() {
    if(!this->dialog_callback_) {
        return;
    }

    if((this->selected_section_ == AppSection::Queue) && this->queue_detail_mode_ && (this->selected_card_index_ < this->cached_queue_items_.size())) {
        const auto &item = this->cached_queue_items_[this->selected_card_index_];
        this->dialog_callback_(item.name.empty() ? item.title_id : item.name, BuildQueueInfoBody(this->translator_, item));
        return;
    }

    if((this->focus_zone_ == FocusZone::Cards) && !this->GetSelectedCardDialogTitle().empty()) {
        this->dialog_callback_(this->GetSelectedCardDialogTitle(), this->GetSelectedCardDialogBody());
        return;
    }

    this->dialog_callback_(this->translator_.Get("dialog.infoTitle"), this->translator_.Get(GetSectionPreviewKey(this->selected_section_)));
}

bool ShellLayout::PersistConfig(const bool notify_application) {
    const bool effective_notify = notify_application && !this->applet_mode_;
    const bool saved = shield::app::AppConfigRepository::Save(this->config_);
    if(!saved) {
        return false;
    }
    if(effective_notify && this->config_changed_callback_) {
        this->config_changed_callback_(this->config_);
    }
    return true;
}

void ShellLayout::HandleTouch(const pu::ui::TouchPoint &touch_pos) {
    const auto visible_sections = BuildVisibleSections(this->remote_catalog_state_, this->installed_titles_);
    for(std::size_t visible_index = 0; visible_index < visible_sections.size(); visible_index++) {
        const auto section = visible_sections[visible_index];
        const auto item_y = 175 + static_cast<s32>(visible_index) * 96;

        if(touch_pos.HitsRegion(34, item_y, 292, 78)) {
            this->focus_zone_ = FocusZone::Sidebar;
            this->SetSelectedSection(section);
            return;
        }
    }

    for(std::size_t index = 0; index < this->GetCardPageSlotCount(); index++) {
        const s32 base_x = 420;
        const s32 base_y = 188 + static_cast<s32>(index) * 80;
        const s32 width = 1440;
        const s32 height = 72;

        if(touch_pos.HitsRegion(base_x, base_y, width, height)) {
            this->focus_zone_ = FocusZone::Cards;
            this->selected_card_index_ = this->card_view_offset_ + index;
            this->RefreshNavigation();
            this->RefreshCards();
            this->RefreshFooter();
            if(this->selected_section_ != AppSection::Queue) {
                this->ActivateSelection();
            }
            return;
        }
    }
}

bool ShellLayout::TryBuildQueueItemForRemoteTitle(const shield::catalog::RemoteTitleMetadata &title, shield::catalog::QueueItem &out_item, const bool include_latest_update, const bool include_all_dlcs) const {
    out_item = {};
    out_item.title_id = title.id;
    out_item.base_title_id = title.base_title_id;
    out_item.name = title.name.empty() ? title.id : title.name;
    out_item.subtitle = title.publisher.empty() ? (this->translator_.Get("catalog.titleIdPrefix") + title.id) : title.publisher;
    out_item.package_format = title.package_format;
    out_item.size = title.size;
    out_item.target_location = "SD";
    out_item.installation_model = this->config_.installation_model == "stream" ? "stream" : "direct";
    out_item.keep_download = false;
    out_item.delete_after_download = !out_item.keep_download;
    out_item.verify_integrity = true;
    out_item.auto_start = true;
    out_item.convert_standard_crypto = false;
    out_item.reinstall_ncas = false;
    out_item.include_all_dlcs = include_all_dlcs;
    out_item.include_latest_update = include_latest_update;
    out_item.retry_limit = 2;

    out_item.source_url = FindBestFileUrlForTitle(this->remote_catalog_state_.catalog.files, title.id);

    return !out_item.title_id.empty();
}

bool ShellLayout::TryBuildQueueItemForUpdateCandidate(const shield::catalog::UpdateCandidate &candidate, shield::catalog::QueueItem &out_item) const {
    out_item = {};
    out_item.title_id = candidate.remote.id;
    out_item.base_title_id = candidate.remote.base_title_id;
    out_item.name = candidate.remote.name.empty() ? candidate.installed.name : candidate.remote.name;
    out_item.subtitle = BuildUpdateSubtitle(this->translator_, candidate);
    out_item.package_format = candidate.remote.package_format;
    out_item.size = candidate.remote.size;
    out_item.target_location = "SD";
    out_item.installation_model = this->config_.installation_model == "stream" ? "stream" : "direct";
    out_item.keep_download = false;
    out_item.delete_after_download = !out_item.keep_download;
    out_item.verify_integrity = true;
    out_item.auto_start = true;
    out_item.convert_standard_crypto = false;
    out_item.reinstall_ncas = false;
    out_item.include_all_dlcs = false;
    out_item.include_latest_update = false;
    out_item.retry_limit = 2;

    out_item.source_url = FindBestFileUrlForTitle(this->remote_catalog_state_.catalog.files, candidate.remote.id);

    return !out_item.title_id.empty();
}

bool ShellLayout::TryBuildQueueItemForSelection(shield::catalog::QueueItem &out_item) const {
    if(this->focus_zone_ != FocusZone::Cards) {
        return false;
    }

    if(this->selected_section_ == AppSection::Recommended || this->selected_section_ == AppSection::NewGames || this->selected_section_ == AppSection::Dlcs) {
        if(this->selected_card_index_ >= this->cached_remote_titles_.size()) {
            return false;
        }

        const auto &title = this->cached_remote_titles_[this->selected_card_index_];
        const bool is_new_game = (this->selected_section_ == AppSection::Recommended) || (this->selected_section_ == AppSection::NewGames);
        const bool include_related_content = is_new_game;
        return this->TryBuildQueueItemForRemoteTitle(title, out_item, include_related_content, include_related_content);
    }

    if(this->selected_section_ == AppSection::Updates) {
        if(this->selected_card_index_ >= this->cached_update_candidates_.size()) {
            return false;
        }

        return this->TryBuildQueueItemForUpdateCandidate(this->cached_update_candidates_[this->selected_card_index_], out_item);
    }

    return false;
}

void ShellLayout::SetSelectedSection(const shield::app::AppSection section) {
    const bool was_searchable = (this->selected_section_ == AppSection::Recommended)
        || (this->selected_section_ == AppSection::NewGames)
        || (this->selected_section_ == AppSection::Updates)
        || (this->selected_section_ == AppSection::Dlcs);
    const bool will_be_searchable = (section == AppSection::Recommended)
        || (section == AppSection::NewGames)
        || (section == AppSection::Updates)
        || (section == AppSection::Dlcs);
    if(was_searchable && !will_be_searchable) {
        this->search_query_.clear();
    }
    this->selected_section_ = section;
    this->queue_detail_mode_ = false;
    this->selected_card_index_ = 0;
    this->card_view_offset_ = 0;
    this->RebuildCardCache();
    this->RefreshAll();
}

bool ShellLayout::IsCardFocused() const {
    return this->focus_zone_ == FocusZone::Cards;
}

std::string ShellLayout::GetSelectedCardDialogTitle() const {
    if(this->selected_card_index_ < this->card_view_offset_) {
        return "";
    }

    const std::size_t local_index = this->selected_card_index_ - this->card_view_offset_;
    if(local_index >= this->card_dialog_titles_.size()) {
        return "";
    }

    return this->card_dialog_titles_[local_index];
}

std::string ShellLayout::GetSelectedCardDialogBody() const {
    if(this->selected_card_index_ < this->card_view_offset_) {
        return "";
    }

    const std::size_t local_index = this->selected_card_index_ - this->card_view_offset_;
    if(local_index >= this->card_dialog_bodies_.size()) {
        return "";
    }

    return this->card_dialog_bodies_[local_index];
}

void ShellLayout::ApplyConfig(const shield::app::AppConfig &config, const shield::i18n::I18n &translator, const shield::catalog::RemoteCatalogState &remote_catalog_state) {
    this->config_ = config;
    this->translator_ = translator;
    this->remote_catalog_state_ = remote_catalog_state;
    this->queue_detail_mode_ = false;
    if(!IsSectionVisible(this->selected_section_, this->remote_catalog_state_, this->installed_titles_)) {
        this->selected_section_ = AppSection::Installed;
        this->focus_zone_ = FocusZone::Sidebar;
    }
    this->InvalidateSectionCaches();
    this->RebuildCardCache();
    this->RefreshAll();
}

void ShellLayout::ApplyLoadedData(const std::vector<shield::catalog::InstalledTitle> &installed_titles, const std::vector<shield::catalog::QueueItem> &queue_items, const shield::catalog::RemoteCatalogState &remote_catalog_state, const shield::platform::SystemOverview &system_overview, const bool refresh_remote_catalog) {
    this->installed_titles_ = installed_titles;
    this->queue_items_ = queue_items;
    if(refresh_remote_catalog) {
        this->remote_catalog_state_ = remote_catalog_state;
    }
    else {
        this->remote_catalog_state_.configured = remote_catalog_state.configured;
        this->remote_catalog_state_.source_url = remote_catalog_state.source_url;
        this->remote_catalog_state_.source_title = remote_catalog_state.source_title;
        this->remote_catalog_state_.status_message = remote_catalog_state.status_message;
        this->remote_catalog_state_.warning_message = remote_catalog_state.warning_message;
        this->remote_catalog_state_.device_uid = remote_catalog_state.device_uid;
    }
    this->system_overview_ = system_overview;
    if(this->selected_card_index_ >= this->queue_items_.size()) {
        this->queue_detail_mode_ = false;
    }
    if(!IsSectionVisible(this->selected_section_, this->remote_catalog_state_, this->installed_titles_)) {
        this->selected_section_ = AppSection::Installed;
        this->focus_zone_ = FocusZone::Sidebar;
    }
    if(refresh_remote_catalog) {
        this->InvalidateSectionCaches();
    }
    else {
        this->local_section_cache_valid_ = false;
    }
    this->RebuildCardCache();
    this->RefreshAll();
}

void ShellLayout::ApplyQueueData(const std::vector<shield::catalog::QueueItem> &queue_items) {
    this->queue_items_ = queue_items;
    if(this->selected_card_index_ >= this->queue_items_.size()) {
        this->queue_detail_mode_ = false;
    }

    if(this->selected_section_ == AppSection::Queue) {
        this->RebuildCardCache();
        this->RefreshHeader();
        this->RefreshCards();
        this->RefreshFooter();
        return;
    }

    this->RefreshFooter();
}

void ShellLayout::ApplyTaskProgress(const TaskProgress &task_progress) {
    this->task_progress_ = task_progress;
    this->RefreshTaskProgress();
    this->RefreshFooter();
}

void ShellLayout::RefreshVisibleCards() {
    this->RefreshCards();
}

}
