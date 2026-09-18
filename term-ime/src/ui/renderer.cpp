#include "renderer.hpp"
#include "../util/utf8.hpp"
#include "jsx.hpp"
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <spdlog/spdlog.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>

namespace {
int set_termios(int fd, const termios* settings) {
    int rc;
    do { rc = tcsetattr(fd, TCSAFLUSH, settings); } while (rc < 0 && errno == EINTR);
    return rc;
}
winsize geometry(int fd) {
    winsize ws{};
    if (ioctl(fd, TIOCGWINSZ, &ws) < 0 || ws.ws_row < 2 || ws.ws_col == 0) {
        ws.ws_row = 24;
        ws.ws_col = 80;
    }
    return ws;
}
std::string sgr(const Pen& pen) {
    // Each selection is complete: notably, leaving reverse video must reset 7.
    std::string s = "\x1b[0";
    if (pen.reverse)
        s += ";7";
    s += ';' + std::to_string((pen.bright ? 90 : 30) + pen.fg);
    s += ';' + std::to_string((pen.bg_bright ? 100 : 40) + pen.bg);
    return s + 'm';
}
// Only normalize the CSI forms the model understands. Prefixes/intermediates
// belong to different commands and must not accidentally become cursor moves.
bool plain_csi(std::string_view token) {
    return token.size() >= 3 && token.substr(0, 2) == "\x1b[" &&
           token.find_first_not_of("0123456789;", 2) == token.size() - 1;
}
Pen cell_pen(const Cell& c) { return {c.fg, c.bg, c.bright, c.bg_bright, c.reverse}; }
std::string glyph(const Cell& c) {
    std::string text = c.ch ? utf8::encode(c.ch) : " ";
    for (char32_t mark : c.combining)
        text += utf8::encode(mark);
    return text;
}
}

Renderer::Renderer() = default;
Renderer::~Renderer() { restore(); }

void Renderer::init() {
    if (initialized_)
        return;
    tty_fd_ = STDIN_FILENO;
    // Validate both endpoints before changing the terminal or printing escapes.
    if (!isatty(tty_fd_) || !isatty(STDOUT_FILENO)) {
        spdlog::warn("Renderer requires terminal stdin and stdout");
        return;
    }
    auto saved = std::make_unique<termios>();
    if (tcgetattr(tty_fd_, saved.get()) < 0) {
        spdlog::warn("Cannot read terminal settings: {}", errno);
        return;
    }
    termios raw = *saved;
    cfmakeraw(&raw);  // Includes IXON, ICRNL, OPOST, and eight-bit input.
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    const auto ws = geometry(tty_fd_);
    if (!parser_) {
        startup_screen_ = std::make_unique<Screen>(ws.ws_row - 1, ws.ws_col);
        startup_parser_ = std::make_unique<Parser>(*startup_screen_);
        parser_ = startup_parser_.get();
    }
    if (set_termios(tty_fd_, &raw) < 0) {
        spdlog::warn("Cannot enter raw terminal mode: {}", errno);
        return;
    }
    saved_termios_ = saved.release();
    initialized_ = true;  // restore() also rolls back a failed initial write.
    const std::string start = "\x1b[?1049h\x1b[?6l\x1b[0m\x1b[2J\x1b[1;" +
                              std::to_string(ws.ws_row - 1) + "r\x1b[H";
    if (fwrite(start.data(), 1, start.size(), stdout) != start.size()) {
        spdlog::warn("Cannot initialize terminal display: {}", errno);
        restore();
        return;
    }
    restore_private_modes();
    if (fflush(stdout) != 0) {
        spdlog::warn("Cannot initialize terminal display: {}", errno);
        restore();
    }
}

void Renderer::restore() {
    if (!initialized_)
        return;
    // Only this owner may leave the host alternate screen. Do not clear the
    // main screen; even an output error must not prevent termios restoration.
    printf("\x1b[0m\x1b[?6l\x1b[?7h\x1b[r\x1b[?1049l");
    // Child mouse/focus/paste/application-cursor modes must not leak back into
    // the outer shell. This path never dereferences the (possibly gone) Parser.
    restore_private_modes(true);
    fflush(stdout);
    if (saved_termios_) {
        if (set_termios(tty_fd_, saved_termios_) < 0)
            spdlog::warn("Cannot restore terminal settings: {}", errno);
        delete saved_termios_;
        saved_termios_ = nullptr;
    }
    initialized_ = false;
}

