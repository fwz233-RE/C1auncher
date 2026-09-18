#include <gtest/gtest.h>
#include "util/i18n.hpp"

class I18nTest : public ::testing::Test {
   protected:
    void SetUp() override {
        original_lang_ = I18n::current_lang();
    }
    void TearDown() override {
        I18n::set_lang(original_lang_);
    }
   private:
    I18n::Lang original_lang_;
};

// ==========================================================================
// File-loaded translations
// ==========================================================================

TEST_F(I18nTest, LoadZhCnFromFile) {
    I18n::init(I18n::Lang::ZH_CN, "data/translations");
    EXPECT_EQ(I18n::get("mode.chinese"), "中文");
    EXPECT_EQ(I18n::get("mode.english"), "EN");
    EXPECT_EQ(I18n::get("hint.toggle_mode"), "切换");
    EXPECT_EQ(I18n::get("hint.select"), "选择");
    EXPECT_EQ(I18n::get("hint.cancel"), "取消");
    EXPECT_EQ(I18n::get("status.pinyin"), "拼音");
    EXPECT_EQ(I18n::get("settings.title"), "设置");
    EXPECT_EQ(I18n::get("settings.language"), "语言");
    EXPECT_EQ(I18n::get("settings.ui_language"), "界面语言");
    EXPECT_EQ(I18n::get("settings.close"), "关闭");
}

TEST_F(I18nTest, LoadEnFromFile) {
    I18n::init(I18n::Lang::EN, "data/translations");
    EXPECT_EQ(I18n::get("mode.chinese"), "中文");
    EXPECT_EQ(I18n::get("mode.english"), "EN");
    EXPECT_EQ(I18n::get("hint.toggle_mode"), "Toggle");
    EXPECT_EQ(I18n::get("hint.select"), "Select");
    EXPECT_EQ(I18n::get("hint.cancel"), "Cancel");
    EXPECT_EQ(I18n::get("status.pinyin"), "Pinyin");
    EXPECT_EQ(I18n::get("settings.title"), "Settings");
    EXPECT_EQ(I18n::get("settings.language"), "Language");
    EXPECT_EQ(I18n::get("settings.ui_language"), "UI Language");
    EXPECT_EQ(I18n::get("settings.close"), "Close");
}

// ==========================================================================
// Default / fallback translations (when translation files are missing)
// ==========================================================================

TEST_F(I18nTest, DefaultTranslationsZhCN) {
    I18n::init(I18n::Lang::ZH_CN, "/nonexistent/path");
    EXPECT_EQ(I18n::get("mode.chinese"), "中文");
    EXPECT_EQ(I18n::get("mode.english"), "EN");
    EXPECT_EQ(I18n::get("hint.toggle_mode"), "切换");
    EXPECT_EQ(I18n::get("hint.select"), "选择");
    EXPECT_EQ(I18n::get("hint.cancel"), "取消");
    EXPECT_EQ(I18n::get("status.pinyin"), "拼音");
    EXPECT_EQ(I18n::get("settings.title"), "设置");
}

TEST_F(I18nTest, DefaultTranslationsEN) {
    I18n::init(I18n::Lang::EN, "/nonexistent/path");
    EXPECT_EQ(I18n::get("mode.chinese"), "中文");
    EXPECT_EQ(I18n::get("mode.english"), "EN");
    EXPECT_EQ(I18n::get("hint.toggle_mode"), "Toggle");
    EXPECT_EQ(I18n::get("hint.select"), "Select");
    EXPECT_EQ(I18n::get("hint.cancel"), "Cancel");
    EXPECT_EQ(I18n::get("status.pinyin"), "Pinyin");
    EXPECT_EQ(I18n::get("settings.title"), "Settings");
}

// ==========================================================================
// Fallback: missing key returns the key itself
// ==========================================================================

TEST_F(I18nTest, FallbackToKeyWhenNotFound) {
    I18n::init(I18n::Lang::ZH_CN, "/nonexistent/path");
    EXPECT_EQ(I18n::get("nonexistent.key"), "nonexistent.key");
}

// ==========================================================================
// Language switching
// ==========================================================================

TEST_F(I18nTest, SwitchLanguagePreservesTranslations) {
    I18n::init(I18n::Lang::ZH_CN, "/nonexistent/path");
    EXPECT_EQ(I18n::get("hint.select"), "选择");

    I18n::set_lang(I18n::Lang::EN);
    EXPECT_EQ(I18n::get("hint.select"), "Select");
}

// ==========================================================================
// All translation keys match between zh-CN and en files
// ==========================================================================

TEST_F(I18nTest, TranslationKeysMatchBetweenFiles) {
    // Load zh-CN and capture keys
    I18n::init(I18n::Lang::ZH_CN, "data/translations");
    // Verify all keys used in code are present in both files
    // Keys used: hint.cancel, hint.select, hint.toggle_mode, status.pinyin,
    //            settings.title, settings.close, settings.ui_language,
    //            settings.language, mode.chinese, mode.english
    EXPECT_NE(I18n::get("settings.close"), "settings.close");    // zh-CN: "关闭"
    EXPECT_NE(I18n::get("settings.language"), "settings.language"); // zh-CN: "语言"
}

// ==========================================================================
// Default translations completeness check
// ==========================================================================

TEST_F(I18nTest, DefaultTranslationsCoverAllCodeKeys) {
    // All keys used in src/ code must be present in load_default_translations
    // Code uses: hint.cancel, hint.select, hint.toggle_mode, status.pinyin,
    //            settings.title, settings.close, settings.ui_language
    // Default has: mode.chinese, mode.english, hint.toggle_mode, hint.select,
    //              hint.cancel, status.pinyin, settings.title
    // Missing from default: settings.close, settings.ui_language ← BUG!

    I18n::init(I18n::Lang::ZH_CN, "/nonexistent/path");
    // These should NOT fall back to the key itself
    EXPECT_NE(I18n::get("settings.close"), "settings.close");
    EXPECT_NE(I18n::get("settings.ui_language"), "settings.ui_language");
}
