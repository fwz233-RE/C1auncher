#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Language configuration for multi-language support
struct LanguageConfig {
    std::string id;       // Language identifier (zh-CN, ja, ko, etc.)
    std::string name;     // Display name
    std::string schema;   // librime schema id
    bool enabled = true;  // Whether this language is enabled

    json to_json() const;
    static LanguageConfig from_json(const json& j);
};

struct AppConfig {
    // Shell settings
    // Empty means follow SHELL, with /bin/bash as the final fallback. Keep
    // this distinct from an explicitly configured /bin/bash when saving.
    std::string shell;
    std::string resolved_shell() const;

    // Language settings (replaces hardcoded chinese/english mode)
    std::vector<LanguageConfig> languages;
    std::string active_language = "zh-Hans";  // Current active language

    // UI language for i18n
    std::string ui_language = "zh-CN";  // UI display language: "en", "zh-CN"

    // IME settings
    std::string dict_path = "data/pinyin.dict";
    std::vector<std::string> extra_dicts;
    // Candidate bar: upper bound on how many candidates are shown per page
    // (1-9; 9 is the highest single-digit selector key). The number actually
    // shown adapts to the terminal width — see ui::FitCandidateBar.
    int max_candidates = 9;

    // Fuzzy pinyin (n/l, zh/z, r/l, r/y, hu/f, en-eng/in-ing). On by default to
    // preserve the schema's historical behaviour; the schema itself is precise,
    // so turning this off gives exact spelling.
    bool fuzzy_pinyin = true;

    // Rime data directories (optional)
    std::string rime_shared_data_dir;  // System rime-data directory
    std::string rime_user_data_dir;    // User config directory

    // Display settings
    bool show_mode_indicator = true;
    std::string candidate_bar_position = "bottom";  // "bottom" or "top"

    // Logging
    std::string log_level = "warn";  // "debug", "info", "warn", "error"
    std::string log_file = "";       // empty = no file logging

    // Path used by load(), including missing/bad files. Runtime-only: never
    // serialized, so a JSON field cannot redirect where settings are saved.
    std::string source_path;

    // Load from file
    static AppConfig load(const std::string& path);

    // Save to file. Returns false (and logs) on any filesystem/IO failure;
    // never throws.
    bool save(const std::string& path) const;

    // Get default config path
    static std::string default_path();

    // Convert to/from JSON
    json to_json() const;
    static AppConfig from_json(const json& j);

    // Get default languages
    static std::vector<LanguageConfig> default_languages();
};
