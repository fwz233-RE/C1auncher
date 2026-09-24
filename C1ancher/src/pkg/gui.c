/* Native 296x152 package browser. The desktop PTY forwards keys only;
 * pixels go directly to the e-paper device under its exclusive lease. */
#include "metrics.h"
#include "gui.h"
#include "desktop.h"
#include "text.h"
#include "gui_locale.h"
#include "platform/app_lease.h"
#include "ui/input_method.h"
#include "ui/chrome.h"
#include "ui/focus.h"
#include "gui_refresh.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define GUI_WIDTH C1_CHROME_WIDTH
#define GUI_HEIGHT C1_CHROME_SCREEN_HEIGHT
#define GUI_BYTES (GUI_WIDTH * GUI_HEIGHT / 8U)
#define GUI_HEADER_HEIGHT C1_CHROME_HEIGHT
#define GUI_ROWS 5U
#define GUI_LIST_Y 37U
#define GUI_SEARCH_Y 36U
#define GUI_SEARCH_LIST_Y 54U
#define GUI_ROW_HEIGHT 19U
#define GUI_FOOTER_Y 135U

struct gui_state {
    struct c1pkg_index index;
    struct c1pkg_installed_list installed;
    struct c1pkg_download_list downloads;
    size_t visible[C1PKG_MAX_DOWNLOAD_ITEMS];
    size_t count, selected[2], offset[2];
    unsigned int tab; /* 0 local, 1 store */
    enum gui_language language;
    char query[C1PKG_NAME_MAX + 1U];
    /* Browse-mode keyboard prefix, separate from explicit IME search. */
    struct c1pkg_prefix jump_prefix;
    int search_editing, utc_offset;
    char clock[6], battery[8];
    uint64_t telemetry_at;
    struct c1pkg_metrics_snapshot metrics;
    struct c1pkg_metrics_job *metrics_job;
    uint64_t metrics_at;
    struct gui_refresh_job refresh;
    const struct c1pkg_config *config;
    char refresh_status[C1PKG_ERROR_MAX];
    c1_input_method input_method;
    struct c1_ime_response ime_view;
    int ime_notice, ime_ready;
    int storage_ready, verified, busy, cancelled;
    int run_lock, lease, display;
    char status[C1PKG_ERROR_MAX];
    char ime_error[C1PKG_ERROR_MAX]; /* A refresh result must not replace an input error. */
    char storage_error[C1PKG_ERROR_MAX];
    unsigned char frame[GUI_BYTES], previous[GUI_BYTES];
    int previous_valid, display_failed;
};

static void initialize_gui(struct gui_state *s)
{
    memset(s, 0, sizeof(*s));
    s->run_lock = s->lease = s->display = -1;
    s->tab = 0U; /* Offline local applications are immediately reachable. */
    gui_load_settings(GUI_DESKTOP_CONFIG, &s->language, &s->utc_offset);
    strcpy(s->clock, "--:--"); strcpy(s->battery, "--%");
    gui_refresh_init(&s->refresh);
    c1_input_method_init(&s->input_method);
}

static volatile sig_atomic_t interrupted;
static struct termios gui_saved_terminal;
static int gui_terminal_saved;
static void gui_signal(int number) { (void)number; interrupted = 1; }

enum gui_key {
    G_NONE = C1PKG_KEY_NONE, G_UP = C1PKG_KEY_UP, G_DOWN = C1PKG_KEY_DOWN,
    G_LEFT = C1PKG_KEY_LEFT, G_RIGHT = C1PKG_KEY_RIGHT,
    G_ENTER = C1PKG_KEY_ENTER, G_BACK = C1PKG_KEY_BACK,
    G_REFRESH = C1PKG_KEY_REFRESH,
    G_IME_TOGGLE = 512, G_HOME, G_SEARCH, G_PAGE_UP, G_PAGE_DOWN, G_MANAGE
};

/* PTY modifier reporting is explicit: CSI-u or xterm modifyOtherKeys. Read
 * escape sequences incrementally, never wait for a suffix in the IME loop. */
static struct { char sequence[32]; size_t used; uint64_t deadline; } gui_keyboard;

static int gui_sequence_key(const char *sequence)
{
    if (!strcmp(sequence, "[32;2u") || !strcmp(sequence, "[27;2;32~")) return G_IME_TOGGLE;
    /* The desktop emits this only for a standalone Shift release in c1pkg GUI,
     * never for Shift+Space/numeric keycaps or a user terminal session. */
    if (!strcmp(sequence, "[57441u")) return G_MANAGE;
    if (!strcmp(sequence, "[H") || !strcmp(sequence, "OH") ||
        !strcmp(sequence, "[1~") || !strcmp(sequence, "[7~")) return G_HOME;
    if (!strcmp(sequence, "[A") || !strcmp(sequence, "OA")) return G_UP;
    if (!strcmp(sequence, "[B") || !strcmp(sequence, "OB")) return G_DOWN;
    if (!strcmp(sequence, "[C") || !strcmp(sequence, "OC")) return G_RIGHT;
    if (!strcmp(sequence, "[D") || !strcmp(sequence, "OD")) return G_LEFT;
    if (!strcmp(sequence, "[5~")) return G_PAGE_UP;
    if (!strcmp(sequence, "[6~")) return G_PAGE_DOWN;
    /* Shift plus an explicitly reported numeric keycap retains direct choice.
     * Device letter-keycaps already arrive as the mapped ASCII digit. */
    for (unsigned int key = '1'; key <= '5'; ++key) {
        char csi[24], xterm[24];
        snprintf(csi, sizeof(csi), "[%u;2u", key);
        snprintf(xterm, sizeof(xterm), "[27;2;%u~", key);
        if (!strcmp(sequence, csi) || !strcmp(sequence, xterm)) return (int)key;
    }
    return G_NONE;
}

static int gui_key(int timeout)
{
    for (;;) {
        struct pollfd input = {STDIN_FILENO, POLLIN, 0};
        unsigned char byte;
        uint64_t now = c1pkg_input_now_ms();
        int wait = timeout, ready;
        if (interrupted) return G_HOME;
        /* A late UI iteration is not a late PTY suffix. Probe readable bytes
         * first, even after the nominal 40ms deadline (e.g. a slow redraw). */
        if (gui_keyboard.used) {
            uint64_t remaining = gui_keyboard.deadline > now ? gui_keyboard.deadline - now : 0U;
            if (wait < 0 || (uint64_t)wait > remaining) wait = (int)remaining;
        }
        ready = poll(&input, 1U, wait);
        if (ready < 0 && errno == EINTR) return G_NONE;
        if (ready == 0) {
            if (gui_keyboard.used && c1pkg_input_now_ms() >= gui_keyboard.deadline) {
                int key = gui_keyboard.used == 1U ? G_BACK : G_NONE;
                gui_keyboard.used = 0U;
                return key;
            }
            if (timeout != 0 && gui_keyboard.used) continue;
            return G_NONE;
        }
        if (ready < 0 || !(input.revents & POLLIN) || read(STDIN_FILENO, &byte, 1U) != 1) {
            interrupted = 1; return G_HOME;
        }
        if (gui_keyboard.used) {
            size_t used = gui_keyboard.used;
            if (used == 1U && byte != '[' && byte != 'O') {
                gui_keyboard.used = 0U; return G_NONE;
            }
            if (used >= sizeof(gui_keyboard.sequence)) {
                /* Drain the whole overlong CSI; never reinterpret its numeric
                 * suffix as application-search or command keystrokes. */
                gui_keyboard.deadline = c1pkg_input_now_ms() + 40U;
                if (byte >= 0x40U && byte <= 0x7eU) { gui_keyboard.used = 0U; return G_NONE; }
                continue;
            }
            gui_keyboard.sequence[used - 1U] = (char)byte;
            gui_keyboard.sequence[used] = '\0';
            ++gui_keyboard.used;
            gui_keyboard.deadline = c1pkg_input_now_ms() + 40U;
            if (used > 1U && byte >= 0x40U && byte <= 0x7eU) {
                int key = gui_sequence_key(gui_keyboard.sequence);
                gui_keyboard.used = 0U;
                if (key == G_HOME) interrupted = 1;
                return key;
            }
            continue;
        }
        if (byte == 27U) {
            gui_keyboard.used = 1U; gui_keyboard.deadline = c1pkg_input_now_ms() + 40U; continue;
        }
        if (byte == 3U) { interrupted = 1; return G_HOME; }
        if (byte == '\r' || byte == '\n') return G_ENTER;
        if (byte == '\t') return G_SEARCH; /* Legacy/external PTY search alias. */
        if (byte == ' ') return G_REFRESH;
        if (byte == 8U || byte == 127U) return C1PKG_KEY_ERASE;
        if (byte == 21U) return C1PKG_KEY_CLEAR;
        return byte >= 33U && byte <= 126U ? (int)byte : G_NONE;
    }
}

