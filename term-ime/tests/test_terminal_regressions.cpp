#include <gtest/gtest.h>
#include "terminal/output_stream.hpp"
#include "terminal/parser.hpp"
#include "ui/renderer.hpp"
#include "util/utf8.hpp"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {
void feed(Parser& parser, const std::string& bytes) {
    parser.feed(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}
std::string visible_line(const Screen& screen, int row) {
    std::string out;
    for (int col = 0; col < screen.cols(); ++col) {
        const Cell cell = screen.get(row, col);
        if (!cell.continuation) out += utf8::encode(cell.ch);
    }
    return out;
}

// Real kernel PTY: Renderer still sees isatty stdin AND stdout, unlike a pipe
// capture. Small geometry prevents the output queue from filling during tests.
class ScopedTerminal {
   public:
    int master = -1, slave = -1;
    int saved_in = -1, saved_out = -1;
    termios original{};
    ScopedTerminal() {
        winsize ws{};
        ws.ws_row = 6;
        ws.ws_col = 20;
        if (openpty(&master, &slave, nullptr, nullptr, &ws) < 0)
            throw std::runtime_error("openpty failed");
        if (tcgetattr(slave, &original) < 0)
            throw std::runtime_error("tcgetattr failed");
        original.c_iflag |= IXON | ICRNL;
        original.c_oflag |= OPOST;
        if (tcsetattr(slave, TCSANOW, &original) < 0)
            throw std::runtime_error("tcsetattr failed");
        fflush(stdout);
        saved_in = dup(STDIN_FILENO);
        saved_out = dup(STDOUT_FILENO);
        if (saved_in < 0 || saved_out < 0 || dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0)
            throw std::runtime_error("dup failed");
        fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    }
    ~ScopedTerminal() {
        fflush(stdout);
        dup2(saved_in, STDIN_FILENO);
        dup2(saved_out, STDOUT_FILENO);
        close(saved_in); close(saved_out); close(master); close(slave);
    }
    void resize(int rows, int cols) {
        winsize ws{};
        ws.ws_row = rows;
        ws.ws_col = cols;
        if (ioctl(slave, TIOCSWINSZ, &ws) < 0)
            throw std::runtime_error("TIOCSWINSZ failed");
    }
    std::string take() {
        fflush(stdout);
        std::string bytes;
        char buf[4096];
        for (;;) {
            pollfd pfd{master, POLLIN, 0};
            if (poll(&pfd, 1, 5) <= 0) break;
            const ssize_t n = read(master, buf, sizeof(buf));
            if (n <= 0) break;
            bytes.append(buf, static_cast<size_t>(n));
        }
        return bytes;
    }
};

class TerminalRendererRegression : public ::testing::Test {
   protected:
    ScopedTerminal terminal;
    Screen screen{5, 20};
    Parser parser{screen};
    Renderer renderer;
    void SetUp() override {
        renderer.attach_terminal(parser);
        renderer.init();
        ASSERT_TRUE(renderer.is_initialized());
        terminal.take();
    }
    void output(const std::string& bytes, bool suppress = false) {
        renderer.forward_output(bytes.data(), bytes.size(), suppress);
    }
};
}

TEST(TerminalStreamRegression, AllSplitPointsPreserveCompleteCsiUtf8AndOsc) {
    const std::string source = "A\x1b[31m中文\x1b]0;中文\x07Z";
    for (size_t split = 0; split <= source.size(); ++split) {
        OutputStream stream;
        std::string output;
        auto emit = [&](std::string_view token) {
            ASSERT_FALSE(token.empty());
            if (token[0] == '\x1b') {
                EXPECT_TRUE(token.back() == 'm' || token.back() == '\x07');
            } else {
                size_t pos = 0;
                while (pos < token.size()) {
                    const char32_t ch = utf8::decode(reinterpret_cast<const uint8_t*>(token.data()), token.size(), pos);
                    EXPECT_NE(ch, U'\uFFFD');
                }
            }
            output.append(token);
        };
        stream.feed(source.data(), split, emit);
        EXPECT_LE(stream.buffered_bytes(), OutputStream::kMaxControlBytes);
        stream.feed(source.data() + split, source.size() - split, emit);
        EXPECT_EQ(output, source) << "split=" << split;
        EXPECT_EQ(stream.buffered_bytes(), 0u);
    }
}

TEST(TerminalStreamRegression, EmbeddedC0ExecutesBeforeCsiFinishes) {
    OutputStream stream;
    std::vector<std::string> tokens;
    auto emit = [&](std::string_view token) { tokens.emplace_back(token); };
    const std::string partial = "\x1b[1\n";
    stream.feed(partial.data(), partial.size(), emit);
    EXPECT_EQ(tokens, (std::vector<std::string>{"\n"}));
    EXPECT_EQ(stream.buffered_bytes(), 3u);  // ESC[1 is still pending
    stream.feed("C", 1, emit);
    EXPECT_EQ(tokens, (std::vector<std::string>{"\n", "\x1b[1C"}));
    EXPECT_EQ(stream.buffered_bytes(), 0u);
}

