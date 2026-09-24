#include <gtest/gtest.h>
#include "core/event_loop.hpp"
#include "core/app.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>

TEST(EventLoopRegression, InitializesUserDataBeforeLibuvReadsIt) {
    EventLoop loop;
    EXPECT_EQ(loop.loop()->data, nullptr);
    bool fired = false;
    loop.set_timer([&fired]() { fired = true; }, 0);
    loop.run();
    EXPECT_TRUE(fired);
}

// Test the actual App routing, not a copy of its logic. Rime's process-global
// dictionary deployment and the shell are replaced at their existing seams.
class FakeRime : public RimeIme {
   public:
    ImeMode current_mode = ImeMode::Chinese;
    std::string preedit;
    std::vector<int> selections;
    std::vector<std::pair<int, int>> keys;
    std::u32string pending;
    bool accepts = true;
    int cancels = 0, down_calls = 0, up_calls = 0, page = 0;
    ImeMode mode() const override { return current_mode; }
    void set_mode(ImeMode value) override { current_mode = value; }
    void toggle_mode() override {
        current_mode = current_mode == ImeMode::English ? ImeMode::Chinese : ImeMode::English;
    }
    ImeState state() const override { return preedit.empty() ? ImeState::Inactive : ImeState::Selecting; }
    bool input(char ch) override {
        if (!accepts)
            return false;
        if (ch == '!') {
            pending = U"！";
            preedit.clear();
        } else {
            preedit += ch;
            page = 0;
        }
        return true;
    }
    bool process_key(int key, int modifiers = 0) override {
        keys.emplace_back(key, modifiers);
        if (key != 0xFF0D)
            return false;
        pending.assign(preedit.begin(), preedit.end());
        preedit.clear();
        return true;
    }
    std::u32string take_commit() override {
        auto result = pending;
        pending.clear();
        return result;
    }
    std::string buffer() const override { return preedit; }
    std::vector<Candidate> candidates() const override {
        std::vector<Candidate> result;
        if (!preedit.empty())
            for (int i = 0; i < 9; ++i)
                result.push_back({std::u32string(1, U'a' + i + page * 16), ""});
        return result;
    }
    std::u32string select(int index) override {
        selections.push_back(index);
        preedit.clear();
        return U"选";
    }
    void backspace() override {
        if (!preedit.empty())
            preedit.pop_back();
    }
    void cancel() override { ++cancels; preedit.clear(); }
    void page_down() override { ++down_calls; page = std::min(1, page + 1); }
    void page_up() override { ++up_calls; page = std::max(0, page - 1); }
};

struct AppTestPeer {
    static FakeRime& setup(App& app, std::vector<uint8_t>& output) {
        auto ime = std::make_unique<FakeRime>();
        auto& reference = *ime;
        app.ime_ = std::move(ime);
        app.ime_ready_ = true;
        app.initialized_ = true;
        app.input_sink_ = [&output](const std::vector<uint8_t>& bytes) {
            output.insert(output.end(), bytes.begin(), bytes.end());
        };
        return reference;
    }
    static void expire(App& app) { app.on_escape_timeout(); }
    static void loop(App& app, EventLoop* loop) { app.event_loop_ = loop; }
    static void ready(App& app, bool ready) { app.ime_ready_ = ready; }
    static void cap(App& app, int count) { app.config_.max_candidates = count; }
    static void open_settings(App& app) {
        app.settings_state_.visible = true;
        ui::settings_init(app.settings_state_, app.config_);
        app.settings_state_.on_close = [&app]() { app.settings_state_.visible = false; };
    }
    static void terminal(App& app) {
        app.screen_ = std::make_unique<Screen>(23, 80);
        app.parser_ = std::make_unique<Parser>(*app.screen_);
        app.renderer_.attach_terminal(*app.parser_);
    }
    static int focus(const App& app) { return app.settings_state_.focus_index; }
    static bool pending(const App& app) { return app.input_processor_.in_escape(); }
    static void save_to(App& app, const std::string& path) {
        app.config_ = AppConfig::load(path);
        app.config_.max_candidates = 3;
        app.on_settings_close();
    }
};

class AppInputTest : public ::testing::Test {
   protected:
    App app;
    std::vector<uint8_t> output;
    FakeRime* ime = nullptr;
    void SetUp() override { ime = &AppTestPeer::setup(app, output); }
    void send(const std::string& bytes) { app.on_keyboard_data(bytes.data(), bytes.size()); }
    std::string written() const { return std::string(output.begin(), output.end()); }
};

