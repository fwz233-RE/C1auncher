#include "i18n.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

I18n::Lang I18n::current_lang_ = I18n::Lang::EN;
std::unordered_map<std::string, std::string> I18n::translations_;
std::filesystem::path I18n::translations_path_;

void I18n::init(Lang lang) {
    translations_path_ = get_default_translations_path();
    current_lang_ = lang;
    if (!load_translations(lang)) {
        load_default_translations(lang);
    }
    spdlog::info("I18n initialized with language: {} (path: {})", lang_code(lang), translations_path_.string());
}

void I18n::init(Lang lang, const std::string& translations_path) {
    translations_path_ = translations_path;
    current_lang_ = lang;
    if (!load_translations(lang)) {
        load_default_translations(lang);
        spdlog::warn("Failed to load translations from {}, using defaults", translations_path);
    }
    spdlog::info("I18n initialized with language: {}", lang_code(lang));
}

I18n::Lang I18n::current_lang() {
    return current_lang_;
}

void I18n::set_lang(Lang lang) {
    if (current_lang_ != lang) {
        current_lang_ = lang;
        if (!load_translations(lang)) {
            load_default_translations(lang);
        }
        spdlog::info("Language changed to: {}", lang_code(lang));
    }
}

std::string I18n::get(const std::string& key) {
    auto it = translations_.find(key);
    if (it != translations_.end()) {
        return it->second;
    }
    // Return key itself if not found (fallback)
    return key;
}

std::vector<std::pair<I18n::Lang, std::string>> I18n::available_languages() {
    return {{Lang::EN, "English"}, {Lang::ZH_CN, "简体中文"}};
}

std::string I18n::lang_code(Lang lang) {
    switch (lang) {
    case Lang::ZH_CN:
        return "zh-CN";
    default:
        return "en";
    }
}

I18n::Lang I18n::parse_lang(const std::string& code) {
    if (code == "zh-CN" || code == "zh-Hans")
        return Lang::ZH_CN;
    return Lang::EN;
}

std::filesystem::path I18n::get_default_translations_path() {
    // Try multiple paths in order
    std::vector<std::filesystem::path> search_paths;

    // 1. Build directory (current working directory)
    search_paths.push_back(std::filesystem::current_path() / "data" / "translations");

    // 2. Executable directory (for portable installs)
    // This allows running from build directory or portable installs

    // 3. System install paths
    search_paths.push_back(std::filesystem::path("/usr/share/term-ime/translations"));
    search_paths.push_back(std::filesystem::path("/usr/local/share/term-ime/translations"));

    // Find first existing path
    for (const auto& path : search_paths) {
        if (std::filesystem::exists(path)) {
            spdlog::debug("Found translations path: {}", path.string());
            return path;
        }
    }

    // Return default (may not exist, will use fallback)
    return std::filesystem::current_path() / "data" / "translations";
}

bool I18n::load_translations(Lang lang) {
    std::string filename = lang_code(lang) + ".json";
    std::filesystem::path file_path = translations_path_ / filename;

    if (!std::filesystem::exists(file_path)) {
        spdlog::debug("Translation file not found: {}", file_path.string());
        return false;
    }

    try {
        std::ifstream file(file_path);
        json j;
        file >> j;

        translations_.clear();
        for (auto& [key, value] : j.items()) {
            if (value.is_string()) {
                translations_[key] = value.get<std::string>();
            }
        }

        spdlog::info("Loaded {} translations from {}", translations_.size(), file_path.string());
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to load translations: {}", e.what());
        return false;
    }
}

void I18n::load_default_translations(Lang lang) {
    translations_.clear();

    // Minimal fallback translations
    switch (lang) {
    case Lang::ZH_CN:
        translations_ = {
            {"mode.chinese", "中文"},   {"mode.english", "EN"},     {"hint.toggle_mode", "切换"},
            {"hint.select", "选择"},    {"hint.cancel", "取消"},    {"status.pinyin", "拼音"},
            {"settings.title", "设置"}, {"settings.close", "关闭"}, {"settings.ui_language", "界面语言"},
        };
        break;

    default:
        translations_ = {
            {"mode.chinese", "中文"},       {"mode.english", "EN"},      {"hint.toggle_mode", "Toggle"},
            {"hint.select", "Select"},      {"hint.cancel", "Cancel"},   {"status.pinyin", "Pinyin"},
            {"settings.title", "Settings"}, {"settings.close", "Close"}, {"settings.ui_language", "UI Language"},
        };
        break;
    }

    spdlog::info("Using default translations for {}", lang_code(lang));
}