TEST(TerminalStreamRegression, EmbeddedControlsPreserveEscapeAndIntermediateState) {
    const std::string source = std::string("\x1b\n[1\r\t\b\x07\x7f") + '\0' + "C\x1b(\nB";
    const std::string expected = std::string("\n\r\t\b\x07") + '\0' + "\x1b[1C\n\x1b(B";
    for (size_t split = 0; split <= source.size(); ++split) {
        OutputStream stream;
        std::string output;
        auto emit = [&](std::string_view token) { output.append(token); };
        stream.feed(source.data(), split, emit);
        stream.feed(source.data() + split, source.size() - split, emit);
        EXPECT_EQ(output, expected) << split;
        EXPECT_EQ(stream.buffered_bytes(), 0u);
    }
}

TEST(TerminalStreamRegression, AbortingCsiDoesNotUndoAlreadyExecutedControls) {
    for (char abort : {'\x18', '\x1a', '\x1b'}) {
        OutputStream stream;
        std::string output;
        auto emit = [&](std::string_view token) { output.append(token); };
        const std::string source = std::string("\x1b[1\n") + abort + (abort == '\x1b' ? "[2CX" : "X");
        stream.feed(source.data(), source.size(), emit);
        EXPECT_EQ(output, abort == '\x1b' ? "\n\x1b[2CX" : "\nX");
    }
}

TEST(TerminalStreamRegression, ControlStringContentsAreNotExecutedAsStandaloneC0) {
    for (const std::string& source : {std::string("\x1b]0;title\n\r\t\x07"), std::string("\x1bPdata\n\x07\x1b\\")}) {
        OutputStream stream;
        std::vector<std::string> tokens;
        auto emit = [&](std::string_view token) { tokens.emplace_back(token); };
        stream.feed(source.data(), source.size() - 1, emit);
        EXPECT_TRUE(tokens.empty());
        stream.feed(source.data() + source.size() - 1, 1, emit);
        EXPECT_EQ(tokens, (std::vector<std::string>{source}));
    }
}

TEST(TerminalStreamRegression, OversizedCsiStillExecutesEmbeddedC0BeforeItsTerminator) {
    OutputStream stream;
    std::string output;
    auto emit = [&](std::string_view token) { output.append(token); };
    const std::string source = "\x1b[" + std::string(OutputStream::kMaxControlBytes, '1');
    stream.feed(source.data(), source.size(), emit);
    EXPECT_TRUE(output.empty());
    EXPECT_EQ(stream.buffered_bytes(), 0u);
    stream.feed("\n\x7f", 2, emit);
    EXPECT_EQ(output, "\n");
    EXPECT_EQ(stream.buffered_bytes(), 0u);
    stream.feed("mOK", 3, emit);
    EXPECT_EQ(output, "\nOK");
    EXPECT_EQ(stream.buffered_bytes(), 0u);
}

TEST(TerminalParserRegression, EmbeddedC0UpdatesCursorBeforeCsiFinishes) {
    Screen screen(5, 20);
    Parser parser(screen);
    feed(parser, "\x1b[2;4H\x1b[2\n");
    EXPECT_EQ(screen.cursor_row(), 2);
    EXPECT_EQ(screen.cursor_col(), 3);
    feed(parser, "CX");
    EXPECT_EQ(screen.get(2, 5).ch, U'X');
    EXPECT_EQ(screen.cursor_row(), 2);
    EXPECT_EQ(screen.cursor_col(), 6);
}

TEST(TerminalParserRegression, EmbeddedC0UpdatesCursorWithoutLosingCsiParameters) {
    struct Case { char control; int row; int col; };
    for (const auto& test : {Case{'\n', 2, 5}, Case{'\v', 2, 5}, Case{'\f', 2, 5},
                             Case{'\r', 1, 2}, Case{'\b', 1, 4}, Case{'\t', 1, 10},
                             Case{'\x07', 1, 5}, Case{'\0', 1, 5}, Case{'\x7f', 1, 5}}) {
        const std::string source = std::string("\x1b[2;4H\x1b[2") + test.control + "CX";
        for (size_t split = 0; split <= source.size(); ++split) {
            Screen screen(5, 20);
            Parser parser(screen);
            feed(parser, source.substr(0, split));
            feed(parser, source.substr(split));
            EXPECT_EQ(screen.get(test.row, test.col).ch, U'X') << int(test.control) << ":" << split;
            EXPECT_EQ(screen.cursor_row(), test.row);
            EXPECT_EQ(screen.cursor_col(), test.col + 1);
        }
    }
}

TEST_F(TerminalRendererRegression, EmbeddedCsiNewlineAndSubsequentMoveNeverLeaveGhostText) {
    const std::string source = "\x1b[2;2H\x1b[1\nCX\x1b[1DY";
    for (bool suppress : {false, true}) {
        for (size_t split = 0; split <= source.size(); ++split) {
            output("\x1b" "c");
            terminal.take();
            Screen host(6, 20);
            Parser host_parser(host);
            output(source.substr(0, split), suppress);
            // A bar between reads must not flush or discard a pending CSI.
            if (!suppress) {
                renderer.render_candidates({}, 0, "a", "EN");
                feed(host_parser, terminal.take());
            } else {
                EXPECT_TRUE(terminal.take().empty());
            }
            output(source.substr(split), suppress);
            if (suppress) {
                EXPECT_TRUE(terminal.take().empty());
                renderer.redraw_shell(screen);
            }
            feed(host_parser, terminal.take());
            EXPECT_EQ(screen.get(2, 2).ch, U'Y') << split;
            EXPECT_EQ(host.get(2, 2).ch, U'Y') << split;
            EXPECT_EQ(host.get(1, 2).ch, U' ');
            EXPECT_EQ(screen.cursor_row(), 2);
            EXPECT_EQ(screen.cursor_col(), 3);
            EXPECT_EQ(host.cursor_row(), screen.cursor_row());
            EXPECT_EQ(host.cursor_col(), screen.cursor_col());
            for (int row = 0; row < screen.rows(); ++row)
                EXPECT_EQ(visible_line(host, row), visible_line(screen, row)) << row << ":" << split;
        }
    }
}

