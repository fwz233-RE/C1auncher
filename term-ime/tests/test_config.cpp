#include <gtest/gtest.h>
#include "core/config.hpp"
#include <cstdio>
#include <fstream>

class ConfigTest : public ::testing::Test {
   protected:
    void SetUp() override {}
};

TEST_F(ConfigTest, DefaultConfig) {
    AppConfig config;
    EXPECT_TRUE(config.shell.empty());  // runtime default follows SHELL
    EXPECT_EQ(config.max_candidates, 9);
    EXPECT_EQ(config.log_level, "warn");
    EXPECT_TRUE(config.show_mode_indicator);
}

TEST_F(ConfigTest, DefaultLanguages) {
    auto languages = AppConfig::default_languages();
    EXPECT_FALSE(languages.empty());

    // Should have Chinese (simplified is first)
    bool has_chinese = false;
    for (const auto& lang : languages) {
        if (lang.id == "zh-Hans") {
            has_chinese = true;
            EXPECT_EQ(lang.name, "简体中文");
            EXPECT_TRUE(lang.enabled);
        }
    }
    EXPECT_TRUE(has_chinese);
}

TEST_F(ConfigTest, LanguageConfigToJson) {
    LanguageConfig lang;
    lang.id = "zh-CN";
    lang.name = "中文";
    lang.schema = "luna_pinyin";
    lang.enabled = true;

    json j = lang.to_json();
    EXPECT_EQ(j["id"], "zh-CN");
    EXPECT_EQ(j["name"], "中文");
    EXPECT_EQ(j["schema"], "luna_pinyin");
    EXPECT_EQ(j["enabled"], true);
}

TEST_F(ConfigTest, LanguageConfigFromJson) {
    json j = {{"id", "zh-Hans"}, {"name", "简体中文"}, {"schema", "luna_pinyin_simp"}, {"enabled", true}};

    LanguageConfig lang = LanguageConfig::from_json(j);
    EXPECT_EQ(lang.id, "zh-Hans");
    EXPECT_EQ(lang.name, "简体中文");
    EXPECT_EQ(lang.schema, "luna_pinyin_simp");
    EXPECT_TRUE(lang.enabled);
}

TEST_F(ConfigTest, AppConfigToJson) {
    AppConfig config;
    config.shell = "/bin/zsh";
    config.max_candidates = 8;
    config.log_level = "debug";

    json j = config.to_json();
    EXPECT_EQ(j["shell"], "/bin/zsh");
    EXPECT_EQ(j["max_candidates"], 8);
    EXPECT_EQ(j["log_level"], "debug");
}

TEST_F(ConfigTest, RimeDataDirs) {
    AppConfig config;
    EXPECT_TRUE(config.rime_shared_data_dir.empty());
    EXPECT_TRUE(config.rime_user_data_dir.empty());

    config.rime_shared_data_dir = "/custom/rime-data";
    json j = config.to_json();
    EXPECT_EQ(j["rime_shared_data_dir"], "/custom/rime-data");
}

// The candidate cap is user-configurable, and configs written before the
// rename (key "page_size") must keep working.
TEST_F(ConfigTest, MaxCandidatesLoadsAndLegacyKeyStillWorks) {
    const std::string path = "/tmp/term-ime-test-config.json";
    {
        std::ofstream out(path);
        out << R"({"max_candidates": 3})";
    }
    EXPECT_EQ(AppConfig::load(path).max_candidates, 3);
    {
        std::ofstream out(path);
        out << R"({"page_size": 2})";
    }
    EXPECT_EQ(AppConfig::load(path).max_candidates, 2);
    std::remove(path.c_str());
}

TEST_F(ConfigTest, FuzzyPinyinRoundTrip) {
    AppConfig config;
    EXPECT_TRUE(config.fuzzy_pinyin);  // fuzzy pinyin is on by default
    config.fuzzy_pinyin = true;
    EXPECT_EQ(config.to_json()["fuzzy_pinyin"], true);

    const std::string path = "/tmp/term-ime-test-fuzzy.json";
    {
        std::ofstream out(path);
        out << config.to_json().dump();
    }
    EXPECT_TRUE(AppConfig::load(path).fuzzy_pinyin);
    std::remove(path.c_str());
}

// M1: config is never trusted blindly. An out-of-range cap is clamped to the
// single-digit selector range [1,9], and a malformed (non-integer) cap falls
// back to the default WITHOUT discarding the rest of the file (previously a bad
// type threw and reset everything to defaults).
TEST_F(ConfigTest, MaxCandidatesIsClampedAndTypeSafe) {
    const std::string path = "/tmp/term-ime-test-clamp.json";
    auto load_cap = [&](const std::string& body) {
        {
            std::ofstream out(path);
            out << body;
        }
        return AppConfig::load(path);
    };

    EXPECT_EQ(load_cap(R"({"max_candidates": 20})").max_candidates, 9);
    EXPECT_EQ(load_cap(R"({"max_candidates": 0})").max_candidates, 1);
    EXPECT_EQ(load_cap(R"({"max_candidates": -5})").max_candidates, 1);
    EXPECT_EQ(load_cap(R"({"page_size": 99})").max_candidates, 9);  // legacy key too
    EXPECT_EQ(load_cap(R"({"max_candidates": 3})").max_candidates, 3);  // in range kept

    // A string cap is malformed: default to 9, and the sibling field survives.
    AppConfig survived = load_cap(R"({"max_candidates": "lots", "shell": "/bin/zsh"})");
    EXPECT_EQ(survived.max_candidates, 9);
    EXPECT_EQ(survived.shell, "/bin/zsh");

    std::remove(path.c_str());
}
