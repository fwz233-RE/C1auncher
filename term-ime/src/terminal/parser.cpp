#include "parser.hpp"
#include "../util/utf8.hpp"
#include <algorithm>
#include <charconv>
#include <vector>

namespace {
int parse_param(std::string_view s, int def) {
    if (s.empty())
        return def;
    int v = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    if (result.ec != std::errc{} || result.ptr != s.data() + s.size() || v < 0)
        return def;
    return std::min(v, 1000000);  // safe arithmetic before Screen clamps geometry
}
std::vector<int> params(std::string_view s, int def = 0) {
    std::vector<int> out;
    do {
        const size_t sep = s.find(';');
        const auto field = s.substr(0, sep);
        out.push_back(field.empty() ? 0 : parse_param(field, def));
        if (sep == std::string_view::npos)
            break;
        s.remove_prefix(sep + 1);
    } while (true);
    return out;
}
}

Parser::Parser(Screen& screen) : screen_(screen) { reset_private_modes(); }

void Parser::reset_private_modes() {
    for (size_t i = 0; i < kForwardedPrivateModes.size(); ++i)
        private_modes_[i] = kForwardedPrivateModes[i] == 25;
}
bool Parser::private_mode(int mode) const {
    for (size_t i = 0; i < kForwardedPrivateModes.size(); ++i)
        if (kForwardedPrivateModes[i] == mode)
            return private_modes_[i];
    return false;
}
void Parser::resize(int rows, int cols) {
    const int old_cols = screen_.cols();
    screen_.resize(rows, cols);
    const int new_cols = screen_.cols(), new_rows = screen_.rows();
    auto resize_cursor = [&](int& row, int& col, bool& wrap) {
        if (new_cols > old_cols && wrap && col == old_cols - 1) {
            ++col;
            wrap = false;
        }
        row = std::clamp(row, 0, new_rows - 1);
        col = std::clamp(col, 0, new_cols - 1);
    };
    auto resize_margins = [&](int& top, int& bottom) {
        top = std::clamp(top, 0, new_rows - 1);
        if (bottom >= 0)
            bottom = std::min(bottom, new_rows - 1);
        if (top >= (bottom < 0 ? new_rows - 1 : bottom)) {
            top = 0;
            bottom = -1;
        }
    };
    resize_margins(scroll_top_, scroll_bottom_);
    resize_margins(main_scroll_top_, main_scroll_bottom_);
    int row = screen_.cursor_row(), col = screen_.cursor_col();
    resize_cursor(row, col, wrap_pending_);
    if (origin_mode_)
        row = std::clamp(row, scroll_top(), scroll_bottom());
    screen_.move_cursor(row, col);
    // The main screen can be parked at its margin while an alternate screen
    // is visible. Adjust its insertion point too, without clearing either bank.
    if (screen_.alternate_screen()) {
        screen_.switch_alternate(false, false);
        row = screen_.cursor_row();
        col = screen_.cursor_col();
        resize_cursor(row, col, main_wrap_);
        if (main_origin_)
            row = std::clamp(row, main_scroll_top_, main_scroll_bottom_ < 0 ? new_rows - 1 : main_scroll_bottom_);
        screen_.move_cursor(row, col);
        screen_.switch_alternate(true, false);
    }
    for (auto& saved : saved_)
        resize_cursor(saved.row, saved.col, saved.wrap);
}