TEST_F(TerminalRendererRegression, EmbeddedC0QueriesAndModesStillUseChildProtocolHandling) {
    std::string replies;
    renderer.set_response_handler([&](const std::string& reply) { replies += reply; });
    output("\x1b[2;4H\x1b[6\nn\x1b[?25\rl", true);
    EXPECT_EQ(replies, "\x1b[3;4R");
    EXPECT_EQ(screen.cursor_row(), 2);
    EXPECT_EQ(screen.cursor_col(), 0);
    EXPECT_FALSE(parser.private_mode(25));
    EXPECT_TRUE(terminal.take().empty());
    renderer.redraw_shell(screen);
    const auto emitted = terminal.take();
    EXPECT_NE(emitted.find("\x1b[?25l"), std::string::npos);
    EXPECT_EQ(emitted.find("\x1b[6"), std::string::npos);
}

TEST_F(TerminalRendererRegression, EmbeddedNewlineScrollsBeforeCompletingCursorMove) {
    output("TOP\x1b[2;1HKEEP\x1b[3;1HOLD\x1b[4;1HBOTTOM\x1b[2;4r\x1b[4;2H");
    Screen host(6, 20);
    Parser host_parser(host);
    feed(host_parser, terminal.take());
    output("\x1b[1\nCX\x1b[1DY");
    feed(host_parser, terminal.take());
    EXPECT_EQ(visible_line(screen, 1).substr(0, 3), "OLD");
    EXPECT_EQ(visible_line(screen, 2).substr(0, 6), "BOTTOM");
    EXPECT_EQ(screen.get(3, 2).ch, U'Y');
    for (int row = 0; row < screen.rows(); ++row)
        EXPECT_EQ(visible_line(host, row), visible_line(screen, row));
}

TEST(TerminalStreamRegression, OversizedControlsStayBoundedAndDoNotLeakTheirBodies) {
    for (const std::string& prefix : {std::string("\x1b]"), std::string("\x1b[")}) {
        OutputStream stream;
        std::string output;
        auto emit = [&](std::string_view token) { output.append(token); };
        stream.feed(prefix.data(), prefix.size(), emit);
        const std::string chunk(1024, '1');
        for (int i = 0; i < 100; ++i) {
            stream.feed(chunk.data(), chunk.size(), emit);
            EXPECT_LE(stream.buffered_bytes(), OutputStream::kMaxControlBytes);
        }
        const std::string end = prefix.back() == ']' ? "\x07OK" : "mOK";
        stream.feed(end.data(), end.size(), emit);
        EXPECT_EQ(output, "OK");
    }
}

TEST(TerminalParserRegression, RelativeCountsDefaultsAndTabStops) {
    Screen screen(10, 20);
    Parser parser(screen);
    feed(parser, "abcdef\x1b[5DX");
    EXPECT_EQ(visible_line(screen, 0).substr(0, 6), "aXcdef");
    EXPECT_EQ(screen.cursor_col(), 2);
    feed(parser, "\x1b[4B\x1b[3C\x1b[2A\x1b[0D");
    EXPECT_EQ(screen.cursor_row(), 2);
    EXPECT_EQ(screen.cursor_col(), 4);
    feed(parser, "\x1b[;2HA\tB");
    EXPECT_EQ(screen.get(0, 8).ch, U'B');
    EXPECT_EQ(screen.cursor_col(), 9);
    feed(parser, "\x1b[20G\t");
    EXPECT_EQ(screen.cursor_col(), 19);
}

TEST(TerminalParserRegression, NonAsciiControlStringsNeverReachGridAcrossAnySplit) {
    for (const std::string& source : {std::string("\x1b]0;中文\x07X"), std::string("\x1bP中文\x1b\\X")}) {
        for (size_t split = 0; split <= source.size(); ++split) {
            Screen screen(3, 20);
            Parser parser(screen);
            feed(parser, source.substr(0, split));
            feed(parser, source.substr(split));
            EXPECT_EQ(screen.get(0, 0).ch, U'X');
            EXPECT_EQ(screen.get(0, 1).ch, U' ');
            EXPECT_EQ(screen.cursor_col(), 1);
        }
    }
}