void Renderer::update_scroll_region() {
    if (!initialized_)
        return;
    if (parser_)
        restore_shell_state(parser_->screen());
    else {
        const auto ws = geometry(tty_fd_);
        printf("\x1b[1;%dr", ws.ws_row - 1);
    }
    last_bar_sig_.clear();
    bar_skip_count_ = 0;
    fflush(stdout);
}
void Renderer::render(const Screen&) { /* PTY output is forwarded incrementally. */ }

void Renderer::forward_output(const char* data, size_t len, bool suppress) {
    output_stream_.feed(data, len, [this, suppress](std::string_view token) { forward_token(token, suppress); });
    if (!suppress)
        fflush(stdout);
}

void Renderer::restore_private_modes(bool defaults) {
    // Reset disabled selections first. Some hosts implement mouse tracking as
    // one shared selector, so a later DECRST could otherwise undo the active
    // selection emitted earlier in the list.
    for (bool enabling : {false, true}) {
        for (int mode : Parser::kForwardedPrivateModes) {
            const bool enabled = !defaults && parser_ ? parser_->private_mode(mode) : mode == 25;
            if (enabled == enabling)
                printf("\x1b[?%d%c", mode, enabled ? 'h' : 'l');
        }
    }
}

bool Renderer::answer_query(std::string_view token) {
    if (!parser_ || token.size() < 3 || token.substr(0, 2) != "\x1b[")
        return false;
    const char final = token.back();
    if (final != 'n' && final != 'c' && final != 't')
        return false;
    auto fields = token.substr(2, token.size() - 3);
    char prefix = 0;
    if (!fields.empty() && (fields.front() == '?' || fields.front() == '>' || fields.front() == '=')) {
        prefix = fields.front();
        fields.remove_prefix(1);
    }
    if (fields.find_first_not_of("0123456789;") != std::string_view::npos)
        return false;
    int request = 0;
    if (!fields.empty()) {
        const auto parsed = std::from_chars(fields.data(), fields.data() + fields.size(), request);
        if (parsed.ec != std::errc{} || parsed.ptr != fields.data() + fields.size())
            request = -1;
    }
    const auto& screen = parser_->screen();
    std::string response;
    if (final == 'n' && request == 6 && (prefix == 0 || prefix == '?')) {
        const int row = screen.cursor_row() - (parser_->origin_mode() ? parser_->scroll_top() : 0) + 1;
        response = prefix == '?' ? "\x1b[?" : "\x1b[";
        response += std::to_string(std::max(1, row)) + ';' + std::to_string(screen.cursor_col() + 1) + 'R';
    } else if (final == 'n' && prefix == 0 && request == 5) {
        response = "\x1b[0n";
    } else if (final == 'c' && prefix == 0 && request == 0) {
        // VT100 without optional capabilities; do not advertise the host's
        // xterm extensions, graphics protocols, or extra screen row.
        response = "\x1b[?1;0c";
    } else if (final == 't' && prefix == 0 && request == 18) {
        response = "\x1b[8;" + std::to_string(screen.rows()) + ';' + std::to_string(screen.cols()) + 't';
    }
    if (!response.empty() && response_handler_)
        response_handler_(response);
    // Unsupported queries/window operations in these families are consumed,
    // not delegated to the host (whose responses would enter keyboard/IME UI).
    return true;
}

