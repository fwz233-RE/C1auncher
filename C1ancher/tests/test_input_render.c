/* Real software-only framebuffer checks; no display, PTY or device I/O. */
#include "ui/render.h"
#include "display/frame.h"
#include "pkg/text.h"
#include "c1_ime_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool pixel(const uint8_t *frame, unsigned x, unsigned y)
{
    return (frame[(y / 8U) * C1_DISPLAY_WIDTH + x] & (0x80U >> (y % 8U))) != 0;
}

static void test_lock_text(const char *value)
{
    c1_lock_text_layout layout;
    c1_ui_state state = c1_ui_initial_state();
    c1_ui_status status = {0};
    struct {
        uint8_t before[32];
        uint8_t frame[C1_DISPLAY_FRAME_BYTES];
        uint8_t after[32];
    } guarded;
    uint8_t expected[C1_DISPLAY_FRAME_BYTES] = {0};
    uint8_t source[C1_DISPLAY_FRAME_BYTES];
    memset(&guarded, 0xa5, sizeof(guarded));
    state.page = C1_UI_PAGE_LOCK; state.preferences.lock_style = C1_LOCK_TEXT;
    strcpy(state.preferences.lock_text, value);
    assert(c1_ui_layout_lock_text(value, &layout));
    assert(layout.font_height > 16U && layout.font_height <= 144U && layout.line_count > 0U);
    assert(layout.line_count <= C1_LOCK_TEXT_MAX_LINES && layout.block_height <= 144U);
    c1_ui_render(guarded.frame, &state, &status, NULL);
    unsigned top = (C1_DISPLAY_HEIGHT - layout.block_height) / 2U;
    size_t consumed = 0U;
    for (unsigned row = 0U; row < layout.line_count; ++row) {
        char line[C1_LOCK_TEXT_BYTES] = {0};
        assert(layout.offsets[row] >= consumed && layout.lengths[row] > 0U);
        while (consumed < layout.offsets[row]) assert(value[consumed++] == ' ');
        assert(layout.offsets[row] + layout.lengths[row] <= strlen(value));
        memcpy(line, value + layout.offsets[row], layout.lengths[row]);
        assert(c1_preferences_valid_text(line)); /* No UTF-8 code point split. */
        consumed += layout.lengths[row];
        unsigned base_width = (unsigned)c1pkg_text_width(line);
        assert(layout.widths[row] == (base_width * layout.font_height + 15U) / 16U);
        assert(layout.widths[row] > 0U && layout.widths[row] <= 284U);
        assert(top >= 4U && top + layout.font_height <= 148U);
        memset(source, 0, sizeof(source));
        c1pkg_text(source, 0, 0, line, 284, 1);
        unsigned left = (C1_DISPLAY_WIDTH - layout.widths[row]) / 2U;
        for (unsigned y = 0U; y < layout.font_height; ++y)
            for (unsigned x = 0U; x < layout.widths[row]; ++x)
                if (pixel(source, x * 16U / layout.font_height, y * 16U / layout.font_height))
                    expected[(top + y) / 8U * C1_DISPLAY_WIDTH + left + x] |= (uint8_t)(0x80U >> ((top + y) % 8U));
        top += layout.font_height + layout.font_height / 16U;
    }
    while (value[consumed] == ' ') ++consumed;
    assert(!value[consumed]); /* Every non-space glyph, including the last, fits. */
    assert(!memcmp(expected, guarded.frame, sizeof(expected)));
    for (unsigned i = 0U; i < 32U; ++i) assert(guarded.before[i] == 0xa5 && guarded.after[i] == 0xa5);
}

static void test_adaptive_lock_text(void)
{
    static const char *values[] = {"你", "你好世界", "Live free or die.", "愿你拥有安静的一天",
        "千里之行始于足下坚持自己热爱的生活", "中文 English 混排测试 123", "🙂 未收录字符也使用实际字宽"};
    for (size_t i = 0U; i < sizeof(values) / sizeof(values[0]); ++i) test_lock_text(values[i]);
    char value[C1_LOCK_TEXT_BYTES];
    memset(value, 'W', sizeof(value) - 1U); value[sizeof(value) - 1U] = 0;
    test_lock_text(value);
    c1_lock_text_layout layout;
    assert(c1_ui_layout_lock_text(value, &layout) && layout.font_height == 19U);
    for (unsigned i = 0U; i < 64U; ++i) memcpy(value + i * 3U, "中", 3U);
    value[192] = 0;
    test_lock_text(value);
    assert(c1_ui_layout_lock_text(value, &layout) && layout.font_height == 23U);
    assert(!c1_ui_layout_lock_text(NULL, &layout) && !layout.line_count);
    assert(!c1_ui_layout_lock_text("", &layout) && !layout.line_count);
    assert(!c1_ui_layout_lock_text("valid\xe4\xbd", &layout) && !layout.line_count);
    assert(!c1_ui_layout_lock_text("hello", NULL));
    assert(!c1_ui_layout_lock_text("    ", &layout) && !layout.line_count);
}