TEST(TerminalScreenRegression, OverwriteEitherWideHalfRemovesTheOtherHalf) {
    Screen screen(3, 8);
    screen.put(U'中', 0, 0);
    ASSERT_TRUE(screen.get(0, 0).wide);
    ASSERT_TRUE(screen.get(0, 1).continuation);
    screen.put(U'X', 0, 1);
    EXPECT_EQ(screen.get(0, 0).ch, U' ');
    EXPECT_FALSE(screen.get(0, 0).wide);
    EXPECT_EQ(screen.get(0, 1).ch, U'X');
    EXPECT_FALSE(screen.get(0, 1).continuation);
    screen.put(U'文', 0, 1);
    screen.put(U'Y', 0, 1);
    EXPECT_EQ(screen.get(0, 2).ch, U' ');
    EXPECT_FALSE(screen.get(0, 2).continuation);
}

TEST(TerminalScreenRegression, ErasureAndResizeNeverLeaveHalfGlyphs) {
    Screen screen(3, 8);
    Pen red; red.bg = 1;
    screen.put(U'中', 0, 0);
    screen.move_cursor(0, 1);
    screen.erase_line(0, red);
    EXPECT_EQ(screen.get(0, 0).ch, U' ');
    EXPECT_EQ(screen.get(0, 0).bg, 1);
    EXPECT_FALSE(screen.get(0, 1).continuation);
    screen.put(U'中', 0, 0);
    screen.move_cursor(0, 0);
    screen.erase_display(1, red);
    EXPECT_EQ(screen.get(0, 1).ch, U' ');
    EXPECT_FALSE(screen.get(0, 1).continuation);
    screen.put(U'中', 0, 0);
    screen.resize(3, 1);
    EXPECT_EQ(screen.get(0, 0).ch, U' ');
    EXPECT_FALSE(screen.get(0, 0).wide);
}

TEST(TerminalParserRegression, ChildBuffersPreserveMainContentCursorAndPenAcrossResize) {
    Screen screen(3, 8);
    Parser parser(screen);
    feed(parser, "\x1b[31mMAIN\x1b[?1049hALT\x1b[?1049h");
    ASSERT_TRUE(screen.alternate_screen());
    EXPECT_EQ(visible_line(screen, 0).substr(0, 3), "ALT");  // repeated set is idempotent
    parser.resize(4, 10);
    feed(parser, "\x1b[?1049lX");
    EXPECT_FALSE(screen.alternate_screen());
    EXPECT_EQ(visible_line(screen, 0).substr(0, 5), "MAINX");
    EXPECT_EQ(screen.get(0, 4).fg, 1);
    EXPECT_EQ(screen.cursor_col(), 5);
    feed(parser, "\x1b[?47h");
    EXPECT_EQ(visible_line(screen, 0).substr(0, 3), "ALT");
    feed(parser, "\x1b[?47l\x1b[?1047h");
    EXPECT_EQ(screen.get(0, 0).ch, U' ');
    feed(parser, "\x1b[?1047l");
    EXPECT_EQ(visible_line(screen, 0).substr(0, 5), "MAINX");
}

TEST(TerminalParserRegression, ScrollMarginsAndSavedCursorRemainUsable) {
    Screen screen(4, 8);
    Parser parser(screen);
    feed(parser, "TOP\x1b[4;1HBOTTOM\x1b[2;3r\x1b[3;1HA\r\nB");
    EXPECT_EQ(screen.get(0, 0).ch, U'T');
    EXPECT_EQ(screen.get(1, 0).ch, U'A');
    EXPECT_EQ(screen.get(2, 0).ch, U'B');
    EXPECT_EQ(screen.get(3, 0).ch, U'B');
    feed(parser, "\x1b" "7\x1b[1;1H\x1b" "8X");
    EXPECT_EQ(screen.get(2, 1).ch, U'X');
}

TEST_F(TerminalRendererRegression, UiNeverInterruptsPartialCsiOrUtf8) {
    output("\x1b[31");
    EXPECT_TRUE(terminal.take().empty());
    renderer.render_candidates({}, 0, "a", "EN");
    const auto bar = terminal.take();
    EXPECT_EQ(bar.find("\x1b[31"), std::string::npos);
    output("mX");
    EXPECT_EQ(terminal.take(), "\x1b[31mX");
    EXPECT_EQ(screen.get(0, 0).ch, U'X');
    output(std::string("\xe4\xb8", 2));
    EXPECT_TRUE(terminal.take().empty());
    renderer.render_candidates({}, 0, "ab", "EN");
    terminal.take();
    output(std::string("\xadY", 2));
    EXPECT_EQ(terminal.take(), "中Y");
    EXPECT_EQ(screen.get(0, 1).ch, U'中');
}

TEST_F(TerminalRendererRegression, ChildAltSwitchesAreTranslatedAndMainContentIsRepainted) {
    output("MAIN\x1b[?1049;25hALT");
    auto emitted = terminal.take();
    EXPECT_EQ(emitted.find("\x1b[?1049h"), std::string::npos);
    EXPECT_EQ(emitted.find("\x1b[?1049;25h"), std::string::npos);
    EXPECT_NE(emitted.find("\x1b[?25h"), std::string::npos);
    ASSERT_TRUE(screen.alternate_screen());
    output("\x1b[?1049l");
    emitted = terminal.take();
    EXPECT_EQ(emitted.find("\x1b[?1049l"), std::string::npos);
    EXPECT_NE(emitted.find("MAIN"), std::string::npos);
    EXPECT_FALSE(screen.alternate_screen());
    EXPECT_EQ(screen.cursor_col(), 4);
}