static int gui_ime_visible(const struct gui_state *s)
{
    return s->input_method.enabled || s->input_method.count || s->input_method.client.pending_sequence ||
           s->ime_view.preedit[0] || s->ime_view.candidate_count;
}

static int gui_search_visible(const struct gui_state *s)
{
    return s->search_editing || s->query[0] || gui_ime_visible(s);
}

static unsigned int gui_list_y(const struct gui_state *s)
{
    return gui_search_visible(s) ? GUI_SEARCH_LIST_Y : GUI_LIST_Y;
}

static size_t gui_rows(const struct gui_state *s)
{
    return gui_ime_visible(s) ? 2U : gui_search_visible(s) ? 4U : GUI_ROWS;
}

static void gui_ime_close(struct gui_state *s)
{
    c1_input_method_close(&s->input_method);
    memset(&s->ime_view, 0, sizeof(s->ime_view));
    s->ime_ready = 0;
}

static void gui_ime_error(struct gui_state *s)
{
    gui_ime_close(s);
    s->ime_notice = 1;
    snprintf(s->status, sizeof(s->status), "%s", GUI_LABEL(s, "输入法失败 ASCII输入", "IME failed; ASCII input"));
    strcpy(s->ime_error, s->status);
}

/* Dialogs/operations accept Q and physical erase as cancel. List search has
 * its own explicit focus and never interprets typed letters as commands. */
static int gui_cancel_key(int key)
{
    return key == G_BACK || key == G_HOME || key == C1PKG_KEY_ERASE || key == 'q' || key == 'Q';
}

static void box(struct gui_state *s, unsigned int x, unsigned int y,
                unsigned int width, unsigned int height, bool black)
{
    unsigned int xx, yy;
    for (yy = y; yy < y + height && yy < GUI_HEIGHT; ++yy)
        for (xx = x; xx < x + width && xx < GUI_WIDTH; ++xx) {
            size_t index = (yy / 8U) * GUI_WIDTH + xx;
            unsigned char bit = (unsigned char)(0x80U >> (yy % 8U));
            if (black) s->frame[index] |= bit;
            else s->frame[index] &= (unsigned char)~bit;
        }
}

/* Clip at complete UTF-8 glyph boundaries with a visible ASCII ellipsis.
 * Measure through the font API: unsupported Unicode is an 8px replacement,
 * not necessarily a 16px glyph. Metadata itself is never changed. */
static void gui_fit_text(char *out, size_t size, const char *value, unsigned int width)
{
    size_t used = 0U;
    unsigned int pixels = 0U;
    int clipped;
    if (size == 0U) return;
    out[0] = '\0';
    if (value == NULL) return;
    clipped = c1pkg_text_width(value) > (int)width || strlen(value) >= size;
    if (clipped && (width < 24U || size < 4U)) return;
    if (clipped) width -= 24U;
    while (*value != '\0') {
        char glyph[5] = "";
        unsigned char first = (unsigned char)*value;
        size_t length = first < 0x80U ? 1U : first >= 0xc2U && first <= 0xdfU ? 2U :
                        first >= 0xe0U && first <= 0xefU ? 3U : first >= 0xf0U && first <= 0xf4U ? 4U : 0U;
        size_t i;
        int advance;
        if (length == 0U) break;
        for (i = 1U; i < length; ++i)
            if (((unsigned char)value[i] & 0xc0U) != 0x80U) break;
        if (i != length) break;
        memcpy(glyph, value, length);
        advance = c1pkg_text_width(glyph);
        if (advance <= 0 || pixels + (unsigned int)advance > width ||
            used + length + (clipped ? 3U : 0U) >= size) break;
        memcpy(out + used, value, length);
        used += length;
        pixels += (unsigned int)advance;
        value += length;
    }
    if (clipped) { memcpy(out + used, "...", 3U); used += 3U; }
    out[used] = '\0';
}

static void text(struct gui_state *s, unsigned int x, unsigned int y,
                 const char *value, unsigned int width, bool black)
{
    char fitted[C1PKG_ERROR_MAX];
    gui_fit_text(fitted, sizeof(fitted), value, width);
    c1pkg_text(s->frame, x, y, fitted, width, black);
}

static size_t gui_tab_count(const struct gui_state *s, unsigned int tab)
{
    size_t count = 0U;
    if (tab == 0U) {
        for (size_t i = 0U; i < s->installed.count; ++i)
            if (!c1pkg_is_internal_id(s->installed.items[i].id)) ++count;
    } else {
        for (size_t i = 0U; i < s->index.count; ++i)
            if (!c1pkg_is_internal_id(s->index.packages[i].id)) ++count;
    }
    return count;
}

static void gui_list_title(const struct gui_state *s, char *title, size_t capacity)
{
    bool known = s->tab == 0U ? s->storage_ready : s->index.sequence != 0;
    const char *scope = s->tab == 0U ? GUI_LABEL(s, "本地", "Local") :
                       GUI_LABEL(s, "在线", "Store");
    if (known) snprintf(title, capacity, GUI_LABEL(s, "%s%zu个应用", "%s %zu apps"), scope, gui_tab_count(s, s->tab));
    else snprintf(title, capacity, GUI_LABEL(s, "%s数量未知", "%s count unknown"), scope);
}

static void render_header(struct gui_state *s, const char *context)
{
    const char *name = context && *context ? context : GUI_LABEL(s, "应用", "Apps");
    c1_chrome_header(s->frame, name, s->clock, s->battery);
}

static void gui_telemetry_tick(struct gui_state *s, uint64_t now)
{
    gui_clock_text(s->clock, time(NULL), s->utc_offset);
    if (now >= s->telemetry_at) {
        gui_battery_text(s->battery, "/sys/class/power_supply/battery/capacity");
        s->telemetry_at = now + 30000U;
    }
}

static void render_footer(struct gui_state *s, const char *hint)
{
    box(s, 6U, 132U, 284U, 1U, true);
    text(s, 6U, GUI_FOOTER_Y, hint, 284U, true);
}

static void present(struct gui_state *s)
{
    ssize_t written;
    if (s->display < 0 || s->display_failed) return;
    if (s->previous_valid && memcmp(s->frame, s->previous, GUI_BYTES) == 0) return;
    do { written = write(s->display, s->frame, GUI_BYTES); } while (written < 0 && errno == EINTR && !interrupted);
    if (written != (ssize_t)GUI_BYTES) { s->display_failed = 1; return; }
    /* Use the driver's normal fast write during countdown; a full refresh is
     * only requested on first paint, not once per second. */
    if (!s->previous_valid) {
        int fd = open("/sys/devices/platform/e0266a128/epaper/refresh", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t refreshed = write(fd, "1", 1U);
            if (refreshed != 1) fprintf(stderr, "c1pkg: full refresh unavailable\n");
            (void)close(fd);
        }
    }
    memcpy(s->previous, s->frame, GUI_BYTES);
    s->previous_valid = 1;
}

static const char *state_label(const struct gui_state *s, enum c1pkg_download_status status)
{
    switch (status) {
    case C1PKG_DOWNLOAD_NOT_INSTALLED: return GUI_LABEL(s, "可安装", "Get");
    case C1PKG_DOWNLOAD_CURRENT: return GUI_LABEL(s, "已安装", "Installed");
    case C1PKG_DOWNLOAD_UPDATE_AVAILABLE: return GUI_LABEL(s, "可更新", "Update");
    case C1PKG_DOWNLOAD_INSTALLED_NEWER: return GUI_LABEL(s, "本地较新", "Local newer");
    case C1PKG_DOWNLOAD_VERSION_UNKNOWN: return GUI_LABEL(s, "版本未知", "Unknown");
    case C1PKG_DOWNLOAD_REMOVED: return GUI_LABEL(s, "本地应用", "Local only");
    }
    return GUI_LABEL(s, "未知", "Unknown");
}

