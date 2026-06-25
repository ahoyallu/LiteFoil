#pragma once

#include <vector>

#include <catalog/InstalledTitle.hpp>
#include <catalog/RemoteCatalog.hpp>

namespace shield::catalog {

struct UpdateCandidate {
    InstalledTitle installed;
    RemoteTitleMetadata remote;
};

class UpdatePlanner {
    public:
        static std::vector<UpdateCandidate> Build(const std::vector<InstalledTitle> &installed_titles, const RemoteCatalog &remote_catalog);
};

}
