#include "language.hpp"
#include <spdlog/spdlog.h>

void LanguageManager::load(const AppConfig& config) {
    languages_ = config.languages;
    // Reset so an empty (or shorter) list can never leave a stale index behind.
    current_index_ = 0;

    // Find and set active language
    size_t idx = find_language(config.active_language);
    if (idx < languages_.size() && languages_[idx].enabled) {
        current_index_ = idx;
    } else {
        // Find first enabled language
        for (size_t i = 0; i < languages_.size(); ++i) {
            if (languages_[i].enabled) {
                current_index_ = i;
                break;
            }
        }
    }

    spdlog::info("LanguageManager loaded {} languages, active: {}", languages_.size(), current().name);
}

bool LanguageManager::switch_language(const std::string& lang_id) {
    size_t idx = find_language(lang_id);
    if (idx >= languages_.size()) {
        spdlog::warn("Language not found: {}", lang_id);
        return false;
    }

    if (!languages_[idx].enabled) {
        spdlog::warn("Language disabled: {}", lang_id);
        return false;
    }

    if (idx == current_index_) {
        return true;  // Already active
    }

    current_index_ = idx;
    spdlog::info("Switched to language: {} ({})", current().name, current().id);

    if (on_change_) {
        on_change_(current());
    }

    return true;
}

void LanguageManager::next_language() {
    if (languages_.empty()) {
        return;  // no language configured: nothing to switch to
    }

    // Find next enabled language
    for (size_t i = current_index_ + 1; i < languages_.size(); ++i) {
        if (languages_[i].enabled) {
            current_index_ = i;
            spdlog::info("Switched to language: {}", current().name);
            if (on_change_)
                on_change_(current());
            return;
        }
    }
    // Wrap around to first enabled
    for (size_t i = 0; i < current_index_; ++i) {
        if (languages_[i].enabled) {
            current_index_ = i;
            spdlog::info("Switched to language: {}", current().name);
            if (on_change_)
                on_change_(current());
            return;
        }
    }
}

void LanguageManager::prev_language() {
    if (languages_.empty()) {
        return;  // no language configured: nothing to switch to
    }

    // Find previous enabled language
    for (size_t i = current_index_ - 1; i < languages_.size(); --i) {
        if (languages_[i].enabled) {
            current_index_ = i;
            spdlog::info("Switched to language: {}", current().name);
            if (on_change_)
                on_change_(current());
            return;
        }
    }
    // Wrap around to last enabled
    for (size_t i = languages_.size() - 1; i > current_index_; --i) {
        if (languages_[i].enabled) {
            current_index_ = i;
            spdlog::info("Switched to language: {}", current().name);
            if (on_change_)
                on_change_(current());
            return;
        }
    }
}

const LanguageConfig& LanguageManager::current() const {
    static const LanguageConfig empty{"", "", "", false};
    if (current_index_ >= languages_.size()) {
        return empty;  // no language configured (or stale index): safe empty value
    }
    return languages_[current_index_];
}

std::vector<LanguageConfig> LanguageManager::enabled_languages() const {
    std::vector<LanguageConfig> result;
    for (const auto& lang : languages_) {
        if (lang.enabled) {
            result.push_back(lang);
        }
    }
    return result;
}

const std::vector<LanguageConfig>& LanguageManager::all_languages() const {
    return languages_;
}

bool LanguageManager::has_language(const std::string& lang_id) const {
    return find_language(lang_id) < languages_.size();
}

void LanguageManager::on_language_change(LanguageChangeCallback callback) {
    on_change_ = callback;
}

size_t LanguageManager::find_language(const std::string& lang_id) const {
    for (size_t i = 0; i < languages_.size(); ++i) {
        if (languages_[i].id == lang_id) {
            return i;
        }
    }
    return languages_.size();  // Not found
}