static void gui_metrics_row_label(const struct gui_state *s, const char *id,
                                   char *out, size_t capacity)
{
    uint64_t count;
    if (!c1pkg_metrics_lookup(&s->metrics, id, &count)) {
        snprintf(out, capacity, "--");
    } else if (s->language == GUI_ZH) {
        snprintf(out, capacity, "%llu次", (unsigned long long)count);
    } else {
        snprintf(out, capacity, "%llu", (unsigned long long)count);
    }
}

static unsigned char gui_fold(unsigned char ch)
{
    return ch >= 'A' && ch <= 'Z' ? (unsigned char)(ch + 'a' - 'A') : ch;
}
static int gui_contains(const char *value, const char *query)
{
    size_t length = strlen(query);
    if (!length) return 1;
    for (; *value; ++value) {
        size_t i = 0U;
        while (i < length && value[i] && gui_fold((unsigned char)value[i]) == gui_fold((unsigned char)query[i])) ++i;
        if (i == length) return 1;
    }
    return 0;
}

static void select_visible(struct gui_state *s);

/* Device keyboards provide ASCII letters. These explicit first-character
 * mappings make Chinese display names reachable without requiring a Chinese
 * search composition; IDs remain an ASCII fallback for names without a map. */
struct gui_pinyin_initial {
    const char *character;
    char initial;
};

static char gui_han_initial(const char *value)
{
    static const struct gui_pinyin_initial mappings[] = {
        {"阿", 'a'}, {"爱", 'a'}, {"安", 'a'}, {"八", 'b'}, {"白", 'b'}, {"百", 'b'},
        {"备", 'b'}, {"笔", 'b'}, {"播", 'b'}, {"北", 'b'}, {"本", 'b'}, {"包", 'b'},
        {"查", 'c'}, {"测", 'c'}, {"常", 'c'}, {"成", 'c'}, {"出", 'c'}, {"词", 'c'},
        {"车", 'c'}, {"春", 'c'}, {"大", 'd'}, {"导", 'd'}, {"地", 'd'}, {"点", 'd'},
        {"电", 'd'}, {"读", 'd'}, {"端", 'd'}, {"动", 'd'}, {"典", 'd'}, {"俄", 'e'},
        {"发", 'f'}, {"法", 'f'}, {"方", 'f'}, {"繁", 'f'}, {"放", 'f'}, {"服", 'f'},
        {"复", 'f'}, {"歌", 'g'}, {"工", 'g'}, {"国", 'g'}, {"规", 'g'}, {"汉", 'h'},
        {"好", 'h'}, {"后", 'h'}, {"画", 'h'}, {"话", 'h'}, {"华", 'h'}, {"机", 'j'},
        {"简", 'j'}, {"建", 'j'}, {"交", 'j'}, {"记", 'j'}, {"进", 'j'}, {"家", 'j'},
        {"教", 'j'}, {"开", 'k'}, {"看", 'k'}, {"科", 'k'}, {"空", 'k'}, {"录", 'l'},
        {"联", 'l'}, {"量", 'l'}, {"旅", 'l'}, {"乐", 'y'}, {"流", 'l'},
        {"浏", 'l'}, {"吗", 'm'}, {"美", 'm'}, {"面", 'm'}, {"民", 'm'}, {"模", 'm'},
        {"目", 'm'}, {"内", 'n'}, {"你", 'n'}, {"年", 'n'}, {"鸟", 'n'}, {"配", 'p'},
        {"朋", 'p'}, {"片", 'p'}, {"器", 'q'}, {"气", 'q'}, {"起", 'q'}, {"前", 'q'},
        {"清", 'q'}, {"全", 'q'}, {"日", 'r'}, {"入", 'r'}, {"软", 'r'}, {"认", 'r'},
        {"设", 's'}, {"生", 's'}, {"声", 's'}, {"时", 's'}, {"试", 's'}, {"书", 's'},
        {"输", 's'}, {"视", 's'}, {"搜", 's'}, {"算", 's'}, {"四", 's'}, {"速", 's'},
        {"天", 't'}, {"题", 't'}, {"体", 't'}, {"听", 't'}, {"图", 't'}, {"统", 't'},
        {"外", 'w'}, {"网", 'w'}, {"文", 'w'}, {"无", 'w'}, {"我", 'w'}, {"玩", 'w'},
        {"下", 'x'}, {"现", 'x'}, {"相", 'x'}, {"新", 'x'}, {"系", 'x'}, {"小", 'x'},
        {"写", 'x'}, {"学", 'x'}, {"选", 'x'}, {"讯", 'x'}, {"应", 'y'}, {"英", 'y'},
        {"影", 'y'}, {"用", 'y'}, {"游", 'y'}, {"邮", 'y'}, {"云", 'y'}, {"阅", 'y'},
        {"语", 'y'}, {"在", 'z'}, {"照", 'z'}, {"知", 'z'}, {"支", 'z'}, {"置", 'z'},
        {"中", 'z'}, {"终", 'z'}, {"桌", 'z'}, {"字", 'z'}, {"资", 'z'}, {"作", 'z'},
        {"组", 'z'}, {"站", 'z'}, {"账", 'z'}
    };
    for (size_t i = 0U; i < sizeof(mappings) / sizeof(mappings[0]); ++i)
        if (!strncmp(value, mappings[i].character, strlen(mappings[i].character))) return mappings[i].initial;
    return '\0';
}

static int gui_matches_initial(const char *value, char initial)
{
    unsigned char first;
    if (value == NULL || value[0] == '\0') return 0;
    first = (unsigned char)value[0];
    if ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z'))
        return gui_fold(first) == (unsigned char)initial;
    return gui_han_initial(value) == initial;
}

static void gui_jump_clear(struct gui_state *s)
{
    c1pkg_prefix_clear(&s->jump_prefix);
}

static int gui_item_prefix_rank(const struct c1pkg_download_item *item,
                                const struct c1pkg_prefix *prefix)
{
    int rank = c1pkg_prefix_match_rank(prefix, item->name, item->id);
    /* A device keyboard can jump to a Chinese display name by its first
     * pinyin initial. Longer ASCII prefixes use the package ID, which is the
     * stable searchable spelling for Chinese applications. */
    if (strlen(prefix->text) == 1U && gui_matches_initial(item->name, prefix->text[0])) {
        if (rank < 1) rank = 1;
    }
    return rank;
}

static int gui_jump_find(struct gui_state *s, const struct c1pkg_prefix *prefix,
                         size_t start, size_t *position)
{
    int best_rank = 0;
    size_t best_position = 0U;
    for (size_t step = 0U; step < s->count; ++step) {
        size_t candidate = (start + step) % s->count;
        const struct c1pkg_download_item *item = &s->downloads.items[s->visible[candidate]];
        int rank = gui_item_prefix_rank(item, prefix);
        if (rank > best_rank) {
            best_rank = rank;
            best_position = candidate;
        }
    }
    if (best_rank == 0) return 0;
    *position = best_position;
    return best_rank;
}