TEST_F(TerminalRendererRegression, SuppressedOutputStillUpdatesStreamAndBothChildBuffers) {
    output("MAIN"); terminal.take();
    output("\x1b[?104", true);
    output("9h中文", true);
    output("\x1b[?1049l\x1b[31", true);
    output("mX", true);
    EXPECT_TRUE(terminal.take().empty());
    EXPECT_FALSE(screen.alternate_screen());
    EXPECT_EQ(visible_line(screen, 0).substr(0, 5), "MAINX");
    EXPECT_EQ(screen.get(0, 4).fg, 1);
    renderer.redraw_shell(screen);
    EXPECT_NE(terminal.take().find("MAIN"), std::string::npos);
}

TEST_F(TerminalRendererRegression, RedrawSkipsContinuationAndEndsAtModelCursorNotPanelCursor) {
    feed(parser, "中ABC\x1b[3;4H");
    // Change only the host cursor, as a settings panel does.
    printf("\x1b[5;10H"); terminal.take();
    renderer.redraw_shell(screen);
    const std::string emitted = terminal.take();
    EXPECT_NE(emitted.find("中ABC"), std::string::npos);
    EXPECT_EQ(emitted.find("中 ABC"), std::string::npos);
    EXPECT_EQ(emitted.find("\x1b[s"), std::string::npos);
    EXPECT_EQ(emitted.find("\x1b[u"), std::string::npos);
    const auto cursor = emitted.rfind("\x1b[3;4H");
    ASSERT_NE(cursor, std::string::npos);
    // After final CUP only the active rendition is restored, never an old cursor.
    EXPECT_EQ(emitted.substr(cursor + 6), "\x1b[0;37;40m");
}

TEST_F(TerminalRendererRegression, BarDoesNotOverwriteChildCursorSaveOrLoseMarginWrap) {
    output("\x1b[2;3H\x1b" "7\x1b[1;20HX"); terminal.take();
    ASSERT_TRUE(parser.wrap_pending());
    renderer.render_candidates({}, 0, "a", "EN");
    const auto emitted = terminal.take();
    EXPECT_EQ(emitted.find("\x1b[s"), std::string::npos);
    EXPECT_EQ(emitted.find("\x1b" "7"), std::string::npos);
    EXPECT_NE(emitted.find("\x1b[1;20H"), std::string::npos);
    EXPECT_NE(emitted.find('X'), std::string::npos);
    output("\x1b" "8Y");
    EXPECT_EQ(screen.get(1, 2).ch, U'Y');
}

TEST_F(TerminalRendererRegression, ChildCursorSaveSlotsAreIndependentAcrossBuffers) {
    output("\x1b[2;3H\x1b" "7\x1b[?1049h\x1b[4;5H\x1b" "7\x1b[?1049l");
    terminal.take();
    output("\x1b" "8");
    const auto emitted = terminal.take();
    EXPECT_EQ(screen.cursor_row(), 1);
    EXPECT_EQ(screen.cursor_col(), 2);
    EXPECT_EQ(emitted.find("\x1b" "8"), std::string::npos);
    EXPECT_NE(emitted.find("\x1b[2;3H"), std::string::npos);
}

TEST_F(TerminalRendererRegression, FullWidthChineseBottomRowDoesNotEmitAnExtraCell) {
    feed(parser, "\x1b[5;1H中ABCDEFGHIJKLMNOPQR");
    ASSERT_TRUE(parser.wrap_pending());
    renderer.redraw_shell(screen);
    const auto emitted = terminal.take();
    EXPECT_NE(emitted.find("中ABCDEFGHIJKLMNOPQR"), std::string::npos);
    EXPECT_EQ(emitted.find("中 ABCDEFGHIJKLMNOPQR"), std::string::npos);
    EXPECT_NE(emitted.find("\x1b[?7l"), std::string::npos);
}

TEST_F(TerminalRendererRegression, RawModePreservesControlBytesAndRestoresOriginalSettings) {
    termios active{};
    ASSERT_EQ(tcgetattr(STDIN_FILENO, &active), 0);
    EXPECT_EQ(active.c_iflag & (IXON | ICRNL | INLCR | IGNCR | ISTRIP), 0u);
    EXPECT_EQ(active.c_oflag & OPOST, 0u);
    EXPECT_EQ(active.c_lflag & (ICANON | ECHO | ISIG | IEXTEN), 0u);
    const std::string sent("\r\x13\x11\x03", 4);
    ASSERT_EQ(write(terminal.master, sent.data(), sent.size()), static_cast<ssize_t>(sent.size()));
    std::string received;
    for (int tries = 0; tries < 4 && received.size() < sent.size(); ++tries) {
        pollfd fd{STDIN_FILENO, POLLIN, 0};
        ASSERT_GT(poll(&fd, 1, 100), 0);
        char buf[4];
        const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        ASSERT_GT(n, 0);
        received.append(buf, static_cast<size_t>(n));
    }
    EXPECT_EQ(received, sent);
    renderer.restore();
    ASSERT_EQ(tcgetattr(STDIN_FILENO, &active), 0);
    EXPECT_EQ(active.c_iflag, terminal.original.c_iflag);
    EXPECT_EQ(active.c_oflag, terminal.original.c_oflag);
    EXPECT_EQ(active.c_lflag, terminal.original.c_lflag);
}