void Parser::feed(const uint8_t* data, size_t len) {
    stream_.feed(reinterpret_cast<const char*>(data), len, [this](std::string_view token) { consume(token); });
}
void Parser::consume(std::string_view token) {
    size_t pos = 0;
    while (pos < token.size()) {
        const auto byte = static_cast<uint8_t>(token[pos]);
        if (byte < 0x80) {
            handle_char(static_cast<char>(byte));
            ++pos;
        } else if (state_ != State::Normal) {
            // Control-string content is opaque bytes, including non-ASCII.
            if (state_ == State::OSCEsc)
                state_ = State::OSC;
            ++pos;
        } else {
            emit_char(utf8::decode(reinterpret_cast<const uint8_t*>(token.data()), token.size(), pos));
        }
    }
}
int Parser::scroll_top() const { return std::clamp(scroll_top_, 0, screen_.rows() - 1); }
int Parser::scroll_bottom() const {
    return scroll_bottom_ < 0 ? screen_.rows() - 1 : std::clamp(scroll_bottom_, scroll_top(), screen_.rows() - 1);
}
int Parser::next_row(int row) {
    if (row == scroll_bottom()) {
        screen_.scroll(scroll_top(), scroll_bottom(), 1, pen_);
        return row;
    }
    return std::min(row + 1, screen_.rows() - 1);
}

void Parser::emit_char(char32_t ch) {
    int width = utf8::width(ch);
    int row = screen_.cursor_row();
    int col = screen_.cursor_col();
    if (width <= 0) {
        if (col > 0 || wrap_pending_)
            screen_.append_combining(ch, row, wrap_pending_ ? col : col - 1);
        return;
    }
    if (wrap_pending_ && autowrap_) {
        row = next_row(row);
        col = 0;
    }
    wrap_pending_ = false;
    if (col + width > screen_.cols()) {
        if (!autowrap_ || width > screen_.cols())
            return;
        row = next_row(row);
        col = 0;
    }
    screen_.put(ch, row, col, pen_);
    col += width;
    if (col >= screen_.cols()) {
        col = screen_.cols() - 1;
        wrap_pending_ = autowrap_;
    }
    screen_.move_cursor(row, col);
}

void Parser::save_cursor() {
    saved_[screen_.alternate_screen() ? 1 : 0] = {screen_.cursor_row(), screen_.cursor_col(), pen_, wrap_pending_};
}
void Parser::restore_cursor() {
    const auto& saved = saved_[screen_.alternate_screen() ? 1 : 0];
    screen_.move_cursor(saved.row, saved.col);
    pen_ = saved.pen;
    wrap_pending_ = saved.wrap;
}
void Parser::set_private_mode(int mode, bool enabled) {
    const auto mouse_tracking = [](int number) {
        return number == 9 || number == 1000 || number == 1002 || number == 1003;
    };
    if (enabled && mouse_tracking(mode)) {
        // Mouse tracking is a selection, not four independent toggles. Keep
        // the last requested mode, rather than replaying an older one later.
        for (size_t i = 0; i < kForwardedPrivateModes.size(); ++i)
            if (mouse_tracking(kForwardedPrivateModes[i]))
                private_modes_[i] = false;
    }
    for (size_t i = 0; i < kForwardedPrivateModes.size(); ++i) {
        if (kForwardedPrivateModes[i] == mode) {
            private_modes_[i] = enabled;
            return;
        }
    }
    if (mode == 7) {
        autowrap_ = enabled;
        wrap_pending_ = false;
    } else if (mode == 6) {
        origin_mode_ = enabled;
        wrap_pending_ = false;
        screen_.move_cursor(enabled ? scroll_top() : 0, 0);
    } else if (mode == 1048) {
        enabled ? save_cursor() : restore_cursor();
    } else if (mode == 47 || mode == 1047 || mode == 1049) {
        if (enabled == screen_.alternate_screen())
            return;
        if (enabled) {
            main_pen_ = pen_;
            main_wrap_ = wrap_pending_;
            main_scroll_top_ = scroll_top_;
            main_scroll_bottom_ = scroll_bottom_;
            main_origin_ = origin_mode_;
            screen_.switch_alternate(true, mode != 47);
            scroll_top_ = 0;
            scroll_bottom_ = -1;
            origin_mode_ = false;
            wrap_pending_ = false;
        } else {
            screen_.switch_alternate(false);
            pen_ = main_pen_;
            wrap_pending_ = main_wrap_;
            scroll_top_ = main_scroll_top_;
            scroll_bottom_ = main_scroll_bottom_;
            origin_mode_ = main_origin_;
        }
    }
}