static int gui_jump_to_prefix(struct gui_state *s, int key, uint64_t now_ms)
{
    struct c1pkg_prefix previous = s->jump_prefix;
    size_t position;
    int repeated = previous.text[0] != '\0' && previous.text[1] == '\0' &&
                   previous.text[0] == (char)(key >= 'A' && key <= 'Z' ? key + ('a' - 'A') : key) &&
                   now_ms >= previous.updated_ms &&
                   now_ms - previous.updated_ms < C1PKG_PREFIX_TIMEOUT_MS;
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    if (key < '0' || key > 'z' || (key > '9' && key < 'A') || (key > 'Z' && key < 'a')) return 0;

    (void)c1pkg_prefix_input(&s->jump_prefix, key, now_ms);
    if (s->count && gui_jump_find(s, &s->jump_prefix, 0U, &position) != 0) {
        /* The first character starts a new prefix at the top. A repeated
         * single-character key retains the old cycling behavior. */
        if (s->jump_prefix.text[1] == '\0' && repeated)
            (void)gui_jump_find(s, &s->jump_prefix, (s->selected[s->tab] + 1U) % s->count, &position);
        s->selected[s->tab] = position;
        return 1;
    }

    /* A failed extension starts a fresh prefix with the current key. This
     * makes `pin` select Pinao instead of falling through to an unrelated
     * first-letter match, while still recovering naturally from typos. */
    c1pkg_prefix_clear(&s->jump_prefix);
    (void)c1pkg_prefix_input(&s->jump_prefix, key, now_ms);
    if (s->count && gui_jump_find(s, &s->jump_prefix, repeated ?
                                  (s->selected[s->tab] + 1U) % s->count : 0U, &position) != 0) {
        s->selected[s->tab] = position;
        return 1;
    }
    return 0;
}

static void select_visible(struct gui_state *s)
{
    size_t i, rows = gui_rows(s);
    s->count = 0U;
    for (i = 0U; i < s->downloads.count; ++i) {
        const struct c1pkg_download_item *item = &s->downloads.items[i];
        if (!c1pkg_is_internal_id(item->id) &&
            ((s->tab == 1U && item->package_index != C1PKG_PACKAGE_NONE) ||
             (s->tab == 0U && item->installed_version[0] != '\0')) &&
            (gui_contains(item->name, s->query) || gui_contains(item->id, s->query)))
            s->visible[s->count++] = i;
    }
    if (s->count == 0U) { s->selected[s->tab] = s->offset[s->tab] = 0U; return; }
    if (s->selected[s->tab] >= s->count) s->selected[s->tab] = s->count - 1U;
    if (s->offset[s->tab] > s->selected[s->tab]) s->offset[s->tab] = s->selected[s->tab];
    if (s->selected[s->tab] >= s->offset[s->tab] + rows)
        s->offset[s->tab] = s->selected[s->tab] - rows + 1U;
    /* Keep a full last page after uninstall or a shrinking refresh. */
    if (s->count <= rows) s->offset[s->tab] = 0U;
    else if (s->offset[s->tab] > s->count - rows) s->offset[s->tab] = s->count - rows;
}

static void gui_query_changed(struct gui_state *s)
{
    s->selected[0] = s->selected[1] = s->offset[0] = s->offset[1] = 0U;
    gui_jump_clear(s);
    select_visible(s);
}