void Renderer::forward_token(std::string_view token, bool suppress) {
    const bool was_alternate = parser_ && parser_->screen().alternate_screen();
    if (parser_)
        parser_->feed(reinterpret_cast<const uint8_t*>(token.data()), token.size());
    // Protocol replies cannot wait for an overlay to close. Reply from the
    // child model rather than the settings cursor or physical host geometry.
    if (answer_query(token))
        return;
    if (suppress)
        return;

    std::string filtered;
    bool intercepted = false;
    bool restore_cursor_mode = false;
    // Private-mode lists can combine e.g. 1049 and 25. Remove only buffer
    // switches and preserve every other mode in the same control sequence.
    if (token.size() >= 5 && token.substr(0, 3) == "\x1b[?" && (token.back() == 'h' || token.back() == 'l')) {
        std::string_view fields = token.substr(3, token.size() - 4);
        do {
            const size_t sep = fields.find(';');
            const auto field = fields.substr(0, sep);
            int mode = 0;
            const auto result = std::from_chars(field.data(), field.data() + field.size(), mode);
            const bool buffer_mode = result.ec == std::errc{} && result.ptr == field.data() + field.size() &&
                                     (mode == 47 || mode == 1047 || mode == 1048 || mode == 1049);
            if (buffer_mode) {
                intercepted = true;
                restore_cursor_mode = restore_cursor_mode || (mode == 1048 && token.back() == 'l');
            } else {
                if (!filtered.empty()) filtered += ';';
                filtered.append(field);
            }
            if (sep == std::string_view::npos) break;
            fields.remove_prefix(sep + 1);
        } while (true);
        if (intercepted && !filtered.empty())
            filtered = "\x1b[?" + filtered + token.back();
    }
    const bool reset = token == "\x1b" "c";
    const bool changed_bank = parser_ && was_alternate != parser_->screen().alternate_screen();
    if ((changed_bank || reset) && parser_) {
        redraw_shell(parser_->screen());
        bar_dirty_ = true;
    }
    if (reset) {
        // RIS belongs to the child terminal, not to the host screen owner.
        printf("\x1b[?25h");
        return;
    }
    const bool csi = plain_csi(token);
    if (parser_ && (restore_cursor_mode || token == "\x1b" "8" || (csi && token.back() == 'u'))) {
        restore_shell_state(parser_->screen());
        if (!intercepted)
            return;
    }
    if (parser_ && (token == "\x1b" "7" || (csi && token.back() == 's')))
        return;  // Child save slots live in Parser, one per child screen.
    if (intercepted) {
        if (!filtered.empty())
            fwrite(filtered.data(), 1, filtered.size(), stdout);
        return;
    }
    // Every supported CSI cursor operation is canonicalized from the model.
    // DECSTBM alone does not confine absolute addressing when origin is off:
    // forwarding CUP 999;1 would otherwise write into the host's status row.
    // This also applies child clamps to relative movement, tabs, and VPA/HPA.
    if (parser_ && csi && std::string_view("HfABCDEFaGe`dIZr").find(token.back()) != std::string_view::npos) {
        restore_shell_state(parser_->screen());
        return;
    }
    // A one-row child has no representable DECSTBM on a larger host (top must
    // be less than bottom). Paint its model instead of allowing raw wrapping
    // or scrolling to reach the status row.
    if (parser_ && parser_->screen().rows() == 1) {
        redraw_shell(parser_->screen());
        bar_dirty_ = true;
        return;
    }
    fwrite(token.data(), 1, token.size(), stdout);
    // Complete control tokens only: this works even when CSI was split across
    // PTY reads, and has no unbounded numeric parsing/overflow.
    if (token.size() >= 3 && token.substr(0, 2) == "\x1b[") {
        const char final = token.back();
        if (final == 'J' || final == 'K' || final == 'H' || final == 'f' || final == 'd')
            bar_dirty_ = true;
    }
}

void Renderer::restore_shell_state(const Screen& screen) {
    const bool attached = parser_ && &parser_->screen() == &screen;
    const auto ws = geometry(tty_fd_);
    const int rows = std::max(1, std::min(screen.rows(), static_cast<int>(ws.ws_row) - 1));
    const int cols = std::max(1, std::min(screen.cols(), static_cast<int>(ws.ws_col)));
    const int top = attached ? std::min(parser_->scroll_top(), rows - 1) : 0;
    const int bottom = attached ? std::min(parser_->scroll_bottom(), rows - 1) : rows - 1;
    const bool origin = attached && parser_->origin_mode();
    restore_private_modes(!attached);
    printf("\x1b[?6l\x1b[%d;%dr\x1b[?7%c", top + 1, bottom + 1,
           !attached || parser_->autowrap() ? 'h' : 'l');
    if (origin)
        printf("\x1b[?6h");
    const int row = std::clamp(screen.cursor_row(), 0, rows - 1);
    int col = std::clamp(screen.cursor_col(), 0, cols - 1);
    const int addressed_row = row - (origin ? top : 0) + 1;
    printf("\x1b[%d;%dH", addressed_row, col + 1);
    // Cursor addressing cancels deferred wrap. Reprinting the existing margin
    // glyph reconstructs it without scrolling or changing the logical model.
    if (attached && parser_->wrap_pending() && col == cols - 1) {
        if (screen.get(row, col).continuation)
            --col;
        const Cell cell = screen.get(row, col);
        const std::string text = sgr(cell_pen(cell)) + glyph(cell);
        printf("\x1b[%d;%dH", addressed_row, col + 1);
        fwrite(text.data(), 1, text.size(), stdout);
    }
    const auto pen = sgr(attached ? parser_->pen() : Pen{});
    fwrite(pen.data(), 1, pen.size(), stdout);
}