TEST_F(AppInputTest, PageKeysDoNotSelectTheirParameterDigits) {
    send("ni");
    send("\x1b[6~");
    EXPECT_TRUE(ime->selections.empty());
    EXPECT_EQ(ime->down_calls, 1);
    send("\x1b[5~");
    EXPECT_TRUE(ime->selections.empty());
    EXPECT_EQ(ime->up_calls, 1);
    EXPECT_TRUE(output.empty());
}

TEST_F(AppInputTest, EveryPageDownSplitHasTheSameMeaning) {
    const std::string key = "\x1b[6~";
    for (size_t split = 1; split < key.size(); ++split) {
        ime->preedit = "ni";
        ime->page = 0;
        ime->down_calls = 0;
        send(key.substr(0, split));
        EXPECT_EQ(ime->preedit, "ni");
        send(key.substr(split));
        EXPECT_EQ(ime->down_calls, 1);
        EXPECT_TRUE(ime->selections.empty());
    }
}

TEST_F(AppInputTest, SplitArrowWaitsInsteadOfCancellingComposition) {
    send("ni");
    send("\x1b");
    EXPECT_EQ(ime->cancels, 0);
    EXPECT_TRUE(AppTestPeer::pending(app));
    send("[C");
    EXPECT_EQ(ime->cancels, 0);
    EXPECT_EQ(ime->down_calls, 1);
}

TEST_F(AppInputTest, BareEscapeCancelsOnlyAfterTimeout) {
    send("ni\x1b");
    EXPECT_EQ(ime->preedit, "ni");
    AppTestPeer::expire(app);
    EXPECT_TRUE(ime->preedit.empty());
    EXPECT_EQ(ime->cancels, 1);
    send("a");
    EXPECT_EQ(ime->preedit, "a");
    EXPECT_TRUE(output.empty());
}

TEST_F(AppInputTest, SplitSettingsArrowDoesNotCloseThePanel) {
    AppTestPeer::open_settings(app);
    send("\x1b");
    EXPECT_TRUE(app.is_settings_visible());
    send("[B");
    EXPECT_TRUE(app.is_settings_visible());
    EXPECT_EQ(AppTestPeer::focus(app), 1);
    send("\x1b");
    EXPECT_TRUE(app.is_settings_visible());
    AppTestPeer::expire(app);
    EXPECT_FALSE(app.is_settings_visible());
}

TEST_F(AppInputTest, RealEventLoopResolvesSettingsEscapeAndCancelsCompletedArrowTimeout) {
    EventLoop loop;
    AppTestPeer::loop(app, &loop);
    AppTestPeer::open_settings(app);
    send("\x1b");
    EXPECT_TRUE(app.is_settings_visible());
    loop.run();
    EXPECT_FALSE(app.is_settings_visible());

    AppTestPeer::open_settings(app);
    send("\x1b");
    loop.set_timer([this]() { send("[B"); }, 10, false);
    loop.run();
    EXPECT_TRUE(app.is_settings_visible());
    EXPECT_EQ(AppTestPeer::focus(app), 1);
    AppTestPeer::loop(app, nullptr);
}

TEST_F(AppInputTest, SelectionDoesNotDependOnInputBatching) {
    send("ni1");
    ASSERT_EQ(ime->selections, std::vector<int>{0});
    EXPECT_EQ(written(), "选");
    send("ni");
    send("1");
    EXPECT_EQ(ime->selections, (std::vector<int>{0, 0}));
    EXPECT_EQ(written(), "选选");
}

TEST_F(AppInputTest, CandidateWindowResetsBeforeSelectingNewComposition) {
    AppTestPeer::cap(app, 3);
    send("ni..");  // visible slots now refer to candidate indexes 6..8
    send("a1");  // new preedit and select in the same read
    EXPECT_EQ(ime->selections, std::vector<int>{0});
}

TEST_F(AppInputTest, PreviousPageReturnsToItsLastWindow) {
    AppTestPeer::cap(app, 3);
    send("ni...");  // advance to Rime page 1
    ASSERT_EQ(ime->page, 1);
    send(",1");
    EXPECT_EQ(ime->page, 0);
    EXPECT_EQ(ime->selections, std::vector<int>{6});
}

TEST_F(AppInputTest, AltAndPrefixCombinationsRemainOpaque) {
    send("\x1bx");
    send("\x01x");
    EXPECT_TRUE(ime->preedit.empty());
    EXPECT_EQ(written(), std::string("\x1bx\x01x"));
}

TEST_F(AppInputTest, FailedImeStaysInPassthroughEvenAfterModeShortcut) {
    AppTestPeer::ready(app, false);
    send("\x01 abc");
    EXPECT_EQ(written(), "abc");
    EXPECT_TRUE(ime->preedit.empty());
}

TEST_F(AppInputTest, UnconsumedInputIsForwarded) {
    ime->accepts = false;
    send("abc");
    EXPECT_EQ(written(), "abc");
}

