#include "ui/chrome.h"
#include "services/desktop_data.h"
#ifndef C1_VERSION
#define C1_VERSION "unknown"
#endif
#include "ui/render.h"
#include "display/frame.h"
#include "ui/canvas.h"
#include "ui/focus.h"
#include "ui/wallpaper.h"
#include "pkg/text.h"
#include "c1_ime_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TR(zh, en) c1_ui_tr(state->preferences.language, zh, en)

static void text(uint8_t *frame, int x, int y, const char *s, int width, bool ink)
{
    (void)c1pkg_font_license();
    if (c1pkg_text_width(s) > width && width >= 24) {
        c1pkg_text(frame, x, y, s, width - 16, ink);
        c1pkg_text(frame, x + width - 16, y, "..", 16, ink);
    } else c1pkg_text(frame, x, y, s, width, ink);
}

static void line(uint8_t *frame, unsigned x, unsigned y, unsigned w)
{
    c1_canvas_fill_rect(frame, x, y, w, 1, true);
}

static const char *page_name(const c1_ui_state *state)
{
    switch (state->page) {
    case C1_UI_PAGE_DESKTOP: return TR("桌面", "Desktop");
    case C1_UI_PAGE_WIFI: return "Wi-Fi";
    case C1_UI_PAGE_WIFI_PASSWORD: return TR("连接 Wi-Fi", "Join Wi-Fi");
    case C1_UI_PAGE_BATTERY: return TR("电池", "Battery");
    case C1_UI_PAGE_ABOUT: return TR("关于", "About");
    case C1_UI_PAGE_SETTINGS: return TR("设置", "Settings");
    case C1_UI_PAGE_LOCK_TEXT: return TR("锁屏文字", "Lock text");
    case C1_UI_PAGE_TERMINAL:
        if (state->terminal_action == C1_UI_ACTION_TERMINAL_APP) return TR("应用", "Apps");
        if (state->terminal_action == C1_UI_ACTION_TERMINAL_UPDATE) return TR("系统更新", "System update");
        return TR("终端", "Terminal");
    case C1_UI_PAGE_LOCK: return "";
    }
    return "";
}

static void header(uint8_t *frame, const c1_ui_state *state, const c1_ui_status *status)
{
    char clock[8], battery[8];
    if (status->time_available) snprintf(clock, sizeof(clock), "%02u:%02u", status->hour % 24, status->minute % 60);
    else snprintf(clock, sizeof(clock), "--:--");
    if (status->battery_available) snprintf(battery, sizeof(battery), "%u%%", status->battery_percent % 101);
    else snprintf(battery, sizeof(battery), "--%%");
    c1_chrome_header(frame, page_name(state), clock, battery);
}

static void footer(uint8_t *frame, const char *hint)
{
    line(frame, 0, 134, 296);
    text(frame, 6, 136, hint, 284, true);
}

static void title(uint8_t *frame, const char *s, const char *right)
{
    int width = right ? c1pkg_text_width(right) : 0;
    if (width > 152) width = 152;
    text(frame, 8, 21, s, right && width ? 272 - width : 280, true);
    if (right) text(frame, 288 - width, 21, right, width, true);
}

static void render_desktop(uint8_t *frame, const c1_ui_state *state, const c1_ui_status *status)
{
    static const char *zh[] = {"应用", "终端", "Wi-Fi", "电池", "设置"};
    static const char *en[] = {"Apps", "Terminal", "Wi-Fi", "Battery", "Settings"};
    for (unsigned i = 0; i < 5; ++i) {
        unsigned y = 23 + i * 21;
        bool selected = state->selection == i;
        if (selected) c1_canvas_fill_rect(frame, 6, y, 284, 20, true);
        text(frame, 14, (int)y + 2, TR(zh[i], en[i]), 104, !selected);
        char applications[40];
        const char *detail = "";
        if (i == 0 && status->new_applications) {
            snprintf(applications, sizeof(applications), TR("%u个新应用", "%u new apps"), status->new_applications);
            detail = applications;
        }
        if (i == 2) detail = status->wifi_connected ?
            (status->wifi_ipv4[0] ? TR("已连接", "Connected") : TR("正在获取地址", "Getting IP")) :
            TR("无网络", "No network");
        if (i == 4 && status->update_available) detail = TR("有系统更新", "New system version");
        text(frame, 280 - c1pkg_text_width(detail), (int)y + 2, detail, 156, !selected);
    }
    const char *quote = c1_desktop_quote_valid(status->daily_quote) ? status->daily_quote :
                        TR("Live free or die.", "Live free or die.");
    footer(frame, state->wifi_notice[0] ? state->wifi_notice : quote);
}

