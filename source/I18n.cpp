#include <i18n/I18n.hpp>

#include <fstream>
#include <vector>

#include <third_party/nlohmann/json.hpp>

namespace shield::i18n {
namespace {

using Json = nlohmann::json;

bool TryLoadLanguageFile(const std::string &language_tag, std::unordered_map<std::string, std::string> &strings) {
    std::ifstream language_stream("romfs:/lang/" + language_tag + ".json");
    if(!language_stream.good()) {
        return false;
    }

    const Json json = Json::parse(language_stream, nullptr, false);
    if(json.is_discarded() || !json.is_object()) {
        return false;
    }

    strings.clear();

    // Phase 0 keeps translation files intentionally flat so future tooling can stay simple.
    for(const auto &[key, value] : json.items()) {
        if(value.is_string()) {
            strings[key] = value.get<std::string>();
        }
    }

    return true;
}

}

bool I18n::Load(const std::string &preferred_language_tag) {
    std::vector<std::string> candidates;
    candidates.push_back(preferred_language_tag.empty() ? "en-US" : preferred_language_tag);

    if(candidates.front() != "en-US") {
        candidates.push_back("en-US");
    }

    // Build the new table OUTSIDE the lock so reader threads can keep serving
    // from the current map without contention. Then swap under exclusive lock.
    std::unordered_map<std::string, std::string> new_strings;
    std::string new_tag;
    bool ok = false;
    for(const auto &candidate : candidates) {
        if(TryLoadLanguageFile(candidate, new_strings)) {
            new_tag = candidate;
            ok = true;
            break;
        }
    }
    if(!ok) {
        new_strings.clear();
        new_tag = "en-US";
    }

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        strings_ = std::move(new_strings);
        language_tag_ = std::move(new_tag);
    }
    return ok;
}

std::string I18n::Get(const std::string &key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = strings_.find(key);
    if(found == strings_.end()) {
        return key;
    }

    return found->second;
}

std::string I18n::GetLanguageTag() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return language_tag_;
}

}