TEST_F(AppInputTest, PendingEscapeTimeoutDoesNotDiscardParameters) {
    ime->current_mode = ImeMode::English;
    send("\x1b[12;");
    AppTestPeer::expire(app);
    EXPECT_EQ(written(), "\x1b[12;");
    send("hello");
    EXPECT_EQ(written(), "\x1b[12;hello");
}

TEST_F(AppInputTest, PunctuationCommitsAreDrainedImmediately) {
    send("!");
    EXPECT_EQ(written(), "！");
    EXPECT_TRUE(ime->pending.empty());
}

TEST_F(AppInputTest, ReturnReachesRimeAndCtrlCReachesShell) {
    send("ni\r");
    EXPECT_EQ(written(), "ni");
    send("hao\x03");
    EXPECT_TRUE(ime->preedit.empty());
    EXPECT_EQ(written(), std::string("ni\x03"));
}

TEST_F(AppInputTest, HomeEndDeleteAreRimeEditingKeysNotSelectionDigits) {
    send("ni\x1b[H\x1b[F\x1b[3~");
    EXPECT_TRUE(ime->selections.empty());
    EXPECT_TRUE(output.empty());
    EXPECT_EQ(ime->keys, (std::vector<std::pair<int, int>>{{0xFF50, 0}, {0xFF57, 0}, {0xFFFF, 0}}));
}

TEST_F(AppInputTest, SettingsCloseSavesTheCustomSourceFile) {
    char dir[] = "/tmp/term-ime-app-config-XXXXXX";
    ASSERT_NE(mkdtemp(dir), nullptr);
    const std::string path = std::string(dir) + "/custom.json";
    AppTestPeer::save_to(app, path);
    EXPECT_EQ(AppConfig::load(path).max_candidates, 3);
    EXPECT_FALSE(AppConfig::load(path).to_json().contains("source_path"));
    std::filesystem::remove_all(dir);
}

TEST_F(AppInputTest, TerminalResponsesBypassSettingsAndChineseInput) {
    send("ni");
    AppTestPeer::terminal(app);
    AppTestPeer::open_settings(app);
    const std::string query = "\x1b[4;6H\x1b[6n\x1b[18t";
    app.on_pty_data(query.data(), query.size());
    EXPECT_EQ(written(), "\x1b[4;6R\x1b[8;23;80t");
    EXPECT_EQ(ime->preedit, "ni");
    EXPECT_TRUE(ime->selections.empty());
    EXPECT_TRUE(app.is_settings_visible());
}

TEST_F(AppInputTest, BracketedPastePreservesTextAndEmbeddedShortcuts) {
    send("ni");
    const std::string paste = "\x1b[200~hello 123 中文\x01s\x01 \x01\x03\x1b[201~";
    send(paste);
    EXPECT_EQ(written(), paste);
    EXPECT_TRUE(ime->preedit.empty());
    EXPECT_TRUE(ime->selections.empty());
    EXPECT_EQ(ime->cancels, 1);
    EXPECT_EQ(ime->current_mode, ImeMode::Chinese);
    EXPECT_FALSE(app.is_settings_visible());
    EXPECT_FALSE(app.quit_requested());
    send("ni1");
    EXPECT_EQ(written(), paste + "选");
}

TEST_F(AppInputTest, PasteDelimitersWorkAcrossEveryReadSplit) {
    const std::string paste = "\x1b[200~hello 123 中文\x01s\x1b[201~";
    for (size_t split = 1; split < paste.size(); ++split) {
        output.clear();
        send(paste.substr(0, split));
        send(paste.substr(split));
        EXPECT_EQ(written(), paste) << "split=" << split;
        EXPECT_TRUE(ime->preedit.empty());
        EXPECT_FALSE(app.is_settings_visible());
    }
}

TEST_F(AppInputTest, PartialPasteEndSurvivesEscapeTimeout) {
    send("\x1b[200~hello\x1b[20");
    AppTestPeer::expire(app);
    send("1~ni1");
    EXPECT_EQ(written(), "\x1b[200~hello\x1b[201~选");
    EXPECT_EQ(ime->selections, std::vector<int>{0});
}

TEST_F(AppInputTest, SettingsConsumePasteWithoutActivatingControlsOrLeakingToShell) {
    AppTestPeer::open_settings(app);
    send("\x1b[200~\x1b\x01s\x01 \r\x01\x03\x1b[201~");
    EXPECT_TRUE(app.is_settings_visible());
    EXPECT_EQ(AppTestPeer::focus(app), 0);
    EXPECT_TRUE(output.empty());
    EXPECT_FALSE(app.quit_requested());
    send("\x1b");
    AppTestPeer::expire(app);
    EXPECT_FALSE(app.is_settings_visible());
}

