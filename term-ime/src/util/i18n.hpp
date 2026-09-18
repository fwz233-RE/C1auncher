#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>

// Internationalization support
class I18n {
   public:
    // Supported languages
    enum class Lang {
        EN,     // English
        ZH_CN,  // Simplified Chinese
    };

    // Initialize with language
    static void init(Lang lang);

    // Initialize with language and custom translations path
    static void init(Lang lang, const std::string& translations_path);

    // Get current language
    static Lang current_lang();

    // Set language
    static void set_lang(Lang lang);

    // Get translated string. Returns by value: the backing map is cleared and
    // refilled on every set_lang(), and a missing key falls back to `key`
    // itself, so a reference could not outlive the call safely.
    static std::string get(const std::string& key);

    // Shorthand for get
    static std::string t(const std::string& key) { return get(key); }

    // Get available languages
    static std::vector<std::pair<Lang, std::string>> available_languages();

    // Get language code string
    static std::string lang_code(Lang lang);

    // Parse language from string
    static Lang parse_lang(const std::string& code);

   private:
    static Lang current_lang_;
    static std::unordered_map<std::string, std::string> translations_;
    static std::filesystem::path translations_path_;

    static bool load_translations(Lang lang);
    static void load_default_translations(Lang lang);
    static std::filesystem::path get_default_translations_path();
};

// Convenience macro for translation
#define TR(key) I18n::t(key)