TEST(TerminalRendererInitializationRegression, RedirectedStdoutFailsWithoutChangingTermios) {
    ScopedTerminal terminal;
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    const int saved_out = dup(STDOUT_FILENO);
    ASSERT_GE(saved_out, 0);
    ASSERT_GE(dup2(pipefd[1], STDOUT_FILENO), 0);
    close(pipefd[1]);
    Renderer renderer;
    renderer.init();
    EXPECT_FALSE(renderer.is_initialized());
    termios after{};
    EXPECT_EQ(tcgetattr(STDIN_FILENO, &after), 0);
    EXPECT_EQ(after.c_iflag, terminal.original.c_iflag);
    EXPECT_EQ(after.c_lflag, terminal.original.c_lflag);
    fflush(stdout);
    dup2(saved_out, STDOUT_FILENO);
    close(saved_out);
    std::string messages;
    char bytes[256];
    for (ssize_t n; (n = read(pipefd[0], bytes, sizeof(bytes))) > 0;)
        messages.append(bytes, static_cast<size_t>(n));
    EXPECT_EQ(messages.find('\x1b'), std::string::npos);
    close(pipefd[0]);
}

TEST_F(TerminalRendererRegression, OverlayRestoresBothDirectionsOfPrivateModeChanges) {
    Screen host(6, 20);
    Parser host_parser(host);
    output("\x1b[?25l\x1b[?1;1000;1006;2004h");
    feed(host_parser, terminal.take());
    ASSERT_FALSE(host_parser.private_mode(25));
    ASSERT_TRUE(host_parser.private_mode(2004));
    ui::SettingsState panel;
    renderer.render_settings(panel);
    feed(host_parser, terminal.take());
    EXPECT_TRUE(host_parser.private_mode(25));
    EXPECT_FALSE(host_parser.private_mode(1));
    EXPECT_FALSE(host_parser.private_mode(1000));
    EXPECT_FALSE(host_parser.private_mode(2004));
    // Suppression must track changes even when a control crosses read/overlay
    // boundaries. Showing the cursor while a running tool exits is not paint.
    output("\x1b[?25", true);
    output("h\x1b[?1;1000;1006;2004l", true);
    EXPECT_TRUE(terminal.take().empty());
    renderer.redraw_shell(screen);
    feed(host_parser, terminal.take());
    EXPECT_TRUE(host_parser.private_mode(25));
    for (int mode : {1, 1000, 1006, 2004})
        EXPECT_FALSE(host_parser.private_mode(mode)) << mode;
    output("\x1b[?25l\x1b[?1;1000;1006;2004h", true);
    EXPECT_TRUE(terminal.take().empty());
    renderer.redraw_shell(screen);
    feed(host_parser, terminal.take());
    EXPECT_FALSE(host_parser.private_mode(25));
    for (int mode : {1, 1000, 1006, 2004})
        EXPECT_TRUE(host_parser.private_mode(mode)) << mode;
    renderer.render_candidates({}, 0, "a", "EN");
    feed(host_parser, terminal.take());
    EXPECT_FALSE(host_parser.private_mode(25));  // the bar must not unhide it
    EXPECT_TRUE(host_parser.private_mode(2004));
}

TEST_F(TerminalRendererRegression, OverlayKeepsLatestMouseTrackingSelection) {
    output("\x1b[?1003h\x1b[?1000h", true);
    EXPECT_FALSE(parser.private_mode(1003));
    EXPECT_TRUE(parser.private_mode(1000));
    EXPECT_TRUE(terminal.take().empty());
    renderer.redraw_shell(screen);
    const auto wire = terminal.take();
    EXPECT_LT(wire.find("\x1b[?1003l"), wire.find("\x1b[?1000h"));
    EXPECT_EQ(wire.find("\x1b[?1003h"), std::string::npos);
}

TEST_F(TerminalRendererRegression, RestoreAndRisResetAllTrackedHostModes) {
    for (int mode : Parser::kForwardedPrivateModes)
        output("\x1b[?" + std::to_string(mode) + (mode == 25 ? "l" : "h"));
    terminal.take();
    output("\x1b" "c", true);
    EXPECT_TRUE(terminal.take().empty());
    for (int mode : Parser::kForwardedPrivateModes)
        EXPECT_EQ(parser.private_mode(mode), mode == 25) << mode;
    renderer.redraw_shell(screen);
    auto reset = terminal.take();
    for (int mode : Parser::kForwardedPrivateModes)
        EXPECT_NE(reset.find("\x1b[?" + std::to_string(mode) + (mode == 25 ? "h" : "l")), std::string::npos);
    output("\x1b[?25l\x1b[?1;9;1000;1002;1003;1004;1005;1006;1015;1016;2004h");
    terminal.take();
    renderer.restore();
    reset = terminal.take();
    for (int mode : Parser::kForwardedPrivateModes)
        EXPECT_NE(reset.find("\x1b[?" + std::to_string(mode) + (mode == 25 ? "h" : "l")), std::string::npos);
}

