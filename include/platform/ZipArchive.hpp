#pragma once

#include <string>

namespace shield::platform {

class ZipArchive {
    public:
        static bool ExtractAll(const std::string &archive_path, const std::string &destination_root, std::string &error_message);
};

}
