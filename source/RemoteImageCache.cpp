#include <platform/RemoteImageCache.hpp>

namespace shield::platform {
namespace {
}

std::string RemoteImageCache::GetOrQueue(const std::string &, const std::string &, const std::string &) {
    return "";
}

bool RemoteImageCache::PumpCompletedDownloads() {
    return false;
}

void RemoteImageCache::Reset() {}

void RemoteImageCache::Shutdown() {}

}