TEST_F(TerminalRendererRegression, QueriesReplyToChildWithOrWithoutOverlayAndNeverReachHost) {
    std::vector<std::string> replies;
    renderer.set_response_handler([&](const std::string& response) { replies.push_back(response); });
    output("\x1b[3;4H"); terminal.take();
    for (bool suppress : {false, true}) {
        replies.clear();
        output("\x1b[6n\x1b[c\x1b[18t\x1b[5n", suppress);
        EXPECT_EQ(replies, (std::vector<std::string>{"\x1b[3;4R", "\x1b[?1;0c", "\x1b[8;5;20t", "\x1b[0n"}));
        EXPECT_TRUE(terminal.take().empty());
    }
    replies.clear();
    output("\x1b[>0c\x1b[999n\x1b[19t\x1b[8;24;80t");
    EXPECT_TRUE(replies.empty());  // no unsupported capabilities/window resize
    EXPECT_TRUE(terminal.take().empty());
}

TEST_F(TerminalRendererRegression, SuppressedQueriesWorkAtEveryReadSplit) {
    std::string replies;
    renderer.set_response_handler([&](const std::string& response) { replies += response; });
    const std::string source = "\x1b[3;4H\x1b[6n\x1b[0c\x1b[18t";
    for (size_t split = 0; split <= source.size(); ++split) {
        output("\x1b" "c", true);
        replies.clear();
        output(source.substr(0, split), true);
        output(source.substr(split), true);
        EXPECT_EQ(replies, "\x1b[3;4R\x1b[?1;0c\x1b[8;5;20t") << split;
        EXPECT_TRUE(terminal.take().empty());
    }
}

TEST_F(TerminalRendererRegression, QueryCoordinatesRespectOriginBanksAndResizedChildGeometry) {
    std::string replies;
    renderer.set_response_handler([&](const std::string& response) { replies += response; });
    output("\x1b[2;4r\x1b[?6h\x1b[2;7H\x1b[6n\x1b[?6n", true);
    EXPECT_EQ(replies, "\x1b[2;7R\x1b[?2;7R");
    replies.clear();
    output("\x1b[?1049h\x1b[6n\x1b[?1049l\x1b[6n", true);
    EXPECT_EQ(replies, "\x1b[1;1R\x1b[2;7R");
    replies.clear();
    parser.resize(7, 24);
    output("\x1b[18t", true);
    EXPECT_EQ(replies, "\x1b[8;7;24t");
    EXPECT_TRUE(terminal.take().empty());
}

TEST_F(TerminalRendererRegression, QueryDoesNotCancelDeferredWrapOrUseOverlayCursor) {
    output("\x1b[5;20HX"); terminal.take();
    ASSERT_TRUE(parser.wrap_pending());
    ui::SettingsState panel;
    renderer.render_settings(panel); terminal.take();
    std::string reply;
    renderer.set_response_handler([&](const std::string& response) { reply = response; });
    output("\x1b[6n", true);
    EXPECT_EQ(reply, "\x1b[5;20R");
    EXPECT_TRUE(parser.wrap_pending());
    EXPECT_TRUE(terminal.take().empty());
}

TEST(TerminalParserRegression, ResizeConvertsFullLineToInsertionPointWithoutReflow) {
    for (const std::string& text : {std::string("ABCDE"), std::string("ABC中")}) {
        Screen screen(3, 5);
        Parser parser(screen);
        feed(parser, text);
        ASSERT_TRUE(parser.wrap_pending());
        parser.resize(3, 8);
        EXPECT_FALSE(parser.wrap_pending());
        EXPECT_EQ(screen.cursor_col(), 5);
        feed(parser, "F");
        EXPECT_EQ(visible_line(screen, 0), text + "F  ");
        EXPECT_EQ(screen.cursor_row(), 0);
        EXPECT_EQ(screen.cursor_col(), 6);
        EXPECT_EQ(screen.get(1, 0).ch, U' ');
    }
}

TEST(TerminalParserRegression, ResizeUpdatesParkedMainAndSavedInsertionPoints) {
    Screen screen(3, 5);
    Parser parser(screen);
    feed(parser, "ABCDE\x1b" "7\x1b[?1049h12345\x1b" "7");
    parser.resize(3, 8);
    feed(parser, "\x1b" "8X");
    EXPECT_EQ(visible_line(screen, 0), "12345X  ");
    feed(parser, "\x1b[?1049lF");
    EXPECT_EQ(visible_line(screen, 0), "ABCDEF  ");
    feed(parser, "\x1b" "8G");
    EXPECT_EQ(visible_line(screen, 0), "ABCDEG  ");
}

TEST(TerminalParserRegression, ResizeKeepsPendingWrapWhenWidthUnchangedOrReduced) {
    for (int cols : {5, 3}) {
        Screen screen(3, 5);
        Parser parser(screen);
        feed(parser, "ABCDE");
        parser.resize(4, cols);
        EXPECT_TRUE(parser.wrap_pending());
        EXPECT_EQ(screen.cursor_col(), cols - 1);
        feed(parser, "F");
        EXPECT_EQ(screen.get(1, 0).ch, U'F');
        EXPECT_EQ(screen.cursor_row(), 1);
    }
}

