#pragma once

#include <vector>

#include <catalog/InstalledTitle.hpp>

namespace shield::platform {

class InstalledTitleScanner {
    public:
        static std::vector<shield::catalog::InstalledTitle> Scan();
};

}
