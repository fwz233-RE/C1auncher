#include <gtest/gtest.h>
#include "core/config.hpp"
#include "core/input_processor.hpp"
#include "terminal/pty.hpp"
#include "ime/rime_engine.hpp"
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
class ShellEnvironment {
 public:
    ShellEnvironment() {
        const char* value = getenv("SHELL");
        present = value != nullptr;
        if (value) original = value;
    }
    ~ShellEnvironment() {
        if (present) setenv("SHELL", original.c_str(), 1);
        else unsetenv("SHELL");
    }
 private:
    bool present;
    std::string original;
};
class TemporaryDirectory {
 public:
    TemporaryDirectory() {
        char pattern[] = "/tmp/term-ime-followup-XXXXXX";
        if (mkdtemp(pattern)) path = pattern;
    }
    ~TemporaryDirectory() { if (!path.empty()) std::filesystem::remove_all(path); }
    std::filesystem::path path;
};
}

TEST(ConfigFollowup, ExplicitShellWinsAndAutomaticChoiceSurvivesSerialization) {
    ShellEnvironment restore;
    setenv("SHELL", "/bin/sh", 1);
    auto automatic = AppConfig::from_json(json::object());
    EXPECT_TRUE(automatic.shell.empty());
    EXPECT_EQ(automatic.resolved_shell(), "/bin/sh");
    EXPECT_EQ(AppConfig::from_json(automatic.to_json()).resolved_shell(), "/bin/sh");
    EXPECT_EQ(AppConfig::from_json({{"shell", "/bin/bash"}}).resolved_shell(), "/bin/bash");
    EXPECT_EQ(AppConfig::from_json({{"shell", "/bin/zsh"}}).resolved_shell(), "/bin/zsh");
    EXPECT_EQ(AppConfig::from_json({{"shell", ""}}).resolved_shell(), "/bin/sh");
    setenv("SHELL", "", 1);
    EXPECT_EQ(automatic.resolved_shell(), "/bin/bash");
    unsetenv("SHELL");
    EXPECT_EQ(automatic.resolved_shell(), "/bin/bash");
}

TEST(PtyFollowup, MissingExecutableReportsFailureAndCanRetry) {
    Pty pty;
    EXPECT_FALSE(pty.spawn("/nonexistent/term-ime-test-shell"));
    EXPECT_NE(pty.spawn_error().find("/nonexistent/term-ime-test-shell"), std::string::npos);
    EXPECT_EQ(pty.fd(), -1);
    EXPECT_TRUE(pty.spawn("/bin/sh"));
    EXPECT_TRUE(pty.spawn_error().empty());
    EXPECT_GE(pty.fd(), 0);
}

TEST(PtyFollowup, NonExecutableAndInvalidInterpreterAreStartupErrors) {
    TemporaryDirectory directory;
    ASSERT_FALSE(directory.path.empty());
    const auto script = directory.path / "shell";
    { std::ofstream out(script); out << "#!/nonexistent/term-ime-interpreter\n"; }
    Pty pty;
    EXPECT_FALSE(pty.spawn(script));
    EXPECT_EQ(pty.fd(), -1);
    ASSERT_EQ(chmod(script.c_str(), 0700), 0);
    EXPECT_FALSE(pty.spawn(script));
    EXPECT_EQ(pty.fd(), -1);
    // Failed children must be reaped, not left for the destructor or main loop.
    int status;
    EXPECT_EQ(waitpid(-1, &status, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST(InputPasteFollowup, AllBytesAreOpaqueUntilExactEndMarker) {
    InputProcessor input;
    const std::string payload = std::string("\x1b[200~hello 123 中文\x01s\x01 \x01\x03\x1b[20x\x1b\x1b[201~");
    std::string written;
    int begins = 0;
    for (uint8_t byte : payload) {
        const auto result = input.process(byte);
        EXPECT_FALSE(result.toggle_mode);
        if (result.forward) {
            EXPECT_TRUE(result.paste);
            written.append(result.data.begin(), result.data.end());
        }
        begins += result.paste_begin;
        if (begins) {
            EXPECT_FALSE(input.in_escape());
            EXPECT_FALSE(input.flush_escape().forward);
        }
        EXPECT_LE(result.data.size(), 6u);
    }
    EXPECT_EQ(written, payload);
    EXPECT_EQ(begins, 1);
    input.process(1);
    EXPECT_TRUE(input.process(' ').toggle_mode);
}

TEST(InputPasteFollowup, ResetEndsPasteAndStorageDoesNotGrowWithPayload) {
    InputProcessor input;
    for (uint8_t byte : std::string("\x1b[200~")) input.process(byte);
    for (int i = 0; i < 100000; ++i) {
        auto result = input.process('a');
        ASSERT_EQ(result.data.size(), 1u);
        ASSERT_TRUE(result.paste);
    }
    input.reset();
    input.process(1);
    EXPECT_TRUE(input.process(' ').toggle_mode);
}

TEST(RimeSelectionFollowup, LargePagesUsePageRelativeCandidateApi) {
    TemporaryDirectory directory;
    ASSERT_FALSE(directory.path.empty());
    { std::ofstream patch(directory.path / "luna_pinyin_simp.custom.yaml");
      patch << "patch:\n  menu/page_size: 12\n  translator/enable_user_dict: false\n"; }
    RimeIme ime("", directory.path);
    ASSERT_TRUE(ime.initialize());
    ASSERT_TRUE(ime.select_schema("luna_pinyin_simp"));
    ime.set_mode(ImeMode::Chinese);
    for (int index : {0, 8, 9, 11}) {
        ASSERT_TRUE(ime.input('n'));
        ASSERT_TRUE(ime.input('i'));
        const auto candidates = ime.candidates();
        ASSERT_EQ(candidates.size(), 12u);
        EXPECT_TRUE(ime.select(-1).empty());
        EXPECT_TRUE(ime.select(12).empty());
        EXPECT_EQ(ime.select(index), candidates[index].text);
        EXPECT_TRUE(ime.buffer().empty());
    }
    ASSERT_TRUE(ime.input('n'));
    ASSERT_TRUE(ime.input('i'));
    ime.page_down();
    const auto second_page = ime.candidates();
    ASSERT_EQ(second_page.size(), 12u);
    EXPECT_EQ(ime.select(9), second_page[9].text);
}
