#pragma once

#include <array>
#include <cstddef>

namespace shield::app {

enum class AppSection : std::size_t {
    Installed = 0,
    Recommended,
    NewGames,
    Updates,
    Dlcs,
    Queue,
    Settings
};

inline constexpr std::array<AppSection, 7> kAllSections = {
    AppSection::Installed,
    AppSection::Recommended,
    AppSection::NewGames,
    AppSection::Updates,
    AppSection::Dlcs,
    AppSection::Queue,
    AppSection::Settings
};

inline constexpr std::size_t ToIndex(const AppSection section) {
    return static_cast<std::size_t>(section);
}

}