static void signal_icon(uint8_t *frame, unsigned x, unsigned y, int dbm, bool ink)
{
    unsigned bars = dbm >= -55 ? 4 : dbm >= -67 ? 3 : dbm >= -75 ? 2 : 1;
    for (unsigned i = 0; i < bars; ++i) c1_canvas_fill_rect(frame, x + i * 3, y + 12 - i * 3, 2, 3 + i * 3, ink);
}

static const char *notice(const c1_ui_state *state, const char *message)
{
    if (state->preferences.language == C1_LANGUAGE_EN) return message;
    if (!strcmp(message, "TARGET AUTHENTICATION TIMED OUT") || !strcmp(message, "AUTHENTICATION FAILED")) return "验证失败，请检查密码或信号";
    if (!strcmp(message, "TARGET ADDRESS ACQUISITION FAILED")) return "未获取地址，请检查路由器";
    if (!strcmp(message, "CREDENTIAL REJECTED")) return "密码设置失败，请重新输入";
    if (!strcmp(message, "CREDENTIAL PREPARATION FAILED")) return "密码处理失败或已取消";
    if (!strcmp(message, "WLAN0 DHCP OWNED BY ANOTHER SERVICE")) return "网络地址服务被其他程序占用";
    if (!strcmp(message, "WLAN0 OWNED BY ANOTHER SERVICE")) return "无线网被其他程序占用";
    if (!strcmp(message, "WLAN0 RADIO BLOCKED OR UNAVAILABLE")) return "无线硬件未就绪或被禁用";
    if (!strcmp(message, "WLAN0 INITIALIZATION TIMED OUT")) return "无线网卡启动超时，请重试";
    if (!strcmp(message, "WI-FI MODULE LOAD FAILED")) return "无线驱动加载失败，请重试";
    if (!strcmp(message, "WI-FI FAILED DRIVER RECOVERY REFUSED")) return "无线驱动暂无法恢复，请重试";
    if (!strcmp(message, "SCAN TIMED OUT")) return "扫描超时，请重试";
    if (!strcmp(message, "CONNECT FAILED; RESTORE INCOMPLETE")) return "连接失败，旧网络恢复未完成";
    if (!strcmp(message, "WI-FI RESUME AUTHENTICATION FAILED")) return "唤醒后未能重新连接网络";
    if (!strcmp(message, "WI-FI RESUME ADDRESS FAILED")) return "唤醒后未能获取网络地址";
    if (!strcmp(message, "USE 8-63 CHARACTERS")) return "密码需要 8-63 个字符";
    if (!strcmp(message, "BUSY - YOUR PASSWORD IS KEPT")) return "正在处理，密码已保留";
    if (!strcmp(message, "THIS SECURITY TYPE IS NOT SUPPORTED")) return "暂不支持此加密方式";
    if (!strcmp(message, "ALREADY CONNECTED TO THIS NETWORK")) return "已连接此网络";
    if (!strcmp(message, "BUSY - WAIT OR SELECT TURN OFF")) return "请等待，或选择关闭";
    if (!strcmp(message, "WI-FI TASK ALREADY RUNNING")) return "网络任务进行中";
    if (!strcmp(message, "UPDATE CHECK RUNNING; TRY SCAN LATER")) return "正在检查更新，请稍后扫描";
    if (!strcmp(message, "STOP QUEUED; WAITING FOR CURRENT TASK")) return "正在取消并恢复网络";
    return message;
}

static const char *phase_text(const c1_ui_state *state, const c1_ui_status *status)
{
    if (status->wifi_stop_pending) return TR("正在停止 / 恢复网络", "Stopping / restoring network");
    switch (status->wifi_phase) {
    case C1_WIFI_PHASE_AUTHENTICATING: return TR("正在验证密码", "Authenticating");
    case C1_WIFI_PHASE_RESTORING: return TR("正在恢复网络", "Restoring network");
    default: return status->wifi_activity == C1_UI_ACTION_WIFI_SCAN ? TR("正在扫描网络", "Scanning networks") :
                       TR("正在连接，请稍候", "Connecting, please wait");
    }
}