void Parser::handle_char(char c) {
    switch (state_) {
    case State::Normal:
        if (c == '\x1b') {
            state_ = State::Escape;
        } else if (c == '\r') {
            wrap_pending_ = false;
            screen_.move_cursor(screen_.cursor_row(), 0);
        } else if (c == '\n' || c == '\v' || c == '\f') {
            wrap_pending_ = false;
            screen_.move_cursor(next_row(screen_.cursor_row()), screen_.cursor_col());
        } else if (c == '\b') {
            wrap_pending_ = false;
            screen_.move_cursor(screen_.cursor_row(), screen_.cursor_col() - 1);
        } else if (c == '\t') {
            wrap_pending_ = false;
            screen_.move_cursor(screen_.cursor_row(), ((screen_.cursor_col() / 8) + 1) * 8);
        } else if (static_cast<unsigned char>(c) >= 0x20 && c != '\x7f') {
            emit_char(static_cast<char32_t>(c));
        }
        break;
    case State::Escape:
        state_ = State::Normal;
        if (c == '[') {
            state_ = State::CSI;
            csi_params_.clear();
            csi_prefix_ = 0;
            csi_intermediate_ = false;
        } else if (c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_') {
            osc_bel_ = c == ']';
            state_ = State::OSC;
        } else if (c >= 0x20 && c <= 0x2f) {
            state_ = State::EscapeIntermediate;
        } else if (c == '7') {
            save_cursor();
        } else if (c == '8') {
            restore_cursor();
        } else if (c == 'D' || c == 'E') {
            wrap_pending_ = false;
            screen_.move_cursor(next_row(screen_.cursor_row()), c == 'E' ? 0 : screen_.cursor_col());
        } else if (c == 'M') {
            wrap_pending_ = false;
            if (screen_.cursor_row() == scroll_top())
                screen_.scroll(scroll_top(), scroll_bottom(), -1, pen_);
            else
                screen_.move_cursor(screen_.cursor_row() - 1, screen_.cursor_col());
        } else if (c == 'c') {
            screen_.switch_alternate(false);
            screen_.clear();
            screen_.move_cursor(0, 0);
            pen_ = Pen{};
            wrap_pending_ = origin_mode_ = false;
            autowrap_ = true;
            reset_private_modes();
            scroll_top_ = 0;
            scroll_bottom_ = -1;
        }
        break;
    case State::EscapeIntermediate:
        if (c >= 0x30 && c <= 0x7e)
            state_ = State::Normal;
        break;
    case State::CSI:
        handle_csi(c);
        break;
    case State::OSC:
        if (osc_bel_ && c == '\x07')
            state_ = State::Normal;
        else if (c == '\x1b')
            state_ = State::OSCEsc;
        break;
    case State::OSCEsc:
        state_ = c == '\\' ? State::Normal : (c == '\x1b' ? State::OSCEsc : State::OSC);
        break;
    }
}

