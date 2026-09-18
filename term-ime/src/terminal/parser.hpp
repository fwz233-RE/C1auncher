#pragma once

#include "screen.hpp"
#include "output_stream.hpp"
#include <array>
#include <cstdint>
#include <string>

class Parser {
   public:
    explicit Parser(Screen& screen);
    void feed(const uint8_t* data, size_t len);
    // Resize without reflow. Widening a pending full line turns its virtual
    // insertion point into the first newly available column (ABCDE -> ABCDEF).
    // Shrinking keeps deferred wrap at the new right margin.
    void resize(int rows, int cols);
    // Host input/display modes must survive output suppressed by overlays.
    // These modes are global to the child terminal, not per screen bank.
    inline static constexpr std::array<int, 12> kForwardedPrivateModes = {
        1, 9, 25, 1000, 1002, 1003, 1004, 1005, 1006, 1015, 1016, 2004};
    bool private_mode(int mode) const;
    Screen& screen() { return screen_; }
    const Screen& screen() const { return screen_; }
    const Pen& pen() const { return pen_; }
    bool wrap_pending() const { return wrap_pending_; }
    bool autowrap() const { return autowrap_; }
    bool origin_mode() const { return origin_mode_; }
    int scroll_top() const;
    int scroll_bottom() const;

   private:
    Screen& screen_;
    OutputStream stream_;
    enum class State { Normal, Escape, EscapeIntermediate, CSI, OSC, OSCEsc };
    State state_ = State::Normal;
    std::string csi_params_;
    char csi_prefix_ = 0;
    bool csi_intermediate_ = false;
    bool osc_bel_ = false;
    Pen pen_;
    bool wrap_pending_ = false;
    bool autowrap_ = true;
    bool origin_mode_ = false;
    int scroll_top_ = 0;
    int scroll_bottom_ = -1;
    struct SavedCursor {
        int row = 0;
        int col = 0;
        Pen pen;
        bool wrap = false;
    };
    SavedCursor saved_[2];
    Pen main_pen_;
    bool main_wrap_ = false;
    int main_scroll_top_ = 0;
    int main_scroll_bottom_ = -1;
    bool main_origin_ = false;
    std::array<bool, kForwardedPrivateModes.size()> private_modes_{};

    void reset_private_modes();
    void consume(std::string_view token);
    void handle_char(char c);
    void handle_csi(char c);
    void emit_char(char32_t ch);
    void apply_sgr();
    void set_private_mode(int mode, bool enabled);
    void save_cursor();
    void restore_cursor();
    int next_row(int row);
};