static void render_wifi(uint8_t *frame, const c1_ui_state *state, const c1_ui_status *status)
{
    title(frame, status->wifi_connected ?
          (status->wifi_ipv4[0] ? status->wifi_connected_ssid : TR("正在获取网络地址", "Getting network address")) :
          TR("无网络", "No network"), status->wifi_connected ? status->wifi_ipv4 : NULL);
    for (unsigned i = 0; i < 2; ++i) {
        bool selected = state->selection == i;
        unsigned x = 8 + i * 142;
        c1_canvas_fill_rect(frame, x, 40, 136, 18, selected);
        text(frame, (int)x + 6, 41, i == 0 ? TR("扫描网络", "Scan") : TR("关闭无线网", "Turn off"), 124, !selected);
    }
    if (!status->network_count) {
        text(frame, 12, 73, status->wifi_busy ? phase_text(state, status) :
             status->wifi_enabled ? TR("未发现网络，请重新扫描", "No networks. Scan again.") :
             TR("无线网已关闭，选择扫描开启", "Wi-Fi is off. Select Scan."), 272, true);
    } else {
        unsigned first = state->selection >= 2 ? ((state->selection - 2) / 3) * 3 : 0;
        for (unsigned i = first; i < first + 3 && i < status->network_count && i < C1_UI_MAX_NETWORKS; ++i) {
            unsigned y = 62 + (i - first) * 22;
            bool selected = state->selection == i + 2;
            if (selected) c1_canvas_fill_rect(frame, 8, y, 280, 21, true);
            const c1_ui_network *n = &status->networks[i];
            const char *mark = c1_ui_network_is_current(status, i) ? "*" : n->saved ? "+" : n->secured ? "#" : "-";
            text(frame, 12, (int)y + 2, mark, 8, !selected);
            text(frame, 26, (int)y + 2, n->ssid, 234, !selected);
            signal_icon(frame, 269, y + 3, n->signal_dbm, !selected);
        }
    }
    const char *message = state->wifi_notice[0] ? state->wifi_notice :
                          status->wifi_busy ? phase_text(state, status) : status->wifi_message;
    footer(frame, message[0] ? notice(state, message) : TR("方向/表情选择  确认连接", "Arrows / Emoji: select  OK: join"));
}

static void render_symbols(uint8_t *frame, const c1_ui_state *state)
{
    c1_canvas_fill_rect(frame, 8, 85, 280, 47, false);
    c1_canvas_stroke_rect(frame, 8, 85, 280, 47, 1, true);
    for (unsigned i = 0; i < C1_UI_EXTENDED_SYMBOL_COUNT; ++i) {
        unsigned x = 12 + (i % 6) * 46, y = 88 + (i / 6) * 21;
        bool selected = state->symbol_selection == i;
        char label[2] = {c1_ui_extended_symbol(i), 0};
        c1_canvas_fill_rect(frame, x, y, 42, 19, selected);
        text(frame, (int)x + 17, (int)y + 1, label, 8, !selected);
    }
}

static void render_password(uint8_t *frame, const c1_ui_state *state)
{
    title(frame, TR("连接网络", "Join network"), state->secret_visible ? TR("明文", "Visible") : TR("隐藏", "Hidden"));
    text(frame, 8, 42, state->selected_ssid, 280, true);
    c1_canvas_stroke_rect(frame, 8, 62, 280, 21, 1, true);
    char shown[34];
    size_t len = state->secret_length < 33 ? state->secret_length : 33;
    if (state->secret_visible) memcpy(shown, state->secret + state->secret_length - len, len);
    else memset(shown, '*', len);
    shown[len] = 0;
    text(frame, 12, 64, shown, 272, true);
    if (state->keyboard_layer == C1_UI_KEYBOARD_SYMBOLS) render_symbols(frame, state);
    else {
        text(frame, 8, 91, TR("Shift 字符层  表情 显示/隐藏", "Shift: abc/ABC/123  Emoji: mask"), 280, true);
        text(frame, 8, 113, state->wifi_notice[0] ? notice(state, state->wifi_notice) : TR("Enter 连接  Back 取消", "Enter: connect  Back: cancel"), 280, true);
    }
    footer(frame, state->keyboard_layer == C1_UI_KEYBOARD_SYMBOLS ? TR("方向选符号  OK 输入  Enter 连接", "Arrows / OK: symbol  Enter: join") :
                    TR("密码直接输入，不记录学习", "Password: direct input, no learning"));
}

static void utf8_encode(uint32_t cp, char out[5])
{
    memset(out, 0, 5);
    if (cp < 0x80) out[0] = (char)cp;
    else if (cp < 0x800) { out[0] = (char)(0xc0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { out[0] = (char)(0xe0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 63)); out[2] = (char)(0x80 | (cp & 63)); }
    else { out[0] = (char)(0xf0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 63)); out[2] = (char)(0x80 | ((cp >> 6) & 63)); out[3] = (char)(0x80 | (cp & 63)); }
}

