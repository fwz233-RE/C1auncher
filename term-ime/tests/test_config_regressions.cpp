#include "core/config.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unistd.h>

class ConfigRegressionTest : public ::testing::Test {
   protected:
    std::filesystem::path directory;
    void SetUp() override {
        char pattern[] = "/tmp/term-ime-config-regression-XXXXXX";
        ASSERT_NE(mkdtemp(pattern), nullptr);
        directory = pattern;
    }
    void TearDown() override { std::filesystem::remove_all(directory); }
};

TEST_F(ConfigRegressionTest, SourcePathSurvivesMissingAndMalformedFiles) {
    const auto path = directory / "custom.json";
    EXPECT_EQ(AppConfig::load(path).source_path, path.string());
    { std::ofstream file(path); file << "not json"; }
    auto config = AppConfig::load(path);
    EXPECT_EQ(config.source_path, path.string());
    EXPECT_EQ(config.max_candidates, 9);
    EXPECT_TRUE(config.save(config.source_path));
    EXPECT_EQ(AppConfig::load(path).source_path, path.string());
}

TEST_F(ConfigRegressionTest, JsonCannotRedirectSaveDestination) {
    const auto path = directory / "custom.json";
    { std::ofstream file(path); file << R"({"source_path":"/tmp/other.json","max_candidates":4})"; }
    auto config = AppConfig::load(path);
    EXPECT_EQ(config.source_path, path.string());
    EXPECT_FALSE(config.to_json().contains("source_path"));
}

TEST_F(ConfigRegressionTest, MalformedFieldsDoNotDiscardValidSiblings) {
    auto config = AppConfig::from_json({{"shell", "/bin/zsh"}, {"fuzzy_pinyin", "bad"},
                                       {"ui_language", 123}, {"rime_user_data_dir", "/custom/user"}});
    EXPECT_EQ(config.shell, "/bin/zsh");
    EXPECT_EQ(config.ui_language, "zh-CN");
    EXPECT_TRUE(config.fuzzy_pinyin);
    EXPECT_EQ(config.rime_user_data_dir, "/custom/user");
}

TEST_F(ConfigRegressionTest, MissingFuzzyPreferenceKeepsDefault) {
    EXPECT_EQ(AppConfig::from_json(json::object()).fuzzy_pinyin, AppConfig{}.fuzzy_pinyin);
}

TEST_F(ConfigRegressionTest, CandidateCapClampsBeforeNarrowingInteger) {
    EXPECT_EQ(AppConfig::from_json({{"max_candidates", std::numeric_limits<uint64_t>::max()}}).max_candidates, 9);
    EXPECT_EQ(AppConfig::from_json({{"max_candidates", std::numeric_limits<int64_t>::min()}}).max_candidates, 1);
}

TEST_F(ConfigRegressionTest, KnownLanguageGetsDefaultSchema) {
    auto config = AppConfig::from_json({{"languages", json::array({{{"id", "zh-Hans"}, {"enabled", true}}})}});
    ASSERT_EQ(config.languages.size(), 1u);
    EXPECT_EQ(config.languages[0].schema, "luna_pinyin_simp");
}

TEST_F(ConfigRegressionTest, UnsupportedOptionsAreNotAdvertisedAsImplemented) {
    auto config = AppConfig::from_json({{"candidate_bar_position", "top"}, {"dict_path", "old.dict"},
                                       {"extra_dicts", json::array({"old-extra.dict"})}});
    EXPECT_EQ(config.candidate_bar_position, "bottom");
    EXPECT_FALSE(config.to_json().contains("dict_path"));
    EXPECT_FALSE(config.to_json().contains("extra_dicts"));
}

TEST_F(ConfigRegressionTest, SaveReplacesAtomicallyAndLeavesNoTemporaryFiles) {
    const auto path = directory / "config.json";
    AppConfig config;
    ASSERT_TRUE(config.save(path));
    config.max_candidates = 2;
    ASSERT_TRUE(config.save(path));
    EXPECT_EQ(AppConfig::load(path).max_candidates, 2);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                            std::filesystem::directory_iterator()), 1);
}

TEST_F(ConfigRegressionTest, FailedRenameDoesNotRemoveExistingDestination) {
    auto path = directory / "config.json";
    std::filesystem::create_directory(path);
    EXPECT_FALSE(AppConfig{}.save(path));
    EXPECT_TRUE(std::filesystem::is_directory(path));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                            std::filesystem::directory_iterator()), 1);
}