static void test_edit_cursor_layout(void)
{
    c1_ui_state state = c1_ui_initial_state();
    c1_ui_status status = {0};
    struct { uint8_t before[16], frame[C1_DISPLAY_FRAME_BYTES], after[16]; } guarded;
    uint8_t expected[C1_DISPLAY_FRAME_BYTES], plain[C1_DISPLAY_FRAME_BYTES];
    c1_lock_text_edit_layout layout;
    state.page = C1_UI_PAGE_LOCK_TEXT;
    assert(c1_ui_layout_lock_text_edit(&state, &layout));
    assert(layout.offset == 0U && layout.length == 0U && layout.cursor_x == 0U);
    assert(!c1_ui_layout_lock_text_edit(NULL, &layout) && !c1_ui_layout_lock_text_edit(&state, NULL));
    for (unsigned i = 0U; i < 10U; ++i) assert(c1_ui_lock_text_append_utf8(&state, "Wi中é🙂 x"));
    assert(c1pkg_text_width(state.lock_text_draft) > (int)C1_LOCK_TEXT_EDIT_WIDTH);
    for (;;) {
        assert(c1_ui_layout_lock_text_edit(&state, &layout));
        assert(layout.offset <= state.lock_text_cursor);
        assert(layout.offset + layout.length >= state.lock_text_cursor);
        assert(layout.offset + layout.length <= strlen(state.lock_text_draft));
        assert(layout.cursor_x <= C1_LOCK_TEXT_EDIT_WIDTH);
        assert(((unsigned char)state.lock_text_draft[layout.offset] & 0xc0U) != 0x80U);
        assert(((unsigned char)state.lock_text_draft[layout.offset + layout.length] & 0xc0U) != 0x80U);
        char left[C1_LOCK_TEXT_BYTES] = {0}, right[C1_LOCK_TEXT_BYTES] = {0};
        memcpy(left, state.lock_text_draft + layout.offset, state.lock_text_cursor - layout.offset);
        memcpy(right, state.lock_text_draft + state.lock_text_cursor,
               layout.offset + layout.length - state.lock_text_cursor);
        assert(!left[0] || c1_preferences_valid_text(left));
        assert(!right[0] || c1_preferences_valid_text(right));
        assert(c1pkg_text_width(left) == (int)layout.cursor_x);
        assert(c1pkg_text_width(left) + c1pkg_text_width(right) <= (int)C1_LOCK_TEXT_EDIT_WIDTH);
        memset(&guarded, 0xa5, sizeof(guarded));
        c1_ui_render(guarded.frame, &state, &status, NULL);
        memset(expected, 0, sizeof(expected));
        c1pkg_text(expected, 12, 55, left, C1_LOCK_TEXT_EDIT_WIDTH, true);
        c1pkg_text(expected, 14 + (int)layout.cursor_x, 55, right,
                   C1_LOCK_TEXT_EDIT_WIDTH - layout.cursor_x, true);
        for (unsigned y = 55U; y < 71U; ++y) {
            for (unsigned x = 12U; x < 280U; ++x)
                assert(pixel(guarded.frame, x, y) == (x == 12U + layout.cursor_x || pixel(expected, x, y)));
        }
        for (unsigned i = 0; i < 16; ++i) assert(guarded.before[i] == 0xa5 && guarded.after[i] == 0xa5);
        if (!state.lock_text_cursor) break;
        assert(c1_ui_lock_text_move(&state, -1));
    }
    assert(layout.offset == 0U && layout.length > 0U); /* Beginning scrolls back into view. */
    while (c1_ui_lock_text_move(&state, 1)) {}
    assert(c1_ui_layout_lock_text_edit(&state, &layout) && layout.offset > 0U);
    assert(layout.offset + layout.length == strlen(state.lock_text_draft));
    state.lock_text_draft[0] = 0; state.lock_text_cursor = 0;
    c1_ui_render(plain, &state, &status, NULL);
    for (unsigned y = 55; y < 71; ++y) assert(pixel(plain, 12, y)); /* Empty caret remains visible. */
    strcpy(state.wifi_notice, "Save failed; retry");
    c1_ui_render(guarded.frame, &state, &status, NULL);
    struct c1_ime_response view = {.flags = C1_IME_READY | C1_IME_CHINESE};
    c1_ui_render_input(guarded.frame, &state, &view, true, false);
    unsigned changed = 0U;
    for (unsigned y = 100U; y < 116U; ++y)
        for (unsigned x = 10U; x < 286U; ++x) changed += pixel(guarded.frame, x, y) != pixel(plain, x, y);
    assert(changed); /* Input strip does not overwrite errors or editor caret. */
    for (unsigned y = 55; y < 71; ++y) assert(pixel(guarded.frame, 12, y));
    strcpy(state.lock_text_draft, "invalid\xe4\xbd");
    assert(!c1_ui_layout_lock_text_edit(&state, &layout));
    memset(state.lock_text_draft, 'x', sizeof(state.lock_text_draft));
    assert(!c1_ui_layout_lock_text_edit(&state, &layout));
}

