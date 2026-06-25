#pragma once

#include <vector>

#include <catalog/QueueItem.hpp>

namespace shield::app {

class QueueRepository {
    public:
        static std::vector<shield::catalog::QueueItem> Load();
        static bool Save(const std::vector<shield::catalog::QueueItem> &items);
};

}
