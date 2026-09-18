#pragma once

#include "core/config.hpp"
#include <string>
#include <vector>
#include <functional>

// Manages language switching and configuration
class LanguageManager {
   public:
    using LanguageChangeCallback = std::function<void(const LanguageConfig&)>;

    LanguageManager() = default;

    // Load languages from config
    void load(const AppConfig& config);

    // Switch to a specific language by id
    bool switch_language(const std::string& lang_id);

    // Switch to next enabled language
    void next_language();

    // Switch to previous enabled language
    void prev_language();

    // Get current language
    const LanguageConfig& current() const;

    // Get all enabled languages
    std::vector<LanguageConfig> enabled_languages() const;

    // Get all languages (including disabled)
    const std::vector<LanguageConfig>& all_languages() const;

    // Check if language exists
    bool has_language(const std::string& lang_id) const;

    // Set callback for language change
    void on_language_change(LanguageChangeCallback callback);

   private:
    std::vector<LanguageConfig> languages_;
    size_t current_index_ = 0;
    LanguageChangeCallback on_change_;

    size_t find_language(const std::string& lang_id) const;
};