static void render_terminal(uint8_t *frame, const c1_ui_state *state, c1_terminal_screen *terminal)
{
    const unsigned y = C1_CHROME_HEIGHT;
    if (!terminal) { text(frame, 0, (int)y, TR("正在启动终端", "Starting terminal"), 296, true); return; }
    const c1_terminal_cell *cells = c1_terminal_screen_cells(terminal);
    if (!cells) return;
    const unsigned cw = 8U, ch = 16U;
    const unsigned cols = C1_CHROME_TERMINAL_COLUMNS, rows = C1_CHROME_TERMINAL_ROWS;
    for (unsigned r = 0; r < rows && r < terminal->rows; ++r) {
        for (unsigned c = 0; c < cols && c < terminal->columns; ++c) {
            const c1_terminal_cell *cell = &cells[r * C1_TERMINAL_COLUMNS + c];
            if (cell->continuation) continue;
            unsigned width = cell->width == 2 ? cw * 2U : cw;
            unsigned px = c * cw, py = y + r * ch;
            if (c + (width / cw) > cols) continue;
            if (cell->inverse) c1_canvas_fill_rect(frame, px, py, width, ch, true);
            char glyph[5]; utf8_encode(cell->codepoint ? cell->codepoint : ' ', glyph);
            c1pkg_text(frame, (int)px, (int)py, glyph, (int)width, !cell->inverse);
            if (cell->underline) c1_canvas_fill_rect(frame, px, py + ch - 1U, width, 1, !cell->inverse);
        }
    }
    if (state->terminal_symbol_picker) render_symbols(frame, state);
}