static int gui_append_commit(struct gui_state *s, const char *commit, uint64_t now_ms)
{
    size_t length = strlen(commit), used = strlen(s->query), offset = 0U;
    /* Atomic append: never accept half a commit, invalid scalars or controls.
     * The font metadata validator is reused per glyph; ASCII spaces are legal
     * within a search. Package labels are limited to 40 UTF-8 bytes. */
    if (!length) return 1;
    if (length > C1PKG_NAME_MAX - used) goto invalid;
    while (offset < length) {
        unsigned char first = (unsigned char)commit[offset];
        size_t bytes = first < 0x80U ? 1U : first >= 0xc2U && first <= 0xdfU ? 2U :
                       first >= 0xe0U && first <= 0xefU ? 3U : first >= 0xf0U && first <= 0xf4U ? 4U : 0U;
        char glyph[5] = "";
        if (!bytes || bytes > length - offset) goto invalid;
        memcpy(glyph, commit + offset, bytes);
        if (strcmp(glyph, " ") && !c1pkg_valid_label(glyph)) goto invalid;
        offset += bytes;
    }
    for (offset = 0U; offset < length; ++offset) {
        unsigned char ch = (unsigned char)commit[offset];
        s->query[used + offset] = (char)(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    }
    s->query[used + length] = '\0';
    (void)now_ms;
    gui_query_changed(s);
    return 1;
invalid:
    s->ime_notice = 1;
    snprintf(s->status, sizeof(s->status), "%s", GUI_LABEL(s, "搜索文字无效或过长", "Search invalid or too long"));
    strcpy(s->ime_error, s->status);
    return 0;
}

static int search_gui(struct gui_state *s, int key, uint64_t now_ms)
{
    size_t length;
    if (!s->search_editing) return 0;
    if (key == G_REFRESH) key = ' ';
    if (key != C1PKG_KEY_ERASE && key != C1PKG_KEY_CLEAR && (key < 32 || key > 126)) return 0;
    length = strlen(s->query);
    if (key == C1PKG_KEY_ERASE) {
        if (length) {
            do { --length; } while (length && ((unsigned char)s->query[length] & 0xc0U) == 0x80U);
            s->query[length] = '\0';
        }
    } else if (key == C1PKG_KEY_CLEAR) s->query[0] = '\0';
    else {
        char commit[2] = {(char)key, '\0'};
        (void)gui_append_commit(s, commit, now_ms);
        return 1;
    }
    (void)now_ms;
    gui_query_changed(s);
    return 1;
}

static void reload_local(struct gui_state *s)
{
    struct c1pkg_installed_list local;
    s->storage_error[0] = '\0';
    s->storage_ready = c1pkg_store_list(&local, s->storage_error, sizeof(s->storage_error)) == 0;
    /* Preserve the last known library if storage becomes unavailable. The
     * server list remains independent; mutations stay disabled. */
    if (s->storage_ready) s->installed = local;
    {
        char selected_ids[2][C1PKG_ID_MAX + 1U] = {{0}};
        unsigned int tab = s->tab;
        for (s->tab = 0U; s->tab < 2U; ++s->tab) {
            select_visible(s);
            if (s->count) strcpy(selected_ids[s->tab], s->downloads.items[s->visible[s->selected[s->tab]]].id);
        }
        (void)c1pkg_download_list_build(&s->index, &s->installed, &s->downloads);
        for (s->tab = 0U; s->tab < 2U; ++s->tab) {
            select_visible(s);
            for (size_t i = 0U; i < s->count; ++i)
                if (!strcmp(selected_ids[s->tab], s->downloads.items[s->visible[i]].id)) s->selected[s->tab] = i;
            select_visible(s);
        }
        s->tab = tab;
        select_visible(s);
    }
}

static void render_toolbar(struct gui_state *s)
{
    /* Selected tabs retain their four-corner focus frame, like the battery
     * view labels and cursor; application rows instead use solid black fill. */
    for (unsigned int tab = 0U; tab < 2U; ++tab) {
        const char *label = tab ? GUI_LABEL(s, "商店", "Store") : GUI_LABEL(s, "本地", "Local");
        unsigned int x = 6U + tab * 64U;
        unsigned int width = (unsigned int)c1pkg_text_width(label);
        if (s->tab == tab)
            c1_ui_focus_frame(s->frame, GUI_WIDTH, GUI_HEIGHT, x + 4U, 18U, width + 20U, 17U, true);
        text(s, x + 14U, 18U, label, width, true);
    }
    if (s->search_editing) {
        text(s, 164U, 18U, GUI_LABEL(s, "搜索中", "Search"), 72U, true);
    } else {
        /* Shift is a keycap hint, not the selection indicator. */
        static const unsigned short arrow[14] = {
            0x0040, 0x00a0, 0x0110, 0x0208, 0x0404, 0x0802, 0x1f1f,
            0x0110, 0x0110, 0x0110, 0x0110, 0x0110, 0x0110, 0x01f0
        };
        /* Symmetric 13px arrowhead and shaft share x=153 as their center.
         * Its 14px height is centered exactly on the 16px label row
         * (y=18..33), with four blank pixels before the label at x=164. */
        for (unsigned int y = 0U; y < 14U; ++y)
            for (unsigned int x = 0U; x < 13U; ++x)
                if (arrow[y] & (1U << (12U - x))) box(s, 147U + x, 19U + y, 1U, 1U, true);
        text(s, 164U, 18U, GUI_LABEL(s, "管理", "Manage"), 72U, true);
    }
    if (s->tab == 1U) text(s, 244U, 18U, GUI_LABEL(s, "次数", "Inst"), 42U, true);
}

static const char *gui_footer_status(const struct gui_state *s)
{
    /* Errors and actual operation progress take precedence over catalog status.
     * Search and candidates have their own regions and cannot hide failures. */
    if (s->ime_notice) return s->ime_error[0] ? s->ime_error : s->status;
    if (!s->storage_ready) return GUI_LABEL(s, "存储不可用 请检查挂载", "Storage unavailable; check mount");
    if (s->busy) return s->status;
    if (s->refresh.pid > 0) return s->refresh_status;
    if (s->status[0]) return s->status;
    if (s->verified) return GUI_LABEL(s, "商店已验证 空格刷新", "Store verified; Space:refresh");
    if (s->index.sequence) return GUI_LABEL(s, "离线列表 空格刷新", "Offline catalog; Space:refresh");
    return GUI_LABEL(s, "商店状态未知 空格刷新", "Store unknown; Space:refresh");
}

static void render_gui(struct gui_state *s)
{
    char label[128], count_label[32];
    size_t row;
    memset(s->frame, 0, GUI_BYTES);
    gui_telemetry_tick(s, c1pkg_input_now_ms());
    gui_list_title(s, label, sizeof(label));
    render_header(s, label);
    render_toolbar(s);
    if (gui_search_visible(s)) {
        snprintf(label, sizeof(label), "%s %s%s", GUI_LABEL(s, "搜索:", "Find:"), s->query,
                 s->search_editing ? "_" : "");
        text(s, 6U, GUI_SEARCH_Y, label, 284U, true);
    }
    for (row = 0U; row < gui_rows(s); ++row) {
        size_t position = s->offset[s->tab] + row;
        unsigned int y = gui_list_y(s) + (unsigned int)row * GUI_ROW_HEIGHT;
        bool active = position == s->selected[s->tab];
        if (position >= s->count) continue;
        const struct c1pkg_download_item *item = &s->downloads.items[s->visible[position]];
        if (active) box(s, 6U, y, 284U, GUI_ROW_HEIGHT - 1U, true);
        /* Local rows give the unused statistics column to the application name. */
        text(s, 10U, y + 1U, item->name, s->tab == 1U ? 150U : 200U, !active);
        text(s, s->tab == 1U ? 164U : 214U, y + 1U, state_label(s, item->status), 72U, !active);
        if (s->tab == 1U) {
            gui_metrics_row_label(s, item->id, count_label, sizeof(count_label));
            text(s, 244U, y + 1U, count_label, 42U, !active);
        }
    }
    if (!s->count) {
        text(s, 10U, gui_list_y(s) + 1U, s->query[0] ? GUI_LABEL(s, "没有匹配的应用", "No matching apps") :
             s->tab == 0U ? GUI_LABEL(s, "尚未安装应用", "No local apps") :
             GUI_LABEL(s, "商店列表暂不可用", "Store not available"), 280U, true);
        text(s, 10U, gui_list_y(s) + 21U, s->query[0] ? GUI_LABEL(s, "返回清除筛选", "Back:clear filter") :
             s->tab == 0U ? GUI_LABEL(s, "右键进入商店", "Right: browse store") :
             GUI_LABEL(s, "空格刷新 左键返回本地", "Space:refresh Left:local"), 280U, true);
    }
    if (gui_ime_visible(s)) {
        box(s, 6U, 91U, 284U, 1U, true);
        text(s, 6U, 93U, s->ime_view.preedit[0] ? s->ime_view.preedit :
             GUI_LABEL(s, "中文 Shift+Space切换", "Chinese; Shift+Space toggles"), 284U, true);
        for (row = 0U; row < s->ime_view.candidate_count && row < C1_IME_MAX_CANDIDATES; ++row) {
            unsigned int x = 6U + (unsigned int)row * 57U;
            char number[2] = {(char)('1' + row), '\0'};
            bool highlighted = row == s->ime_view.highlighted_candidate;
            if (highlighted) box(s, x - 1U, 112U, 56U, 18U, true);
            text(s, x, 113U, number, 8U, !highlighted);
            text(s, x + 9U, 113U, s->ime_view.candidates[row], 46U, !highlighted);
        }
    }
    render_footer(s, gui_footer_status(s));
    present(s);
}

/* The repository callback currently reports Chinese retry/wait stages. Adapt
 * those actual events here without changing transport code or inventing a
 * percentage. NULL progress messages only poll cancellation. */
static void gui_progress_label(struct gui_state *s, const char *message)
{
    unsigned long long seconds;
    unsigned int attempt;
    int end = 0, start = 0;
    if (sscanf(message, "等待 %llu 秒后自动继续%n", &seconds, &end) == 1 && end > 0 && message[end] == '\0') {
        snprintf(s->status, sizeof(s->status), GUI_LABEL(s, "等待 %llu 秒后自动继续", "Retry in %llu s; auto-resume"), seconds);
    } else if (sscanf(message, "第 %u/4 次尝试：%n", &attempt, &start) == 1 && start > 0) {
        const char *stage = message + start;
        const char *label = strcmp(stage, "正在检查软件仓库") == 0 ? GUI_LABEL(s, "检查仓库", "Checking catalog") :
                            strcmp(stage, "正在继续下载软件包") == 0 ? GUI_LABEL(s, "继续下载", "Resuming download") :
                            strcmp(stage, "正在下载软件包") == 0 ? GUI_LABEL(s, "下载应用", "Downloading") :
                            GUI_LABEL(s, "处理中", "Working");
        snprintf(s->status, sizeof(s->status), "%s %u/4", label, attempt);
    } else {
        snprintf(s->status, sizeof(s->status), "%s", s->language == GUI_EN ? "Working; Back cancels" : message);
    }
}

static int gui_progress(const char *message, void *context)
{
    struct gui_state *s = context;
    if (message != NULL) {
        gui_progress_label(s, message);
        render_gui(s);
    }
    if (interrupted || s->display_failed || gui_cancel_key(gui_key(0))) s->cancelled = 1;
    return s->cancelled;
}

static void begin_operation(struct gui_state *s)
{
    gui_refresh_dispose(&s->refresh);
    gui_ime_close(s);
    s->ime_notice = 0;
    s->busy = 1; s->cancelled = 0;
    c1pkg_set_progress(gui_progress, s);
}
static void end_operation(struct gui_state *s)
{
    c1pkg_set_progress(NULL, NULL);
    s->busy = 0;
}

static void refresh_gui(struct gui_state *s, const struct c1pkg_config *config)
{
    int result;
    if (s->busy || s->refresh.pid > 0) return;
    s->config = config;
    result = gui_refresh_start(&s->refresh, config);
    snprintf(s->refresh_status, sizeof(s->refresh_status), "%s", result > 0 ?
             GUI_LABEL(s, "后台刷新 可继续选择", "Refreshing; navigation available") :
             GUI_LABEL(s, "刷新启动失败 空格重试", "Refresh unavailable; Space retries"));
    if (result < 0) snprintf(s->status, sizeof(s->status), "%s", s->refresh_status);
}

static void gui_refresh_tick(struct gui_state *s)
{
    char message[C1PKG_ERROR_MAX] = "";
    int result = gui_refresh_poll(&s->refresh, message, sizeof(message));
    if (message[0]) {
        char saved[C1PKG_ERROR_MAX];
        memcpy(saved, s->status, sizeof(saved));
        gui_progress_label(s, message);
        memcpy(s->refresh_status, s->status, sizeof(s->status));
        memcpy(s->status, saved, sizeof(saved));
    }
    if (!result) return;
    struct gui_refresh_result *reply = s->refresh.result;
    if (result == 2) {
        if (reply->cached && reply->index.sequence >= s->index.sequence && !s->verified) {
            s->index = reply->index;
            c1pkg_desktop_seen(s->config, &s->index);
            reload_local(s);
        }
        return;
    }
    if (result > 0 && reply->result == 0 && reply->index.sequence >= s->index.sequence && reply->index.sequence) {
        s->index = reply->index;
        c1pkg_desktop_seen(s->config, &s->index);
        s->verified = 1;
        snprintf(s->status, sizeof(s->status), "%s", GUI_LABEL(s, "商店已验证 空格刷新", "Store verified; Space:refresh"));
    } else {
        /* Keep the last signed snapshot for browsing/local launches. A failed
         * refresh never blesses a stale snapshot for a new installation. */
        s->verified = 0;
        snprintf(s->status, sizeof(s->status), "%s", s->refresh.cancelling ?
                 GUI_LABEL(s, "已取消 保留现有列表", "Cancelled; catalog kept") : s->index.sequence ?
                 GUI_LABEL(s, "离线列表 安装需重新验证", "Offline; revalidate to install") :
                 GUI_LABEL(s, "刷新失败 本地应用可用", "Refresh failed; local apps available"));
    }
    free(s->refresh.result); s->refresh.result = NULL;
    reload_local(s);
}

enum gui_action { A_INSTALL, A_LAUNCH, A_UPDATE, A_REMOVE, A_CANCEL };
static const char *action_label(const struct gui_state *s, enum gui_action action)
{
    switch (action) {
    case A_INSTALL: return GUI_LABEL(s, "安装", "Install");
    case A_LAUNCH: return GUI_LABEL(s, "打开", "Open");
    case A_UPDATE: return GUI_LABEL(s, "更新", "Update");
    case A_REMOVE: return GUI_LABEL(s, "卸载", "Remove");
    case A_CANCEL: return GUI_LABEL(s, "取消", "Cancel");
    }
    return GUI_LABEL(s, "取消", "Cancel");
}
static size_t actions_for(const struct c1pkg_download_item *item, enum gui_action actions[4])
{
    size_t count = 0U;
    if (item->installed_version[0] == '\0') actions[count++] = A_INSTALL;
    else {
        actions[count++] = A_LAUNCH;
        if (item->status == C1PKG_DOWNLOAD_UPDATE_AVAILABLE) actions[count++] = A_UPDATE;
        actions[count++] = A_REMOVE;
    }
    actions[count++] = A_CANCEL;
    return count;
}

static int gui_update_available(const struct c1pkg_download_item *item)
{
    /* Match the status already shown in the list, including a re-verified
     * offline cache or the signed snapshot retained during refresh. This is
     * only a prompt, not installation authority: manage_gui still requires
     * a completed online verification and an unchanged package selection. */
    return item->installed_version[0] != '\0' && item->status == C1PKG_DOWNLOAD_UPDATE_AVAILABLE;
}

static void gui_metrics_tick(struct gui_state *s, const struct c1pkg_config *config,
                              uint64_t now)
{
    if (s->metrics_job) (void)c1pkg_device_metrics_poll(&s->metrics_job, &s->metrics);
    if (!s->metrics_job && config && now >= s->metrics_at) {
        /* The worker keeps GET caching and report/server gates separately.
         * Recheck pending uploads without imposing another 15-minute GUI delay. */
        s->metrics_at = now + (uint64_t)C1PKG_DEVICE_METRICS_RETRY_INTERVAL * 1000U;
        s->metrics_job = c1pkg_device_metrics_start(config);
    }
}

static void gui_metrics_label(const struct gui_state *s, const char *id,
                               char *out, size_t capacity)
{
    uint64_t count;
    if (c1pkg_metrics_lookup(&s->metrics, id, &count))
        snprintf(out, capacity, "%s %llu%s", GUI_LABEL(s, "安装次数", "Installs:"),
                 (unsigned long long)count, GUI_LABEL(s, "次", ""));
    else snprintf(out, capacity, "%s", GUI_LABEL(s, "安装次数：暂无统计", "Installs: unavailable"));
}

static void render_detail(struct gui_state *s, const struct c1pkg_download_item *item,
                          const enum gui_action *actions, size_t count, size_t selected, int confirm)
{
    char line[160];
    size_t i;
    memset(s->frame, 0, GUI_BYTES);
    gui_telemetry_tick(s, c1pkg_input_now_ms());
    render_header(s, item->name);
    text(s, 6U, 18U, confirm ? GUI_LABEL(s, "确认卸载", "Remove app?") : item->name, 284U, true);
    if (confirm) text(s, 6U, 37U, item->name, 284U, true);
    else {
        snprintf(line, sizeof(line), "%s %s", GUI_LABEL(s, "作者", "By:"), item->author);
        text(s, 6U, 37U, line, 284U, true);
    }
    snprintf(line, sizeof(line), "%s %s", GUI_LABEL(s, "本地", "Local:"),
             item->installed_version[0] ? item->installed_version : GUI_LABEL(s, "未安装", "not installed"));
    text(s, 6U, 55U, line, 284U, true);
    snprintf(line, sizeof(line), "%s %s", GUI_LABEL(s, "仓库", "Repo:"),
             item->available_version[0] ? item->available_version : GUI_LABEL(s, "暂无版本", "unavailable"));
    text(s, 6U, 73U, line, 284U, true);
    if (confirm) {
        text(s, 6U, 91U, GUI_LABEL(s, "删除该应用全部已装版本", "Removes all installed versions"), 284U, true);
    } else if (s->tab == 1U) {
        gui_metrics_label(s, item->id, line, sizeof(line));
        text(s, 6U, 91U, line, 284U, true);
    } else if (gui_update_available(item)) {
        text(s, 6U, 91U, GUI_LABEL(s, "有可用更新，是否更新？", "Update available; update?"), 284U, true);
    }
    box(s, 6U, 108U, 284U, 1U, true);
    for (i = 0U; i < count; ++i) {
        unsigned int width = 284U / (unsigned int)count;
        unsigned int x = 6U + (unsigned int)i * width;
        if (i == selected) box(s, x, 109U, width - 2U, 20U, true);
        text(s, x + 4U, 111U, action_label(s, actions[i]), width - 8U, i != selected);
    }
    render_footer(s, GUI_LABEL(s, "左右选择 确认执行 返回取消", "←→ Enter:confirm Back:cancel"));
    present(s);
}

static int choose_action(struct gui_state *s, const struct c1pkg_download_item *item,
                          enum gui_action *actions, size_t count, size_t selection, int confirm)
{
    gui_ime_close(s);
    while (!interrupted && !s->display_failed) {
        int key;
        gui_refresh_tick(s);
        gui_metrics_tick(s, NULL, c1pkg_input_now_ms());
        render_detail(s, item, actions, count, selection, confirm);
        key = gui_key(100);
        if (gui_cancel_key(key)) return A_CANCEL;
        if ((key == G_LEFT || key == G_UP) && selection > 0U) --selection;
        if ((key == G_RIGHT || key == G_DOWN) && selection + 1U < count) ++selection;
        if (key == G_ENTER) return actions[selection];
    }
    return A_CANCEL;
}

static void gui_release(struct gui_state *s)
{
    c1pkg_device_metrics_cancel(s->metrics_job);
    gui_refresh_dispose(&s->refresh);
    while (s->metrics_job) {
        (void)c1pkg_device_metrics_poll(&s->metrics_job, &s->metrics);
        if (s->metrics_job) (void)poll(NULL, 0U, 10);
    }
    gui_ime_close(s);
    c1pkg_set_progress(NULL, NULL);
    if (s->display >= 0) { close(s->display); s->display = -1; }
    c1_app_lease_release(s->lease); s->lease = -1;
    if (s->run_lock >= 0) { c1_app_lease_clear_mode(); c1_app_lease_release(s->run_lock); s->run_lock = -1; }
    if (gui_terminal_saved) { (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &gui_saved_terminal); gui_terminal_saved = 0; }
}

static void manage_gui(struct gui_state *s, const struct c1pkg_config *config, int details)
{
    struct c1pkg_download_item item;
    enum gui_action actions[4];
    size_t count;
    int action, outcome;
    char error[C1PKG_ERROR_MAX] = "";
    if (s->ime_notice) return; /* Invalid commits never authorize an old selection. */
    gui_ime_close(s);
    if (s->count == 0U) return;
    item = s->downloads.items[s->visible[s->selected[s->tab]]];
    count = actions_for(&item, actions);
    /* Local confirm follows the displayed update status, even offline or
     * during refresh. Management defaults to Cancel; the prompt alone never
     * authorizes installation or bypasses the verification gates below. */
    if (!details && s->tab == 0U && gui_update_available(&item)) details = 1;
    for (;;) {
        action = details ? choose_action(s, &item, actions, count, count - 1U, 0) :
                 item.installed_version[0] ? A_LAUNCH : A_INSTALL;
        if (action != A_REMOVE) break;
        enum gui_action confirmation[] = {A_REMOVE, A_CANCEL};
        if (choose_action(s, &item, confirmation, 2U, 1U, 1) == A_REMOVE) break;
        if (interrupted || s->display_failed) return;
        /* Cancel a nested confirmation back to details, not the list. */
    }
    if (action == A_CANCEL) return;
    if (!s->storage_ready) {
        snprintf(s->status, sizeof(s->status), "%s", GUI_LABEL(s, "存储不可用 请检查挂载", "Storage unavailable"));
        return;
    }
    if (action == A_LAUNCH) {
        gui_release(s);
        execl("/usr/data/c1/bin/c1pkg", "c1pkg", "launch", item.id, (char *)NULL);
        fprintf(stderr, "c1pkg: launch failed: %s\n", strerror(errno));
        s->display_failed = 1;
        interrupted = 1;
        return;
    }
    if (action == A_REMOVE) {
        gui_refresh_dispose(&s->refresh);
        snprintf(s->status, sizeof(s->status), "%s", GUI_LABEL(s, "正在卸载应用", "Removing app")); render_gui(s);
        outcome = c1pkg_store_remove_with_run_lock(item.id, s->run_lock, error, sizeof(error));
        snprintf(s->status, sizeof(s->status), "%s", outcome == 0 ? GUI_LABEL(s, "卸载完成", "App removed") :
                 GUI_LABEL(s, "卸载失败 请检查存储", "Remove failed; check storage"));
    } else {
        /* No implicit deferred installation. Revalidation runs in the background;
         * the user confirms the newly displayed signed selection a second time. */
        if (!s->verified || s->refresh.pid > 0) {
            refresh_gui(s, config);
            snprintf(s->refresh_status, sizeof(s->refresh_status), "%s",
                     GUI_LABEL(s, "验证完成后请再次确认", "Verify first, then confirm again"));
            return;
        }
        const struct c1pkg_package *package = c1pkg_repo_find(&s->index, item.id);
        if (package == NULL) {
            snprintf(s->status, sizeof(s->status), "%s", GUI_LABEL(s, "应用已下架 列表已保留", "App no longer in catalog"));
            return;
        }
        if (strcmp(package->version, item.available_version) != 0 || strcmp(package->name, item.name) != 0) {
            snprintf(s->status, sizeof(s->status), "%s",
                     GUI_LABEL(s, "应用信息已更新，请重新确认", "App changed; review and confirm"));
            return;
        }
        if (c1pkg_repo_bind_transport(config) != 0) {
            snprintf(s->status, sizeof(s->status), "%s", GUI_LABEL(s, "仓库地址无效，无法安装", "Invalid repository URL"));
            return;
        }
        begin_operation(s);
        snprintf(s->status, sizeof(s->status), "%s", action == A_UPDATE ?
                 GUI_LABEL(s, "更新并校验应用", "Updating and verifying") : GUI_LABEL(s, "安装并校验应用", "Installing and verifying"));
        render_gui(s);
        outcome = c1pkg_store_install(config, package, error, sizeof(error));
        end_operation(s);
        if (outcome == C1PKG_INSTALL_OK) s->metrics_at = 0U;
        snprintf(s->status, sizeof(s->status), "%s", outcome == C1PKG_INSTALL_OK ?
                 GUI_LABEL(s, "安装完成 可在已安装中打开", "Installed; open in Installed") :
                 outcome == C1PKG_INSTALL_SKIPPED ? GUI_LABEL(s, "无需安装 保留本地版本", "Skipped; local version kept") :
                 outcome == C1PKG_INSTALL_CANCELLED || s->cancelled ? GUI_LABEL(s, "已取消 本地应用未替换", "Cancelled; local app kept") :
                 outcome == C1PKG_INSTALL_STORAGE_ERROR ? GUI_LABEL(s, "存储错误 请检查空间", "Storage error; check space") :
                 GUI_LABEL(s, "安装失败 可确认后重试", "Install failed; retry"));
    }
    if (error[0]) fprintf(stderr, "c1pkg: %s\n", error);
    reload_local(s);
}

/* Browse mode accumulates a short ASCII prefix; explicit search remains
 * the persistent UTF-8 substring filter through '/' or the search key. */
static int gui_list_key(struct gui_state *s, const struct c1pkg_config *config,
                        int key, uint64_t now_ms)
{
    if (interrupted || key == G_HOME) { gui_ime_close(s); return 1; }
    if (key == G_SEARCH || (key == '/' && !s->search_editing)) {
        if (s->search_editing) { gui_ime_close(s); s->search_editing = 0; }
        else s->search_editing = 1;
        gui_jump_clear(s);
        select_visible(s); return 0;
    }
    if (search_gui(s, key, now_ms)) return 0;
    if (key == G_BACK || (key == C1PKG_KEY_ERASE && !s->search_editing)) {
        if (s->ime_notice) { s->ime_notice = 0; return 0; }
        if (s->search_editing || gui_ime_visible(s)) {
            gui_ime_close(s); s->search_editing = 0; select_visible(s); return 0;
        }
        if (s->query[0]) { s->query[0] = '\0'; gui_query_changed(s); return 0; }
        gui_ime_close(s); return 1;
    }
    if (key == G_NONE) return 0;
    /* Outside composition, preserve volume's existing tab navigation. */
    if (key == G_PAGE_UP) key = G_LEFT;
    if (key == G_PAGE_DOWN) key = G_RIGHT;
    if (key == G_ENTER || key == G_MANAGE) {
        gui_jump_clear(s);
        if (s->search_editing) {
            /* Bare Shift must not discard a composition or turn editing into
             * an application action. Enter only applies the filter. */
            if (key == G_ENTER) { gui_ime_close(s); s->search_editing = 0; select_visible(s); }
        } else manage_gui(s, config, key == G_MANAGE || s->tab == 1U);
        return 0;
    }
    if (!s->search_editing && ((key >= 'A' && key <= 'Z') ||
                               (key >= 'a' && key <= 'z') ||
                               (key >= '0' && key <= '9'))) {
        (void)gui_jump_to_prefix(s, key, now_ms);
        select_visible(s);
        return 0;
    }
    if (key == G_LEFT || key == G_RIGHT) {
        gui_jump_clear(s);
        s->tab = key == G_LEFT ? 0U : 1U;
    } else if (key == G_UP && s->selected[s->tab] > 0U) {
        gui_jump_clear(s);
        --s->selected[s->tab];
    } else if (key == G_DOWN && s->selected[s->tab] + 1U < s->count) {
        gui_jump_clear(s);
        ++s->selected[s->tab];
    } else if (key == G_REFRESH) {
        gui_jump_clear(s);
        refresh_gui(s, config);
    }
    select_visible(s); return 0;
}

/* All list keys enter the same asynchronous queue. Only Home/focus loss may
 * bypass it; they close the connection and intentionally discard pending keys.
 * Original navigation commands execute only after an unconsumed reply. */
static uint32_t gui_ime_keysym(int key, uint32_t *modifiers)
{
    *modifiers = 0U;
    switch (key) {
    case G_UP: return 0xff52U;
    case G_DOWN: return 0xff54U;
    case G_LEFT: return C1_IME_KEY_LEFT;
    case G_RIGHT: return C1_IME_KEY_RIGHT;
    case G_PAGE_UP: return C1_IME_KEY_PAGE_UP;
    case G_PAGE_DOWN: return C1_IME_KEY_PAGE_DOWN;
    case G_ENTER: return C1_IME_KEY_RETURN;
    case G_BACK: return C1_IME_KEY_ESCAPE;
    case G_REFRESH: return C1_IME_KEY_SPACE;
    case C1PKG_KEY_ERASE: return C1_IME_KEY_BACKSPACE;
    case C1PKG_KEY_CLEAR: *modifiers = C1_IME_MOD_CONTROL; return 'u';
    default: return key >= 33 && key <= 126 ? (uint32_t)key : 0U;
    }
}

static int gui_ime_original_key(const struct c1_ime_request *request)
{
    if (request->modifiers == C1_IME_MOD_CONTROL && request->keysym == 'u') return C1PKG_KEY_CLEAR;
    switch (request->keysym) {
    case 0xff52U: return G_UP;
    case 0xff54U: return G_DOWN;
    case C1_IME_KEY_LEFT: return G_LEFT;
    case C1_IME_KEY_RIGHT: return G_RIGHT;
    case C1_IME_KEY_PAGE_UP: return G_PAGE_UP;
    case C1_IME_KEY_PAGE_DOWN: return G_PAGE_DOWN;
    case C1_IME_KEY_RETURN: return G_ENTER;
    case C1_IME_KEY_ESCAPE: return G_BACK;
    case C1_IME_KEY_SPACE: return G_REFRESH;
    case C1_IME_KEY_BACKSPACE: return C1PKG_KEY_ERASE;
    default: return request->keysym >= 33U && request->keysym <= 126U ? (int)request->keysym : G_NONE;
    }
}

static void gui_ime_toggle(struct gui_state *s, const char *path, uint64_t now_ms)
{
    int fresh = s->input_method.client.fd < 0;
    s->search_editing = 1;
    s->ime_notice = 0;
    if (!c1_input_method_toggle(&s->input_method, path, (int64_t)now_ms)) { gui_ime_error(s); return; }
    if (fresh) {
        /* STATUS must precede the adapter's queued MODE and every key. The
         * adapter owns this ring; insertion is only into a fresh one-item queue. */
        s->input_method.head = (s->input_method.head + C1_INPUT_QUEUE_SIZE - 1U) % C1_INPUT_QUEUE_SIZE;
        s->input_method.queue[s->input_method.head] = (struct c1_ime_request){C1_IME_OP_STATUS, 0U, 0U, 0U};
        ++s->input_method.count;
        s->ime_ready = 0;
    }
    select_visible(s);
}

static int gui_ime_input(struct gui_state *s, const struct c1pkg_config *config, int key, uint64_t now_ms)
{
    uint32_t modifiers, keysym;
    if (interrupted || key == G_HOME) { gui_ime_close(s); return 1; }
    if (key == G_NONE) return 0;
    if (key == G_IME_TOGGLE) { gui_ime_toggle(s, NULL, now_ms); return 0; }
    if (key == G_SEARCH || !s->search_editing) return gui_list_key(s, config, key, now_ms);
    if ((key >= 32 && key <= 126) || key == C1PKG_KEY_ERASE || key == C1PKG_KEY_CLEAR) s->ime_notice = 0;
    keysym = gui_ime_keysym(key, &modifiers);
    if (keysym && c1_input_method_key(&s->input_method, keysym, modifiers)) {
        if (s->input_method.failed) gui_ime_error(s);
        return 0;
    }
    return gui_list_key(s, config, key, now_ms);
}

static int gui_ime_pump(struct gui_state *s, const struct c1pkg_config *config, uint64_t now_ms)
{
    struct c1_ime_response response;
    int result;
    if (s->input_method.failed) { gui_ime_error(s); select_visible(s); return 0; }
    result = c1_input_method_tick(&s->input_method, &response, (int64_t)now_ms);
    if (result < 0) { gui_ime_error(s); select_visible(s); return 0; }
    if (!result) { select_visible(s); return 0; }
    if (response.status != C1_IME_STATUS_OK || !(response.flags & C1_IME_READY) ||
        ((response.flags & C1_IME_TRUNCATED) && response.commit[0]) ||
        (!s->ime_ready && response.request.operation != C1_IME_OP_STATUS)) {
        gui_ime_error(s); select_visible(s); return 0;
    }
    s->ime_ready = 1;
    s->ime_view = response;
    if (!gui_append_commit(s, response.commit, now_ms)) {
        /* A rejected/truncated search must not launch the previously selected
         * app when Enter is queued behind it. Discard the queue and show why. */
        gui_ime_close(s); select_visible(s); return 0;
    }
    select_visible(s);
    if (response.request.operation == C1_IME_OP_KEY && !(response.flags & C1_IME_CONSUMED))
        return gui_list_key(s, config, gui_ime_original_key(&response.request), now_ms);
    return 0;
}

static int gui_until(int timeout, uint64_t now, uint64_t deadline)
{
    uint64_t remaining = deadline > now ? deadline - now : 0U;
    int wait = remaining > INT_MAX ? INT_MAX : (int)remaining;
    return timeout < 0 || wait < timeout ? wait : timeout;
}

static int gui_poll_timeout(const struct gui_state *s, uint64_t now)
{
    /* Clock minute boundaries and a bounded battery read are independent of
     * network jobs. Final result can precede waitpid readiness by one tick. */
    int timeout = s->refresh.pid > 0 || s->metrics_job ? 100 : 1000;
    if (gui_keyboard.used) timeout = gui_until(timeout, now, gui_keyboard.deadline);
    if (s->input_method.deadline >= 0)
        timeout = gui_until(timeout, now, (uint64_t)s->input_method.deadline);
    return timeout;
}

int c1pkg_gui(const struct c1pkg_config *config)
{
    struct gui_state *s = calloc(1U, sizeof(*s));
    struct termios raw;
    struct sigaction action, old_term, old_hup, old_int;
    int result = 1;
    if (s == NULL) return 1;
    initialize_gui(s);
    interrupted = 0;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &gui_saved_terminal) != 0) goto done;
    raw = gui_saved_terminal;
    raw.c_iflag &= (tcflag_t)~(ICRNL | IXON);
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) goto done;
    gui_terminal_saved = 1;
    s->run_lock = c1_app_run_acquire();
    if (s->run_lock < 0) goto done;
    /* Terminal mode permits desktop key forwarding, but the separate display
     * lease ensures only this graphical renderer can write pixels. */
    s->lease = c1_app_lease_acquire();
    if (s->lease < 0 || !c1_app_lease_write_mode("terminal")) goto done;
    (void)fcntl(s->run_lock, F_SETFD, FD_CLOEXEC);
    (void)fcntl(s->lease, F_SETFD, FD_CLOEXEC);
    s->display = open("/dev/epaper_lcd", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (s->display < 0) goto done;
    memset(&action, 0, sizeof(action)); action.sa_handler = gui_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, &old_term);
    (void)sigaction(SIGHUP, &action, &old_hup);
    (void)sigaction(SIGINT, &action, &old_int);
    reload_local(s);
    (void)c1pkg_repo_bind_transport(config); /* Pure scope hint; deadlines remain durable. */
    /* Both cached signature verification and network refresh run in the child. */
    refresh_gui(s, config);
    while (!interrupted && !s->display_failed) {
        struct pollfd descriptors[4];
        int key;
        gui_refresh_tick(s);
        gui_metrics_tick(s, config, c1pkg_input_now_ms());
        if (gui_ime_pump(s, config, c1pkg_input_now_ms())) break;
        render_gui(s);
        descriptors[0] = (struct pollfd){STDIN_FILENO, POLLIN, 0};
        descriptors[1] = (struct pollfd){s->input_method.client.fd,
                                       c1_input_method_poll_events(&s->input_method), 0};
        descriptors[2] = (struct pollfd){s->refresh.result_fd, POLLIN, 0};
        descriptors[3] = (struct pollfd){s->refresh.progress_fd, POLLIN, 0};
        (void)poll(descriptors, 4U, gui_poll_timeout(s, c1pkg_input_now_ms()));
        gui_refresh_tick(s);
        /* Drain one response before reading another physical key. The adapter
         * serializes queued commands; keyboard escape suffixes never block it. */
        if (gui_ime_pump(s, config, c1pkg_input_now_ms())) break;
        key = gui_key(0);
        if (gui_ime_input(s, config, key, c1pkg_input_now_ms())) break;
    }
    result = s->display_failed ? 1 : 0;
    (void)sigaction(SIGTERM, &old_term, NULL);
    (void)sigaction(SIGHUP, &old_hup, NULL);
    (void)sigaction(SIGINT, &old_int, NULL);
done:
    if (result != 0) fprintf(stderr, "c1pkg: graphical display unavailable: %s\n", strerror(errno));
    gui_release(s);
    free(s);
    return result;
}