static void test_wifi_startup_notices(void)
{
    static const char *messages[][2] = {
        {"WLAN0 INITIALIZATION TIMED OUT", "无线网卡启动超时，请重试"},
        {"WI-FI MODULE LOAD FAILED", "无线驱动加载失败，请重试"},
        {"WI-FI FAILED DRIVER RECOVERY REFUSED", "无线驱动暂无法恢复，请重试"},
    };
    c1_ui_state state = c1_ui_initial_state();
    c1_ui_status status = {0};
    uint8_t actual[C1_DISPLAY_FRAME_BYTES], expected[C1_DISPLAY_FRAME_BYTES];
    state.page = C1_UI_PAGE_WIFI;
    for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); ++i) {
        state.preferences.language = C1_LANGUAGE_ZH;
        strcpy(state.wifi_notice, messages[i][0]);
        c1_ui_render(actual, &state, &status, NULL);
        strcpy(state.wifi_notice, messages[i][1]);
        c1_ui_render(expected, &state, &status, NULL);
        assert(!memcmp(actual, expected, sizeof(actual)));
        assert(c1pkg_text_width(messages[i][1]) <= 280);
        state.preferences.language = C1_LANGUAGE_EN;
        strcpy(state.wifi_notice, messages[i][0]);
        c1_ui_render(actual, &state, &status, NULL);
        strcpy(state.wifi_notice, messages[i][1]);
        c1_ui_render(expected, &state, &status, NULL);
        assert(memcmp(actual, expected, sizeof(actual)) != 0);
    }
}

int main(void)
{
    test_wifi_startup_notices();
    uint8_t frame[C1_DISPLAY_FRAME_BYTES];
    c1_ui_state state = {0};
    struct c1_ime_response view = {0};
    state.page = C1_UI_PAGE_TERMINAL;
    view.flags = C1_IME_READY | C1_IME_CHINESE | C1_IME_COMPOSING;
    view.candidate_count = 5;
    strcpy(view.preedit, "ni");
    for (unsigned i = 0; i < view.candidate_count; ++i) strcpy(view.candidates[i], "你好");
    for (unsigned selected = 0; selected < view.candidate_count; ++selected) {
        memset(frame, 0, sizeof(frame));
        view.highlighted_candidate = selected;
        c1_ui_render_input(frame, &state, &view, true, false);
        for (unsigned i = 0; i < view.candidate_count; ++i)
            assert(pixel(frame, 3U + i * 59U, 135U) == (i == selected));
        /* Padding outside the candidate strip and the terminal content remain clear. */
        assert(!pixel(frame, 0U, 135U) && !pixel(frame, 10U, 118U));
    }
    state.page = C1_UI_PAGE_LOCK_TEXT;
    view.highlighted_candidate = 1;
    memset(frame, 0, sizeof(frame));
    c1_ui_render_input(frame, &state, &view, true, false);
    assert(pixel(frame, 62U, 135U) && !pixel(frame, 3U, 135U));
    state.page = C1_UI_PAGE_WIFI_PASSWORD;
    memset(frame, 0, sizeof(frame));
    c1_ui_render_input(frame, &state, &view, true, false);
    for (unsigned i = 0; i < sizeof(frame); ++i) assert(frame[i] == 0);
    state.page = C1_UI_PAGE_TERMINAL;
    c1_ui_render_input(frame, &state, &view, false, false);
    for (unsigned i = 0; i < sizeof(frame); ++i) assert(frame[i] == 0);
    test_adaptive_lock_text();
    test_edit_cursor_layout();
    puts("input render tests passed: candidates, adaptive full-text lock layout, password bypass");
    return 0;
}