static void graph_line(uint8_t *frame, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    int dx = abs((int)x1 - (int)x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs((int)y1 - (int)y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        c1_canvas_fill_rect(frame, x0, y0, 1U, 1U, true);
        if (x0 == x1 && y0 == y1) break;
        int doubled = 2 * error;
        if (doubled >= dy) { error += dy; x0 = (unsigned)((int)x0 + sx); }
        if (doubled <= dx) { error += dx; y0 = (unsigned)((int)y0 + sy); }
    }
}

static unsigned battery_graph_y(unsigned percent)
{
    return 111U - (percent > 100U ? 100U : percent) * 66U / 100U;
}

static void battery_hour(char out[6], int64_t timestamp, int offset)
{
    unsigned minute = (unsigned)(((timestamp / 60 + offset) % 1440 + 1440) % 1440);
    snprintf(out, 6, "%02u:%02u", (minute / 60) % 24U, minute % 60U);
}

static void render_battery(uint8_t *frame, const c1_ui_state *state, const c1_ui_status *status)
{
    const c1_battery_history *h = &status->battery_history;
    size_t count = h->count < C1_BATTERY_HISTORY_POINTS ? h->count : C1_BATTERY_HISTORY_POINTS;
    char detail[96];
    unsigned view = (unsigned)state->battery_view <= C1_BATTERY_VIEW_MINUTE ? (unsigned)state->battery_view : 0U;
    static const int64_t windows[] = {24 * 3600, 6 * 3600, 3600};
    static const char *zh[] = {"日", "时", "分"};
    static const char *en[] = {"Day", "Hour", "Min"};
    for (unsigned i = 0; i < 3U; ++i) {
        unsigned x = 8U + i * 52U;
        if (i == view) c1_ui_focus_frame(frame, C1_DISPLAY_WIDTH, C1_DISPLAY_HEIGHT, x, 20U, 48U, 19U, true);
        text(frame, (int)x + 8, 22, TR(zh[i], en[i]), 32, true);
    }
    text(frame, 178, 22, TR("音量选视图", "Vol +/-: view"), 110, true);
    int64_t window = windows[view];
    if (!count) {
        text(frame, 8, 65, status->time_available ? TR("等待有效电量采样", "Waiting for battery sample") :
             TR("请先设置日期或联网校时", "Set date or connect to sync"), 280, true);
        footer(frame, TR("左右选采样点  音量选日/时/分", "Left/Right: sample  Vol +/-: view"));
        return;
    }
    size_t selected = c1_ui_battery_selected(state, status);
    const c1_battery_sample *point = &h->samples[selected];
    time_t adjusted = (time_t)(point->timestamp + state->preferences.utc_offset_minutes * 60);
    struct tm date;
    char stamp[24] = "--/-- --:--";
    if (gmtime_r(&adjusted, &date)) (void)strftime(stamp, sizeof(stamp), "%m/%d %H:%M", &date);
    snprintf(detail, sizeof(detail), "%s %u%% %s", stamp, (unsigned)point->percent,
             point->power == C1_BATTERY_PLUGGED ? TR("插电", "Plugged") :
             point->power == C1_BATTERY_DISCHARGING ? TR("电池", "Battery") : TR("未知", "Unknown"));

    /* The three views zoom the same minute history: one day, six hours, or
     * one hour. Hardware is read every ten seconds, while this graph receives
     * at most one real sample per minute. */
    int64_t end = status->battery_history_now;
    if (end < h->samples[count - 1].timestamp) end = h->samples[count - 1].timestamp;
    /* Minute-aligned window avoids pointless horizontal motion on every
     * sensor read. Pan to an older selected point instead of clamping all
     * off-screen samples into a false vertical line at the left edge. */
    end = end / 60 * 60 + 59;
    if (point->timestamp < end - window) end = point->timestamp + window / 2;
    int64_t begin = end - window;
    int64_t span = window;
    text(frame, 0, 40, "100", 24, true);
    text(frame, 8, 69, "50", 16, true);
    text(frame, 16, 104, "0", 8, true);
    c1_canvas_fill_rect(frame, 28, 40, 1, 73, true);
    line(frame, 28, 113, 260);
    for (unsigned x = 33; x < 288; x += 8) line(frame, x, battery_graph_y(50), 2);
    unsigned selected_x = 33U, selected_y = battery_graph_y(point->percent);
    unsigned previous_x = 0U, previous_y = 0U;
    bool previous_visible = false;
    for (size_t i = 0; i < count; ++i) {
        int64_t elapsed = h->samples[i].timestamp - begin;
        if (elapsed < 0 || elapsed > span) { previous_visible = false; continue; }
        unsigned x = 33U + (unsigned)(elapsed * 252 / span);
        unsigned y = battery_graph_y(h->samples[i].percent);
        if (previous_visible && h->samples[i].connected)
            graph_line(frame, previous_x, previous_y, x, y);
        c1_canvas_fill_rect(frame, x, y, 1U, 1U, true);
        if (i == selected) { selected_x = x; selected_y = y; }
        previous_x = x;
        previous_y = y;
        previous_visible = true;
    }
    /* One real sample per minute, no per-point 5px symbols to erase adjacent
     * points in the day view. Supply state is written in the detail footer. */
    c1_ui_focus_frame(frame, C1_DISPLAY_WIDTH, C1_DISPLAY_HEIGHT,
                      selected_x - 4U, selected_y - 4U, 9U, 9U, true);
    char start_text[6], middle_text[6], end_text[6];
    battery_hour(start_text, begin, state->preferences.utc_offset_minutes);
    battery_hour(middle_text, begin + span / 2, state->preferences.utc_offset_minutes);
    battery_hour(end_text, end, state->preferences.utc_offset_minutes);
    if (view == C1_BATTERY_VIEW_DAY) {
        time_t first = (time_t)(begin + state->preferences.utc_offset_minutes * 60);
        time_t last = (time_t)(end + state->preferences.utc_offset_minutes * 60);
        if (gmtime_r(&first, &date)) (void)strftime(start_text, sizeof(start_text), "%m/%d", &date);
        if (gmtime_r(&last, &date)) (void)strftime(end_text, sizeof(end_text), "%m/%d", &date);
    }
    text(frame, 28, 116, start_text, 40, true);
    text(frame, 140, 116, middle_text, 40, true);
    text(frame, 248, 116, end_text, 40, true);
    /* The bottom status line is the selected point; no redundant legend. */
    footer(frame, detail);
}


static void render_settings(uint8_t *frame, const c1_ui_state *state, const c1_ui_status *status)
{
    title(frame, state->selection == C1_SETTING_POWER_MODE ? TR("插电不自动锁屏", "Plugged in: stays awake") :
          status->time_sync_running ? TR("正在校时", "Syncing time") :
          status->time_sync_ok ? TR("时间已同步", "Time synced") : TR("等待联网校时", "Waiting for network time"), NULL);
    static const char *zh[] = {"界面语言", "电源模式", "锁屏样式", "时区", "联网校时", "锁屏文字", "系统更新", "关于"};
    static const char *en[] = {"Language", "Power mode", "Lock screen", "Time zone", "Network time", "Lock text", "System update", "About"};
    static const char *modes_zh[] = {"省电", "标准", "性能"};
    static const char *modes_en[] = {"Saving", "Standard", "Performance"};
    const c1_preferences *p = &state->preferences;
    unsigned mode = (unsigned)p->power_mode <= C1_POWER_MODE_PERFORMANCE ? (unsigned)p->power_mode : C1_POWER_MODE_STANDARD;
    unsigned first = state->selection / 4 * 4;
    for (unsigned i = first; i < first + 4 && i < C1_SETTING_COUNT; ++i) {
        unsigned y = 43 + (i - first) * 22;
        char value[64];
        if (i == C1_SETTING_LANGUAGE) snprintf(value, sizeof(value), "%s", p->language == C1_LANGUAGE_EN ? "English" : "简体中文");
        else if (i == C1_SETTING_POWER_MODE) snprintf(value, sizeof(value), "%s", TR(modes_zh[mode], modes_en[mode]));
        else if (i == C1_SETTING_LOCK_STYLE) {
            static const char *styles_zh[] = {"图片", "保留画面", "文字", "月历"};
            static const char *styles_en[] = {"Picture", "Freeze", "Text", "Calendar"};
            snprintf(value, sizeof(value), "%s", TR(styles_zh[p->lock_style], styles_en[p->lock_style]));
        } else if (i == C1_SETTING_TIME_ZONE) snprintf(value, sizeof(value), "UTC%c%02d:%02d", p->utc_offset_minutes < 0 ? '-' : '+', abs(p->utc_offset_minutes)/60, abs(p->utc_offset_minutes)%60);
        else if (i == C1_SETTING_NETWORK_TIME) snprintf(value, sizeof(value), "%s", p->network_time ? TR("开启", "On") : TR("关闭", "Off"));
        else if (i == C1_SETTING_LOCK_TEXT) snprintf(value, sizeof(value), "%s", TR("编辑", "Edit"));
        else if (i == C1_SETTING_ABOUT) snprintf(value, sizeof(value), "%s", C1_VERSION);
        else snprintf(value, sizeof(value), "%s", status->update_prepared ? TR("确认更新", "Ready") :
                      status->service_busy ? TR("处理中", "Working") :
                      status->update_available ? TR("有新版本", "New version") : TR("检查更新", "Check"));
        bool selected = state->selection == i;
        c1_canvas_fill_rect(frame, 8, y, 280, 21, selected);
        text(frame, 14, (int)y + 2, TR(zh[i], en[i]), 142, !selected);
        text(frame, 288 - c1pkg_text_width(value) - 4, (int)y + 2, value, 124, !selected);
    }
    static const char *rules_zh[] = {"闲置1分锁屏 再2分关机", "闲置3分锁屏 再5分关机", "闲置5分锁屏休眠 不关机"};
    static const char *rules_en[] = {"Idle 1m: lock; off 2m later", "Idle 3m: lock; off 5m later", "Idle 5m: lock/sleep; never off"};
    footer(frame, state->wifi_notice[0] ? state->wifi_notice :
        state->selection == C1_SETTING_POWER_MODE ? TR(rules_zh[mode], rules_en[mode]) :
        state->selection == C1_SETTING_ABOUT ? TR("确认查看系统版本", "OK: system version") :
        state->selection == C1_SETTING_UPDATE ? TR("确认检查/更新  返回退出", "OK: check / update  Back: exit") :
        TR("上下选择  左右修改  Home 返回", "Up/down: row  Left/right: change"));
}

static void render_calendar(uint8_t *frame, const c1_ui_state *state, const c1_ui_status *status)
{
    char month[40];
    if (!status->time_available || status->month < 1 || status->month > 12 || !status->day) {
        text(frame, 24, 64, TR("请先设置日期或联网校时", "Set date or connect to sync"), 256, true); return;
    }
    snprintf(month, sizeof(month), "%04u / %02u", status->year, status->month);
    text(frame, (296 - c1pkg_text_width(month)) / 2, 5, month, 280, true);
    const char *week[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    const char *week_zh[] = {"一", "二", "三", "四", "五", "六", "日"};
    for (unsigned i = 0; i < 7; ++i) text(frame, 17 + (int)i * 40, 25, TR(week_zh[i], week[i]), 24, true);
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned count = days[status->month - 1];
    if (status->month == 2 && status->year % 4 == 0 && (status->year % 100 != 0 || status->year % 400 == 0)) ++count;
    unsigned first = ((status->weekday + 6) % 7 + 35 - ((status->day - 1) % 7)) % 7;
    for (unsigned d = 1; d <= count; ++d) {
        unsigned pos = first + d - 1, x = 10 + (pos % 7) * 40, y = 46 + (pos / 7) * 17;
        bool today = d == status->day;
        c1_canvas_fill_rect(frame, x, y, 32, 16, today);
        char value[4]; snprintf(value, sizeof(value), "%u", d);
        text(frame, (int)x + (32 - c1pkg_text_width(value)) / 2, (int)y, value, 32, !today);
    }
}

static bool lock_text_layout_at(const char *value, unsigned height, c1_lock_text_layout *layout)
{
    const char *cursor = value;
    const unsigned budget = 284U * 16U / height;
    memset(layout, 0, sizeof(*layout));
    layout->font_height = height;
    while (*cursor) {
        while (*cursor == ' ') ++cursor;
        if (!*cursor) break;
        if (layout->line_count == C1_LOCK_TEXT_MAX_LINES) return false;
        const char *start = cursor, *word_break = NULL;
        unsigned used = 0U;
        while (*cursor) {
            unsigned char first = (unsigned char)*cursor;
            size_t bytes = first < 0x80U ? 1U : first < 0xe0U ? 2U : first < 0xf0U ? 3U : 4U;
            char glyph[5] = {0};
            memcpy(glyph, cursor, bytes); /* Complete string was validated first. */
            unsigned advance = (unsigned)c1pkg_text_width(glyph);
            if (used + advance > budget) break;
            if (*cursor == ' ' && cursor != start) word_break = cursor;
            used += advance;
            cursor += bytes;
        }
        if (cursor == start) return false;
        const char *end = cursor;
        /* Prefer a complete English word when the next word crosses the line.
         * Long words and Chinese still wrap at complete glyph boundaries. */
        if (*cursor && *cursor != ' ' && word_break) end = cursor = word_break;
        while (end > start && end[-1] == ' ') --end;
        char line[C1_LOCK_TEXT_BYTES] = {0};
        size_t length = (size_t)(end - start);
        memcpy(line, start, length);
        unsigned row = layout->line_count++;
        layout->offsets[row] = (size_t)(start - value);
        layout->lengths[row] = length;
        layout->widths[row] = ((unsigned)c1pkg_text_width(line) * height + 15U) / 16U;
        layout->block_height = layout->line_count * height + (layout->line_count - 1U) * (height / 16U);
        if (layout->block_height > 144U) return false;
    }
    return layout->line_count != 0U;
}

bool c1_ui_layout_lock_text(const char *value, c1_lock_text_layout *layout)
{
    if (!layout) return false;
    memset(layout, 0, sizeof(*layout));
    if (!c1_preferences_valid_text(value)) return false;
    /* Whole destination-pixel heights permit fractional 16px font scaling:
     * long text can use 19/23/28px instead of abruptly dropping to 16px. */
    for (unsigned height = 144U; height >= 16U; --height)
        if (lock_text_layout_at(value, height, layout)) return true;
    memset(layout, 0, sizeof(*layout));
    return false;
}

static void render_lock_text(uint8_t *frame, const c1_ui_state *state)
{
    c1_lock_text_layout layout;
    if (!c1_ui_layout_lock_text(state->preferences.lock_text, &layout)) return;
    uint8_t glyphs[C1_DISPLAY_FRAME_BYTES];
    unsigned top = (C1_DISPLAY_HEIGHT - layout.block_height) / 2U;
    for (unsigned row = 0U; row < layout.line_count; ++row) {
        char line[C1_LOCK_TEXT_BYTES] = {0};
        memcpy(line, state->preferences.lock_text + layout.offsets[row], layout.lengths[row]);
        memset(glyphs, 0, sizeof(glyphs));
        c1pkg_text(glyphs, 0, 0, line, 284, true);
        unsigned left = (C1_DISPLAY_WIDTH - layout.widths[row]) / 2U;
        for (unsigned y = 0U; y < layout.font_height; ++y) {
            unsigned source_y = y * 16U / layout.font_height;
            for (unsigned x = 0U; x < layout.widths[row]; ++x) {
                unsigned source_x = x * 16U / layout.font_height;
                if (glyphs[(source_y / 8U) * C1_DISPLAY_WIDTH + source_x] & (0x80U >> (source_y % 8U)))
                    c1_canvas_fill_rect(frame, left + x, top + y, 1U, 1U, true);
            }
        }
        top += layout.font_height + layout.font_height / 16U;
    }
}

/* Keep the insertion point visible, not merely the tail of the draft. Measure
 * the same glyph widths as the renderer and never clip a UTF-8 code point. */
bool c1_ui_layout_lock_text_edit(const c1_ui_state *state, c1_lock_text_edit_layout *layout)
{
    if (!layout) return false;
    memset(layout, 0, sizeof(*layout));
    if (!state || (state->lock_text_draft[0] && !c1_preferences_valid_text(state->lock_text_draft))) return false;
    size_t cursor = c1_ui_lock_text_cursor_offset(state), offset = 0U;
    char prefix[C1_LOCK_TEXT_BYTES] = {0};
    memcpy(prefix, state->lock_text_draft, cursor);
    while (c1pkg_text_width(prefix + offset) > (int)C1_LOCK_TEXT_EDIT_WIDTH) {
        ++offset;
        while (((unsigned char)prefix[offset] & 0xc0U) == 0x80U) ++offset;
    }
    layout->offset = offset;
    layout->cursor_x = (unsigned)c1pkg_text_width(prefix + offset);
    unsigned width = layout->cursor_x;
    size_t end = cursor;
    while (state->lock_text_draft[end]) {
        size_t next = end + 1U;
        while (((unsigned char)state->lock_text_draft[next] & 0xc0U) == 0x80U) ++next;
        char glyph[5] = {0};
        memcpy(glyph, state->lock_text_draft + end, next - end);
        unsigned added = (unsigned)c1pkg_text_width(glyph);
        if (width + added > C1_LOCK_TEXT_EDIT_WIDTH) break;
        width += added;
        end = next;
    }
    layout->length = end - offset;
    return true;
}

static void render_lock_text_edit(uint8_t *frame, const c1_ui_state *state)
{
    c1_lock_text_edit_layout layout;
    if (!c1_ui_layout_lock_text_edit(state, &layout)) return;
    size_t cursor = c1_ui_lock_text_cursor_offset(state);
    char prefix[C1_LOCK_TEXT_BYTES] = {0}, suffix[C1_LOCK_TEXT_BYTES] = {0};
    memcpy(prefix, state->lock_text_draft + layout.offset, cursor - layout.offset);
    memcpy(suffix, state->lock_text_draft + cursor, layout.offset + layout.length - cursor);
    c1_canvas_stroke_rect(frame, 8, 51, 280, 24, 1, true);
    text(frame, 12, 55, prefix, C1_LOCK_TEXT_EDIT_WIDTH, true);
    text(frame, 14 + (int)layout.cursor_x, 55, suffix, C1_LOCK_TEXT_EDIT_WIDTH - layout.cursor_x, true);
    c1_canvas_fill_rect(frame, 12U + layout.cursor_x, 55, 1, 16, true);
}

void c1_ui_render_input(uint8_t *frame, const c1_ui_state *state,
                        const struct c1_ime_response *view, bool enabled, bool failed)
{
    if ((!enabled && !failed) || (state->page != C1_UI_PAGE_TERMINAL && state->page != C1_UI_PAGE_LOCK_TEXT)) return;
    c1_canvas_fill_rect(frame, 0, 119, 296, 33, false);
    line(frame, 0, 119, 296);
    if (failed) {
        text(frame, 4, 120, TR("输入法不可用，未确认按键已丢弃", "IME unavailable; pending keys dropped"), 288, true);
        text(frame, 4, 136, TR("Shift+Space 重试 / 切换英文", "Shift+Space: retry / English"), 288, true);
        return;
    }
    text(frame, 4, 120, view->preedit[0] ? view->preedit : TR("中文  Shift+Space 切换英文", "Chinese  Shift+Space: English"), 288, true);
    for (unsigned i = 0; i < view->candidate_count && i < C1_IME_MAX_CANDIDATES; ++i) {
        char candidate[1060];
        snprintf(candidate, sizeof(candidate), "%u.%s", i + 1, view->candidates[i]);
        int x = 4 + (int)i * 59;
        bool highlighted = i == view->highlighted_candidate;
        if (highlighted) c1_canvas_fill_rect(frame, x - 1, 135, 58, 17, true);
        text(frame, x, 136, candidate, 56, !highlighted);
    }
}

void c1_ui_render(uint8_t *frame, const c1_ui_state *state, const c1_ui_status *status, c1_terminal_screen *terminal)
{
    if (!frame || !state || !status) return;
    c1_display_frame_clear(frame, false);
    if (state->page == C1_UI_PAGE_LOCK) {
        if (state->preferences.lock_style == C1_LOCK_CALENDAR) render_calendar(frame, state, status);
        else if (state->preferences.lock_style == C1_LOCK_TEXT) render_lock_text(frame, state);
        else memcpy(frame, c1_wallpaper_frame, C1_DISPLAY_FRAME_BYTES);
        return; /* Freeze is implemented by not submitting a frame in runtime. */
    }
    header(frame, state, status);
    switch (state->page) {
    case C1_UI_PAGE_DESKTOP: render_desktop(frame, state, status); break;
    case C1_UI_PAGE_WIFI: render_wifi(frame, state, status); break;
    case C1_UI_PAGE_WIFI_PASSWORD: render_password(frame, state); break;
    case C1_UI_PAGE_TERMINAL: render_terminal(frame, state, terminal); break;
    case C1_UI_PAGE_BATTERY: render_battery(frame, state, status); break;
    case C1_UI_PAGE_SETTINGS: render_settings(frame, state, status); break;
    case C1_UI_PAGE_ABOUT:
        title(frame, "C1 Slim", NULL);
        text(frame, 12, 55, TR("当前系统版本", "Current system version"), 272, true);
        text(frame, 12, 80, C1_VERSION, 272, true);
        footer(frame, TR("返回设置  Home 返回主页", "Back: settings  Home: desktop"));
        break;
    case C1_UI_PAGE_LOCK_TEXT: {
        title(frame, TR("锁屏文字", "Lock text"), NULL);
        render_lock_text_edit(frame, state);
        text(frame, 10, 81, TR("左右移动  确认选字 / 保存", "Left/Right: move  OK: select/save"), 276, true);
        /* Keep errors above the IME strip, which owns y=119..151. */
        text(frame, 10, 100, state->wifi_notice[0] ? state->wifi_notice :
             TR("Shift+Space 中英文  Back 取消", "Shift+Space: IME  Back: cancel"), 276, true);
        footer(frame, TR("确认保存  Back / Home 取消", "OK: save  Back / Home: cancel")); break;
    }
    case C1_UI_PAGE_LOCK: break;
    }
}