void Renderer::redraw_shell(const Screen& screen) {
    if (!initialized_)
        return;
    const auto ws = geometry(tty_fd_);
    const int rows = std::min(screen.rows(), static_cast<int>(ws.ws_row) - 1);
    const int cols = std::min(screen.cols(), static_cast<int>(ws.ws_col));
    // Repaint independent of child origin/scroll/wrap modes. Never save and
    // restore the settings panel's cursor, or overwrite the child's save slot.
    printf("\x1b[?6l\x1b[?7l\x1b[0m\x1b[2J");
    last_bar_sig_.clear();
    bar_skip_count_ = 0;
    for (int r = 0; r < rows; ++r) {
        std::string line = "\x1b[" + std::to_string(r + 1) + ";1H\x1b[0m";
        std::string current_sgr;
        for (int c = 0; c < cols; ++c) {
            const Cell cell = screen.get(r, c);
            if (cell.continuation)
                continue;
            const std::string selected_sgr = sgr(cell_pen(cell));
            if (selected_sgr != current_sgr) {
                line += selected_sgr;
                current_sgr = selected_sgr;
            }
            line += cell.wide && c + 1 >= cols ? " " : glyph(cell);
        }
        fwrite(line.data(), 1, line.size(), stdout);
    }
    restore_shell_state(screen);
    fflush(stdout);
}

void Renderer::render_element(const ui::Element& element) {
    if (!initialized_)
        return;
    const auto ws = geometry(tty_fd_);
    auto frame = ftxui::Screen::Create(ftxui::Dimension::Fixed(ws.ws_col), ftxui::Dimension::Fixed(1));
    ftxui::Render(frame, element);
    // Keep the child application's saved-cursor register untouched.
    printf("\x1b[?6l\x1b[?7l\x1b[%d;1H\x1b[0m\x1b[2K", ws.ws_row);
    const std::string output = frame.ToString();
    fwrite(output.data(), 1, output.size(), stdout);
    if (parser_)
        restore_shell_state(parser_->screen());
    fflush(stdout);
}

void Renderer::render_candidates(const std::vector<Candidate>& candidates, size_t selected, const std::string& buffer,
                                 const std::string& mode, int max_items) {
    if (!initialized_)
        return;
    const auto ws = geometry(tty_fd_);
    const bool ime_active = !candidates.empty() || !buffer.empty();
    if (!ime_active) {
        const bool force = bar_dirty_;
        bar_dirty_ = false;
        if (!force && mode == last_bar_sig_ && bar_skip_count_ < BAR_FORCE_REDRAW_EVERY) {
            ++bar_skip_count_;
            return;
        }
        last_bar_sig_ = mode;
        bar_skip_count_ = 0;
    } else {
        last_bar_sig_.clear();
        bar_skip_count_ = 0;
    }
    auto element = ui::MainBar({.mode = mode, .lang_name = "", .candidates = candidates, .selected = selected,
                               .buffer = buffer, .term_width = static_cast<int>(ws.ws_col), .max_items = max_items});
    render_element(element);
}
int Renderer::read_key() {
    char c;
    return read(tty_fd_, &c, 1) > 0 ? static_cast<unsigned char>(c) : -1;
}
int Renderer::get_tty_fd() const { return tty_fd_; }

void Renderer::render_settings(ui::SettingsState& state) {
    if (!initialized_)
        return;
    const auto ws = geometry(tty_fd_);
    auto frame = ftxui::Screen::Create(ftxui::Dimension::Fixed(ws.ws_col), ftxui::Dimension::Fixed(ws.ws_row));
    ftxui::Render(frame, ui::SettingsPanel(state));
    // The overlay is not the child application: disable its input-reporting
    // modes temporarily, without changing their state in Parser.
    restore_private_modes(true);
    printf("\x1b[?6l\x1b[?7l\x1b[r\x1b[0m\x1b[2J\x1b[H");
    const std::string output = frame.ToString();
    fwrite(output.data(), 1, output.size(), stdout);
    fflush(stdout);
}