void Parser::handle_csi(char c) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 0x30 && byte <= 0x3f) {
        if ((c >= '0' && c <= '9') || c == ';')
            csi_params_ += c;
        else
            csi_prefix_ = c;
        return;
    }
    if (byte >= 0x20 && byte <= 0x2f) {
        csi_intermediate_ = true;
        return;
    }
    if (byte < 0x40 || byte > 0x7e)
        return;
    state_ = State::Normal;
    if (csi_intermediate_)
        return;
    const auto args = params(csi_params_);
    auto arg = [&](size_t i, int def) { return i < args.size() && args[i] != 0 ? args[i] : def; };
    if (csi_prefix_) {
        if (csi_prefix_ == '?' && (c == 'h' || c == 'l'))
            for (int mode : args)
                set_private_mode(mode, c == 'h');
        return;
    }
    const int n = arg(0, 1);
    const int row = screen_.cursor_row(), col = screen_.cursor_col();
    auto move = [&](int r, int x) {
        wrap_pending_ = false;
        if (origin_mode_)
            r = std::clamp(r, scroll_top(), scroll_bottom());
        screen_.move_cursor(r, x);
    };
    switch (c) {
    case 'H': case 'f': move(arg(0, 1) - 1 + (origin_mode_ ? scroll_top() : 0), arg(1, 1) - 1); break;
    case 'A': move(row - n, col); break;
    case 'B': case 'e': move(row + n, col); break;
    case 'C': case 'a': move(row, col + n); break;
    case 'D': move(row, col - n); break;
    case 'E': move(row + n, 0); break;
    case 'F': move(row - n, 0); break;
    case 'G': case '`': move(row, n - 1); break;
    case 'd': move(n - 1 + (origin_mode_ ? scroll_top() : 0), col); break;
    case 'I': move(row, (col / 8 + n) * 8); break;
    case 'Z': move(row, ((std::max(col - 1, 0) / 8) - n + 1) * 8); break;
    case 'J': screen_.erase_display(args[0], pen_); break;
    case 'K': screen_.erase_line(args[0], pen_); break;
    case 'X': screen_.erase_cells(row, col, col + n - 1, pen_); break;
    case '@': screen_.shift_cells(row, col, n, true, pen_); break;
    case 'P': screen_.shift_cells(row, col, n, false, pen_); break;
    case 'S': screen_.scroll(scroll_top(), scroll_bottom(), n, pen_); break;
    case 'T': screen_.scroll(scroll_top(), scroll_bottom(), -n, pen_); break;
    case 'L': case 'M':
        if (row >= scroll_top() && row <= scroll_bottom())
            screen_.scroll(row, scroll_bottom(), c == 'L' ? -n : n, pen_);
        break;
    case 'r': {
        const int top = arg(0, 1) - 1, bottom = arg(1, screen_.rows()) - 1;
        if (top >= 0 && top < bottom && bottom < screen_.rows()) {
            scroll_top_ = top;
            scroll_bottom_ = bottom == screen_.rows() - 1 ? -1 : bottom;
            move(origin_mode_ ? top : 0, 0);
        }
        break;
    }
    case 's': save_cursor(); break;
    case 'u': restore_cursor(); break;
    case 'm': apply_sgr(); break;
    }
}

void Parser::apply_sgr() {
    const auto args = params(csi_params_, -1);
    for (size_t i = 0; i < args.size(); ++i) {
        // Empty fields are SGR reset, unlike malformed numbers.
        const int n = csi_params_.empty() ? 0 : args[i];
        if (n == 38 || n == 48) {
            if (i + 1 < args.size() && args[i + 1] == 2)
                i += 4;
            else if (i + 1 < args.size() && args[i + 1] == 5)
                i += 2;
            else
                i += 1;
            continue;
        }
        switch (n) {
        case 0: pen_ = Pen{}; break;
        case 1: pen_.bright = true; break;
        case 22: pen_.bright = false; break;
        case 5: pen_.bg_bright = true; break;
        case 25: pen_.bg_bright = false; break;
        case 7: pen_.reverse = true; break;
        case 27: pen_.reverse = false; break;
        case 39: pen_.fg = 7; pen_.bright = false; break;
        case 49: pen_.bg = 0; pen_.bg_bright = false; break;
        default:
            if (n >= 30 && n <= 37) { pen_.fg = n - 30; pen_.bright = false; }
            else if (n >= 90 && n <= 97) { pen_.fg = n - 90; pen_.bright = true; }
            else if (n >= 40 && n <= 47) { pen_.bg = n - 40; pen_.bg_bright = false; }
            else if (n >= 100 && n <= 107) { pen_.bg = n - 100; pen_.bg_bright = true; }
            break;
        }
    }
}
