#include "screen.hpp"
#include "../util/utf8.hpp"
#include <algorithm>
#include <utility>

namespace {
Cell blank_cell(const Pen& pen) {
    Cell c;
    c.fg = pen.fg;
    c.bg = pen.bg;
    c.bright = pen.bright;
    c.bg_bright = pen.bg_bright;
    c.reverse = pen.reverse;
    return c;
}
}

Screen::Screen(int rows, int cols) { resize(rows, cols); }

void Screen::erase_glyph(int row, int col, const Pen& pen) {
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_)
        return;
    const Cell old = grid_[row][col];
    const Cell blank = blank_cell(pen);
    grid_[row][col] = blank;
    if (old.continuation && col > 0)
        grid_[row][col - 1] = blank;
    if (old.wide && col + 1 < cols_)
        grid_[row][col + 1] = blank;
}

void Screen::put(char32_t ch, int row, int col, const Pen& pen) {
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_)
        return;
    const bool wide = utf8::width(ch) == 2;
    erase_glyph(row, col, pen);
    if (wide && col + 1 >= cols_)
        return;  // Never leave a truncated wide glyph after resize or direct put.
    if (wide)
        erase_glyph(row, col + 1, pen);
    Cell cell = blank_cell(pen);
    cell.ch = ch;
    cell.wide = wide;
    grid_[row][col] = cell;
    if (wide) {
        cell.ch = 0;
        cell.wide = false;
        cell.continuation = true;
        grid_[row][col + 1] = cell;
    }
}

void Screen::append_combining(char32_t ch, int row, int col) {
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_)
        return;
    if (grid_[row][col].continuation)
        --col;
    // Bound malicious combining-mark runs independently of terminal geometry.
    if (col >= 0 && grid_[row][col].combining.size() < 32)
        grid_[row][col].combining.push_back(ch);
}

Cell Screen::get(int row, int col) const {
    return row >= 0 && row < rows_ && col >= 0 && col < cols_ ? grid_[row][col] : Cell{};
}
void Screen::move_cursor(int row, int col) {
    cursor_row_ = std::clamp(row, 0, rows_ - 1);
    cursor_col_ = std::clamp(col, 0, cols_ - 1);
}
int Screen::cursor_row() const { return cursor_row_; }
int Screen::cursor_col() const { return cursor_col_; }
int Screen::rows() const { return rows_; }
int Screen::cols() const { return cols_; }

void Screen::scroll(int top, int bottom, int n, const Pen& pen) {
    top = std::clamp(top, 0, rows_ - 1);
    bottom = std::clamp(bottom, 0, rows_ - 1);
    if (top > bottom || n == 0)
        return;
    n = std::clamp(n, -(bottom - top + 1), bottom - top + 1);
    auto first = grid_.begin() + top;
    auto last = grid_.begin() + bottom + 1;
    if (n > 0) {
        std::rotate(first, first + n, last);
        std::fill(last - n, last, std::vector<Cell>(cols_, blank_cell(pen)));
    } else {
        std::rotate(first, last + n, last);
        std::fill(first, first - n, std::vector<Cell>(cols_, blank_cell(pen)));
    }
}
void Screen::scroll_up(int n) { scroll(0, rows_ - 1, std::max(0, n)); }
void Screen::clear() {
    for (auto& row : grid_)
        std::fill(row.begin(), row.end(), Cell{});
}
void Screen::clear_line() { erase_cells(cursor_row_, 0, cols_ - 1); }

void Screen::erase_cells(int row, int first, int last, const Pen& pen) {
    if (row < 0 || row >= rows_)
        return;
    for (int c = std::max(0, first); c <= std::min(last, cols_ - 1); ++c)
        erase_glyph(row, c, pen);
}
void Screen::erase_line(int mode, const Pen& pen) {
    if (mode < 0 || mode > 2)
        return;
    erase_cells(cursor_row_, mode == 0 ? cursor_col_ : 0, mode == 1 ? cursor_col_ : cols_ - 1, pen);
}
void Screen::erase_display(int mode, const Pen& pen) {
    if (mode == 3)  // Erase scrollback only; this model has no scrollback.
        return;
    if (mode == 5)
        mode = 2;  // compatibility with the project's existing extension
    if (mode < 0 || mode > 2)
        return;
    for (int r = 0; r < rows_; ++r) {
        if ((mode == 0 && r < cursor_row_) || (mode == 1 && r > cursor_row_))
            continue;
        const int first = mode == 0 && r == cursor_row_ ? cursor_col_ : 0;
        const int last = mode == 1 && r == cursor_row_ ? cursor_col_ : cols_ - 1;
        erase_cells(r, first, last, pen);
    }
}

void Screen::repair_row(std::vector<Cell>& row) {
    for (size_t c = 0; c < row.size(); ++c) {
        if (row[c].wide && (c + 1 == row.size() || !row[c + 1].continuation))
            row[c] = Cell{};
        if (row[c].continuation && (c == 0 || !row[c - 1].wide))
            row[c] = Cell{};
    }
}
void Screen::shift_cells(int row, int col, int count, bool insert, const Pen& pen) {
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_)
        return;
    count = std::clamp(count, 0, cols_ - col);
    auto& line = grid_[row];
    if (line[col].continuation)
        erase_glyph(row, col, pen);
    if (insert) {
        std::move_backward(line.begin() + col, line.end() - count, line.end());
        std::fill(line.begin() + col, line.begin() + col + count, blank_cell(pen));
    } else {
        std::move(line.begin() + col + count, line.end(), line.begin() + col);
        std::fill(line.end() - count, line.end(), blank_cell(pen));
    }
    repair_row(line);
}

void Screen::switch_alternate(bool enabled, bool clear_on_enter) {
    if (enabled == alternate_)
        return;
    if (other_grid_.empty())
        other_grid_.assign(rows_, std::vector<Cell>(cols_));
    grid_.swap(other_grid_);
    std::swap(cursor_row_, other_cursor_row_);
    std::swap(cursor_col_, other_cursor_col_);
    alternate_ = enabled;
    if (enabled && clear_on_enter) {
        clear();
        move_cursor(0, 0);
    }
}

void Screen::resize(int rows, int cols) {
    rows = rows <= 0 ? 24 : std::min(rows, 1000);
    cols = cols <= 0 ? 80 : std::min(cols, 1000);
    auto resize_grid = [rows, cols](Grid& grid) {
        grid.resize(rows);
        for (auto& row : grid) {
            row.resize(cols);
            repair_row(row);
        }
    };
    resize_grid(grid_);
    if (!other_grid_.empty())
        resize_grid(other_grid_);
    rows_ = rows;
    cols_ = cols;
    move_cursor(cursor_row_, cursor_col_);
    other_cursor_row_ = std::clamp(other_cursor_row_, 0, rows_ - 1);
    other_cursor_col_ = std::clamp(other_cursor_col_, 0, cols_ - 1);
}
