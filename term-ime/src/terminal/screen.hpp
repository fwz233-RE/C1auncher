#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Cell {
    char32_t ch = U' ';
    uint8_t fg = 7;
    uint8_t bg = 0;
    bool wide = false;
    bool bright = false;
    bool bg_bright = false;
    bool reverse = false;
    // The second column of a wide glyph is explicit, never an extra printable
    // space. Both halves are removed when either is overwritten/erased.
    bool continuation = false;
    std::u32string combining;
};

struct Pen {
    uint8_t fg = 7;
    uint8_t bg = 0;
    bool bright = false;
    bool bg_bright = false;
    bool reverse = false;
};

class Screen {
   public:
    Screen(int rows, int cols);
    void put(char32_t ch, int row, int col, const Pen& pen = {});
    void append_combining(char32_t ch, int row, int col);
    Cell get(int row, int col) const;
    void move_cursor(int row, int col);
    int cursor_row() const;
    int cursor_col() const;
    void scroll_up(int n = 1);
    // Positive n scrolls up; negative n scrolls down, within inclusive margins.
    void scroll(int top, int bottom, int n, const Pen& pen = {});
    void clear();
    void clear_line();
    void erase_display(int mode, const Pen& pen);
    void erase_line(int mode, const Pen& pen);
    void erase_cells(int row, int first, int last, const Pen& pen = {});
    void shift_cells(int row, int col, int count, bool insert, const Pen& pen);
    int rows() const;
    int cols() const;
    void resize(int rows, int cols);

    // Two child buffers, independent of the host terminal's alternate screen.
    bool alternate_screen() const { return alternate_; }
    void switch_alternate(bool enabled, bool clear_on_enter = true);

   private:
    using Grid = std::vector<std::vector<Cell>>;
    Grid grid_;
    Grid other_grid_;
    int cursor_row_ = 0;
    int cursor_col_ = 0;
    int other_cursor_row_ = 0;
    int other_cursor_col_ = 0;
    int rows_ = 0;
    int cols_ = 0;
    bool alternate_ = false;

    void erase_glyph(int row, int col, const Pen& pen);
    static void repair_row(std::vector<Cell>& row);
};
