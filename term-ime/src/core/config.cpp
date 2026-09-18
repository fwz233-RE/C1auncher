#include "config.hpp"
#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <unistd.h>
#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <cstring>

namespace {
template <typename T>
T setting(const json& j, const char* key, const T& fallback) {
    try {
        return j.value(key, fallback);
    } catch (const json::exception&) {
        spdlog::warn("Ignoring invalid configuration field {}", key);
        return fallback;
    }
}
}  // namespace

namespace fs = std::filesystem;

// LanguageConfig implementation
json LanguageConfig::to_json() const {
    return json{{"id", id}, {"name", name}, {"schema", schema}, {"enabled", enabled}};
}

LanguageConfig LanguageConfig::from_json(const json& j) {
    LanguageConfig cfg;
    cfg.id = setting(j, "id", std::string());
    cfg.name = setting(j, "name", std::string());
    cfg.schema = setting(j, "schema", std::string());
    if (cfg.schema.empty() && cfg.id == "zh-Hans")
        cfg.schema = "luna_pinyin_simp";
    cfg.enabled = setting(j, "enabled", true);
    return cfg;
}

// AppConfig implementation
std::string AppConfig::resolved_shell() const {
    if (!shell.empty())
        return shell;
    const char* value = getenv("SHELL");
    return value && *value ? value : "/bin/bash";
}

std::vector<LanguageConfig> AppConfig::default_languages() {
    return {{"zh-Hans", "简体中文", "luna_pinyin_simp", true}};
}

json AppConfig::to_json() const {
    json j;
    j["shell"] = shell;

    json langs = json::array();
    for (const auto& lang : languages) {
        langs.push_back(lang.to_json());
    }
    j["languages"] = langs;
    j["active_language"] = active_language;
    j["ui_language"] = ui_language;

    // Legacy fields from the pre-Rime engine are intentionally not advertised
    // or written back: dictionaries are configured through Rime schemas.
    j["max_candidates"] = max_candidates;
    j["fuzzy_pinyin"] = fuzzy_pinyin;

    j["rime_shared_data_dir"] = rime_shared_data_dir;
    j["rime_user_data_dir"] = rime_user_data_dir;

    j["show_mode_indicator"] = show_mode_indicator;
    j["candidate_bar_position"] = candidate_bar_position;

    j["log_level"] = log_level;
    j["log_file"] = log_file;

    return j;
}

AppConfig AppConfig::from_json(const json& j) {
    AppConfig cfg;
    cfg.shell = setting(j, "shell", cfg.shell);

    if (j.contains("languages") && j["languages"].is_array()) {
        for (const auto& lang : j["languages"]) {
            if (lang.is_object())
                cfg.languages.push_back(LanguageConfig::from_json(lang));
        }
    }
    if (cfg.languages.empty())
        cfg.languages = default_languages();
    cfg.active_language = setting(j, "active_language", cfg.active_language);
    cfg.ui_language = setting(j, "ui_language", cfg.ui_language);

    // Only convert after clamping, including integers too large for int.
    const char* cap_key = j.contains("max_candidates") ? "max_candidates" : "page_size";
    if (j.contains(cap_key) && j[cap_key].is_number_integer()) {
        const auto& cap = j[cap_key];
        if (cap.is_number_unsigned())
            cfg.max_candidates = static_cast<int>(std::max<uint64_t>(1, std::min<uint64_t>(9, cap.get<uint64_t>())));
        else
            cfg.max_candidates = static_cast<int>(std::max<int64_t>(1, std::min<int64_t>(9, cap.get<int64_t>())));
    }
    cfg.fuzzy_pinyin = setting(j, "fuzzy_pinyin", cfg.fuzzy_pinyin);
    cfg.rime_shared_data_dir = setting(j, "rime_shared_data_dir", cfg.rime_shared_data_dir);
    cfg.rime_user_data_dir = setting(j, "rime_user_data_dir", cfg.rime_user_data_dir);
    cfg.show_mode_indicator = setting(j, "show_mode_indicator", cfg.show_mode_indicator);
    // The shell's rows/scroll region reserve the bottom line. Top placement
    // has never been implemented; reject it explicitly instead of pretending
    // the serialized preference took effect.
    if (setting(j, "candidate_bar_position", std::string("bottom")) != "bottom")
        spdlog::warn("candidate_bar_position only supports bottom");
    if (j.contains("dict_path") || j.contains("extra_dicts"))
        spdlog::warn("dict_path/extra_dicts are obsolete; use Rime schema dictionaries");
    cfg.log_level = setting(j, "log_level", cfg.log_level);
    if (cfg.log_level != "debug" && cfg.log_level != "info" && cfg.log_level != "warn" &&
        cfg.log_level != "error" && cfg.log_level != "off")
        cfg.log_level = "warn";
    cfg.log_file = setting(j, "log_file", cfg.log_file);

    return cfg;
}

std::string AppConfig::default_path() {
    // Try XDG_CONFIG_HOME first
    const char* xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        return fs::path(xdg_config) / "term-ime" / "config.json";
    }
    // Fallback to HOME/.config
    const char* home = getenv("HOME");
    if (home) {
        return fs::path(home) / ".config" / "term-ime" / "config.json";
    }
    return "config.json";
}

AppConfig AppConfig::load(const std::string& path) {
    AppConfig cfg;
    cfg.languages = default_languages();
    std::error_code ec;
    const auto absolute = fs::absolute(path, ec);
    const std::string source = ec ? path : absolute.string();
    try {
        if (fs::exists(path)) {
            std::ifstream file(path);
            json j;
            file >> j;
            if (!j.is_object())
                throw std::runtime_error("configuration must be a JSON object");
            cfg = from_json(j);
        }
    } catch (const std::exception& e) {
        spdlog::error("Failed to load config {}: {}", path, e.what());
    }
    cfg.source_path = source;
    return cfg;
}

// Never throws: callers (e.g. settings close, which runs from a libuv callback)
// must not have filesystem errors escape into the event loop.
bool AppConfig::save(const std::string& path) const {
    try {
        fs::path p(path);

        // Create parent directories if needed
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }

        const std::string body = to_json().dump(4) + "\n";
        std::string pattern = p.string() + ".tmp.XXXXXX";
        std::vector<char> temp(pattern.begin(), pattern.end());
        temp.push_back('\0');
        int fd = mkstemp(temp.data());
        if (fd < 0)
            return false;
        bool ok = true;
        size_t offset = 0;
        while (offset < body.size()) {
            ssize_t written = ::write(fd, body.data() + offset, body.size() - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0) {
                ok = false;
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if (ok && fsync(fd) != 0)
            ok = false;
        if (close(fd) != 0)
            ok = false;
        if (ok && std::rename(temp.data(), path.c_str()) != 0)
            ok = false;
        if (!ok) {
            unlink(temp.data());
            spdlog::error("Failed to atomically save config {}", path);
        }
        return ok;
    } catch (const std::exception& e) {
        spdlog::error("Failed to save config to {}: {}", path, e.what());
        return false;
    }
}