TEST_F(TerminalRendererRegression, ResizeAtFullLineProducesIdenticalHostAndModelContent) {
    terminal.resize(6, 5);
    parser.resize(5, 5);
    renderer.redraw_shell(screen); terminal.take();
    output("ABCDE"); terminal.take();
    ASSERT_TRUE(parser.wrap_pending());
    terminal.resize(6, 8);
    parser.resize(5, 8);
    renderer.update_scroll_region();
    renderer.redraw_shell(screen);
    Screen host(6, 8);
    Parser host_parser(host);
    feed(host_parser, terminal.take());
    output("F");
    feed(host_parser, terminal.take());
    EXPECT_EQ(visible_line(screen, 0), "ABCDEF  ");
    EXPECT_EQ(visible_line(host, 0), "ABCDEF  ");
    EXPECT_EQ(host.cursor_row(), screen.cursor_row());
    EXPECT_EQ(host.cursor_col(), screen.cursor_col());
    EXPECT_EQ(host.get(1, 0).ch, U' ');
}

TEST_F(TerminalRendererRegression, OversizedAbsoluteMovesStayInChildAndDoNotLoseTextToBar) {
    Screen host(6, 20);
    Parser host_parser(host);
    renderer.redraw_shell(screen);
    renderer.render_candidates({}, 0, "", "EN");
    feed(host_parser, terminal.take());
    const auto bar = visible_line(host, 5);
    for (const std::string& move : {std::string("\x1b[999;1H"), std::string("\x1b[999;1f"), std::string("\x1b[999d\x1b[1G")}) {
        output(move + "LAST");
        const auto emitted = terminal.take();
        EXPECT_EQ(emitted.find("999"), std::string::npos);
        feed(host_parser, emitted);
        EXPECT_EQ(visible_line(screen, 4).substr(0, 4), "LAST");
        EXPECT_EQ(visible_line(host, 4).substr(0, 4), "LAST");
        EXPECT_EQ(visible_line(host, 5), bar);
        renderer.render_candidates({}, 0, "", "EN");
        feed(host_parser, terminal.take());
        EXPECT_EQ(visible_line(host, 4).substr(0, 4), "LAST");
    }
}

TEST_F(TerminalRendererRegression, RelativeAndTabMovesUseChildCoordinatesAndClearPendingWrap) {
    Screen host(6, 20);
    Parser host_parser(host);
    renderer.redraw_shell(screen); feed(host_parser, terminal.take());
    for (const std::string& move : {std::string("\x1b[999B"), std::string("\x1b[999e"), std::string("\x1b[999E")}) {
        output("\x1b[H" + move + "X");
        feed(host_parser, terminal.take());
        EXPECT_EQ(host.cursor_row(), 4);
        EXPECT_EQ(host.get(4, 0).ch, U'X');
        EXPECT_EQ(host.get(5, 0).ch, U' ');
    }
    output("\x1b[1;20HX"); feed(host_parser, terminal.take());
    ASSERT_TRUE(parser.wrap_pending());
    output("\x1b[999CX"); feed(host_parser, terminal.take());
    EXPECT_EQ(host.cursor_row(), 0);  // movement cancels the pending wrap
    EXPECT_TRUE(host_parser.wrap_pending());
    output("\x1b[999IX\x1b[999ZX"); feed(host_parser, terminal.take());
    EXPECT_EQ(host.cursor_row(), screen.cursor_row());
    EXPECT_EQ(host.cursor_col(), screen.cursor_col());
    EXPECT_EQ(visible_line(host, 0), visible_line(screen, 0));
}

TEST_F(TerminalRendererRegression, CanonicalPositioningPreservesOriginAndChildMargins) {
    Screen host(6, 20);
    Parser host_parser(host);
    renderer.redraw_shell(screen); feed(host_parser, terminal.take());
    output("\x1b[2;4r\x1b[?6h\x1b[999;1HLAST");
    feed(host_parser, terminal.take());
    EXPECT_EQ(host.cursor_row(), 3);
    EXPECT_EQ(visible_line(host, 3).substr(0, 4), "LAST");
    EXPECT_EQ(host.get(5, 0).ch, U' ');
    output("\x1b[1;1HX"); feed(host_parser, terminal.take());
    EXPECT_EQ(host.cursor_row(), 1);
    EXPECT_EQ(host.get(1, 0).ch, U'X');
    output("\x1b[999AY"); feed(host_parser, terminal.take());
    EXPECT_EQ(host.cursor_row(), screen.cursor_row());
    EXPECT_EQ(host.cursor_col(), screen.cursor_col());
}

TEST_F(TerminalRendererRegression, OneRowChildNeverForwardsWrapIntoStatusRow) {
    terminal.resize(2, 5);
    parser.resize(1, 5);
    renderer.redraw_shell(screen); terminal.take();
    Screen host(2, 5);
    Parser host_parser(host);
    feed(host_parser, "\x1b[2;1HSTATUS");
    output("ABCDE\r\nFG");
    const auto emitted = terminal.take();
    // Raw forwarding would run the LF/wrap over a two-row physical terminal.
    EXPECT_EQ(emitted.find("ABCDE\r\nFG"), std::string::npos);
    feed(host_parser, emitted);
    EXPECT_EQ(visible_line(host, 0), visible_line(screen, 0));
    EXPECT_EQ(visible_line(host, 0), "FG   ");
    EXPECT_EQ(host.cursor_row(), 0);
}
