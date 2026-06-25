#pragma once

#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace shield::i18n {

class I18n {
    private:
        std::unordered_map<std::string, std::string> strings_;
        std::string language_tag_ = "en-US";
        // The translator is read from worker threads (download/install progress
        // callbacks) while the UI thread may replace the whole table via
        // Load() when the user switches language. Without a lock this is a
        // data race on std::unordered_map → crash.
        mutable std::shared_mutex mutex_;

    public:
        I18n() = default;

        // std::shared_mutex is non-copyable; we provide value-semantics copy
        // that locks the source (shared) and copies only the data.  The
        // destination mutex is default-constructed — this assumes the copy
        // target is not being read concurrently (which matches our usage:
        // ShellLayout::ApplyConfig runs on the UI thread only).
        I18n(const I18n &other) {
            std::shared_lock<std::shared_mutex> lock(other.mutex_);
            strings_ = other.strings_;
            language_tag_ = other.language_tag_;
        }
        I18n &operator=(const I18n &other) {
            if(this == &other) {
                return *this;
            }
            std::shared_lock<std::shared_mutex> src_lock(other.mutex_);
            std::unique_lock<std::shared_mutex> dst_lock(mutex_);
            strings_ = other.strings_;
            language_tag_ = other.language_tag_;
            return *this;
        }

        bool Load(const std::string &preferred_language_tag);
        std::string Get(const std::string &key) const;
        std::string GetLanguageTag() const;
};

}
