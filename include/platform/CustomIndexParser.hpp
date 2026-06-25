#pragma once

#include <optional>
#include <string>

#include <catalog/RemoteCatalog.hpp>

namespace shield::platform {

class CustomIndexParser {
    public:
        static std::optional<shield::catalog::RemoteCatalog> ParseString(const std::string &json_text);
        static std::optional<shield::catalog::RemoteCatalog> ParseFile(const std::string &path);
};

}
