#pragma once

#include <string>
#include <vector>

#include <catalog/RemoteCatalog.hpp>
#include <catalog/UpdateCandidate.hpp>

namespace shield::catalog {

struct RemoteCatalogState {
    bool configured = false;
    bool loaded = false;
    std::string source_url;
    std::string source_title;
    std::string status_message;
    std::string warning_message;
    std::string device_uid;
    RemoteCatalog catalog;
    std::vector<UpdateCandidate> update_candidates;
};

}
