/* Native 296x152 package browser. The desktop PTY forwards keys only;
 * pixels go directly to the e-paper device under its exclusive lease. */
#include "gui.h"
#include "text.h"
#include "platform/app_lease.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define GUI_WIDTH 296U
#define GUI_HEIGHT 152U
#define GUI_BYTES (GUI_WIDTH * GUI_HEIGHT / 8U)
#define GUI_ROWS 3U

struct gui_state {
    struct c1pkg_index index;
    struct c1pkg_installed_list installed;
    struct c1pkg_download_list downloads;
    size_t visible[C1PKG_MAX_DOWNLOAD_ITEMS];
    size_t count, selected[2], offset[2];
    unsigned int tab; /* 0 downloads, 1 installed */
    struct c1pkg_prefix prefix;
    int storage_ready, verified, busy, cancelled;
    int run_lock, lease, display;
    char status[C1PKG_ERROR_MAX];
    char storage_error[C1PKG_ERROR_MAX];
    unsigned char frame[GUI_BYTES], previous[GUI_BYTES];
    int previous_valid, display_failed;
};

static void initialize_gui(struct gui_state *s)
{
    memset(s, 0, sizeof(*s));
    s->run_lock = s->lease = s->display = -1;
    s->tab = 1U; /* Start in the local library, including when it is empty. */
}

static volatile sig_atomic_t interrupted;
static struct termios gui_saved_terminal;
static int gui_terminal_saved;
static void gui_signal(int number) { (void)number; interrupted = 1; }

enum gui_key {
    G_NONE = C1PKG_KEY_NONE, G_UP = C1PKG_KEY_UP, G_DOWN = C1PKG_KEY_DOWN,
    G_LEFT = C1PKG_KEY_LEFT, G_RIGHT = C1PKG_KEY_RIGHT,
    G_ENTER = C1PKG_KEY_ENTER, G_BACK = C1PKG_KEY_BACK,
    G_REFRESH = C1PKG_KEY_REFRESH
};

static int gui_key(int timeout)
{
    int key;
    if (interrupted) return G_BACK;
    key = c1pkg_input_read(STDIN_FILENO, timeout);
    if (key == C1PKG_KEY_EOF || key == C1PKG_KEY_QUIT) {
        interrupted = 1;
        return G_BACK;
    }
    return key;
}

/* Letters belong to type-to-select on lists; dialogs/operations still accept
 * Q and the physical erase key as cancel, in addition to Back/Escape. */
static int gui_cancel_key(int key)
{
    return key == G_BACK || key == C1PKG_KEY_ERASE || key == 'q' || key == 'Q';
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

static void text(struct gui_state *s, unsigned int x, unsigned int y,
                 const char *value, unsigned int width, bool black)
{
    c1pkg_text(s->frame, x, y, value, width, black);
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

static const char *state_label(enum c1pkg_download_status status)
{
    switch (status) {
    case C1PKG_DOWNLOAD_NOT_INSTALLED: return "可安装";
    case C1PKG_DOWNLOAD_CURRENT: return "已安装";
    case C1PKG_DOWNLOAD_UPDATE_AVAILABLE: return "可更新";
    case C1PKG_DOWNLOAD_INSTALLED_NEWER: return "本地较新";
    case C1PKG_DOWNLOAD_VERSION_UNKNOWN: return "版本未知";
    case C1PKG_DOWNLOAD_REMOVED: return "本地应用";
    }
    return "未知";
}

static void select_visible(struct gui_state *s)
{
    size_t i;
    s->count = 0U;
    for (i = 0U; i < s->downloads.count; ++i) {
        const struct c1pkg_download_item *item = &s->downloads.items[i];
        if ((s->tab == 0U && item->package_index != C1PKG_PACKAGE_NONE) ||
            (s->tab == 1U && item->installed_version[0] != '\0'))
            s->visible[s->count++] = i;
    }
    if (s->count == 0U) { s->selected[s->tab] = s->offset[s->tab] = 0U; return; }
    if (s->selected[s->tab] >= s->count) s->selected[s->tab] = s->count - 1U;
    if (s->offset[s->tab] > s->selected[s->tab]) s->offset[s->tab] = s->selected[s->tab];
    if (s->selected[s->tab] >= s->offset[s->tab] + GUI_ROWS)
        s->offset[s->tab] = s->selected[s->tab] - GUI_ROWS + 1U;
}

static int search_gui(struct gui_state *s, int key, uint64_t now_ms)
{
    size_t i;
    int best_rank = 0;
    if (!c1pkg_prefix_input(&s->prefix, key, now_ms)) return 0;
    for (i = 0U; i < s->count; ++i) {
        const struct c1pkg_download_item *item = &s->downloads.items[s->visible[i]];
        int rank = c1pkg_prefix_match_rank(&s->prefix, item->name, item->id);
        if (rank > best_rank) {
            best_rank = rank;
            s->selected[s->tab] = i;
        }
    }
    select_visible(s);
    return 1;
}

static int gui_prefix_unmatched(const struct gui_state *s)
{
    const struct c1pkg_download_item *item;
    if (s->prefix.text[0] == '\0') return 0;
    if (s->count == 0U) return 1;
    item = &s->downloads.items[s->visible[s->selected[s->tab]]];
    return !c1pkg_prefix_matches(&s->prefix, item->name, item->id);
}

static void expire_gui_prefix(struct gui_state *s, uint64_t now_ms)
{
    if (c1pkg_prefix_expire(&s->prefix, now_ms))
        snprintf(s->status, sizeof(s->status), "定位已超时清空 · 保留当前选择");
}

static void reload_local(struct gui_state *s)
{
    struct c1pkg_installed_list local;
    s->storage_error[0] = '\0';
    s->storage_ready = c1pkg_store_list(&local, s->storage_error, sizeof(s->storage_error)) == 0;
    /* Preserve the last known library if storage becomes unavailable. The
     * server list remains independent; mutations stay disabled. */
    if (s->storage_ready) s->installed = local;
    (void)c1pkg_download_list_build(&s->index, &s->installed, &s->downloads);
    select_visible(s);
}

static void render_gui(struct gui_state *s)
{
    char label[80];
    size_t row;
    memset(s->frame, 0, GUI_BYTES);
    text(s, 8U, 2U, "应用中心", 160U, true);
    snprintf(label, sizeof(label), "%lu / %lu", (unsigned long)(s->count ? s->selected[s->tab] + 1U : 0U), (unsigned long)s->count);
    text(s, 220U, 2U, label, 72U, true);
    box(s, 6U, 23U, 284U, 1U, true);
    box(s, s->tab == 0U ? 6U : 152U, 25U, 138U, 18U, true);
    text(s, 14U, 26U, "可下载", 128U, s->tab != 0U);
    text(s, 160U, 26U, "已安装", 128U, s->tab != 1U);
    for (row = 0U; row < GUI_ROWS; ++row) {
        size_t position = s->offset[s->tab] + row;
        unsigned int y = 46U + (unsigned int)row * 20U;
        bool active = position == s->selected[s->tab];
        if (position >= s->count) continue;
        const struct c1pkg_download_item *item = &s->downloads.items[s->visible[position]];
        if (active) box(s, 6U, y, 284U, 19U, true);
        text(s, 10U, y + 1U, item->name, 208U, !active);
        text(s, 224U, y + 1U, state_label(item->status), 64U, !active);
    }
    if (s->count == 0U) {
        text(s, 14U, 54U, s->tab == 1U ? "还没有安装应用" : "暂无可下载应用", 270U, true);
        text(s, 14U, 78U, s->tab == 1U ? "按左键浏览可下载列表" : "按空格刷新软件列表", 270U, true);
    }
    box(s, 6U, 107U, 284U, 1U, true);
    if (s->prefix.text[0] != '\0') {
        snprintf(label, sizeof(label), "%s %s",
                 gui_prefix_unmatched(s) ? "无匹配" : "定位", s->prefix.text);
        text(s, 8U, 110U, label, 280U, true);
    } else text(s, 8U, 110U, s->status, 280U, true);
    text(s, 8U, 134U, s->busy ? "返回取消 · 等待后自动继续" :
         gui_prefix_unmatched(s) ? "无匹配 请修改或清空后确认" :
         !s->storage_ready ? "存储不可用 · 暂不可安装" :
         s->tab == 1U ? "确认打开 空格刷新 字母定位" : "确认管理 空格刷新 字母定位", 280U, true);
    present(s);
}

static int gui_progress(const char *message, void *context)
{
    struct gui_state *s = context;
    if (message != NULL) {
        snprintf(s->status, sizeof(s->status), "%s", message);
        render_gui(s);
    }
    if (interrupted || s->display_failed || gui_cancel_key(gui_key(0))) s->cancelled = 1;
    return s->cancelled;
}

static void begin_operation(struct gui_state *s)
{
    c1pkg_prefix_clear(&s->prefix);
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
    char error[C1PKG_ERROR_MAX] = "", cache_error[C1PKG_ERROR_MAX] = "";
    struct c1pkg_index *fresh = calloc(1U, sizeof(*fresh));
    if (fresh == NULL) { snprintf(s->status, sizeof(s->status), "内存不足"); return; }
    begin_operation(s);
    snprintf(s->status, sizeof(s->status), "正在获取并验证软件列表");
    render_gui(s);
    if (c1pkg_repo_refresh(config, fresh, error, sizeof(error)) == 0) {
        s->index = *fresh; s->verified = 1;
        snprintf(s->status, sizeof(s->status), "列表已更新 · 共 %lu 个应用", (unsigned long)s->index.count);
    } else if (s->cancelled || errno == ECANCELED) {
        s->verified = 0;
        snprintf(s->status, sizeof(s->status), "已取消 · 保留现有列表");
    } else if (s->index.sequence != 0U || c1pkg_repo_load_cached(config, fresh, cache_error, sizeof(cache_error)) == 0) {
        if (s->index.sequence == 0U) s->index = *fresh;
        s->verified = 0;
        snprintf(s->status, sizeof(s->status), "离线列表 · 安装前会重新验证");
    } else {
        snprintf(s->status, sizeof(s->status), "刷新失败 · 按空格重试");
        fprintf(stderr, "c1pkg: %s\n", error);
    }
    end_operation(s);
    free(fresh);
    reload_local(s);
}

enum gui_action { A_INSTALL, A_LAUNCH, A_UPDATE, A_REMOVE, A_CANCEL };
static const char *action_label(enum gui_action action)
{
    switch (action) {
    case A_INSTALL: return "安装";
    case A_LAUNCH: return "打开";
    case A_UPDATE: return "更新";
    case A_REMOVE: return "卸载";
    case A_CANCEL: return "返回";
    }
    return "返回";
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

static void render_detail(struct gui_state *s, const struct c1pkg_download_item *item,
                          const enum gui_action *actions, size_t count, size_t selected, int confirm)
{
    char line[160];
    size_t i;
    memset(s->frame, 0, GUI_BYTES);
    text(s, 8U, 2U, confirm ? "确认卸载" : "应用详情", 280U, true);
    box(s, 6U, 23U, 284U, 1U, true);
    text(s, 8U, 27U, item->name, 280U, true);
    snprintf(line, sizeof(line), "作者 %s", item->author);
    text(s, 8U, 46U, line, 280U, true);
    snprintf(line, sizeof(line), "本地 %s", item->installed_version[0] ? item->installed_version : "未安装");
    text(s, 8U, 66U, line, 280U, true);
    snprintf(line, sizeof(line), "仓库 %s", item->available_version[0] ? item->available_version : "暂无版本");
    text(s, 8U, 86U, confirm ? "将删除该应用的所有已安装版本" : line, 280U, true);
    for (i = 0U; i < count; ++i) {
        unsigned int width = 284U / (unsigned int)count;
        unsigned int x = 6U + (unsigned int)i * width;
        if (i == selected) box(s, x, 108U, width - 2U, 20U, true);
        text(s, x + 4U, 110U, action_label(actions[i]), width - 8U, i != selected);
    }
    text(s, 8U, 134U, "左右选择  确认执行  返回取消", 280U, true);
    present(s);
}

static int choose_action(struct gui_state *s, const struct c1pkg_download_item *item,
                          enum gui_action *actions, size_t count, size_t selection, int confirm)
{
    while (!interrupted && !s->display_failed) {
        int key;
        render_detail(s, item, actions, count, selection, confirm);
        key = gui_key(-1);
        if (gui_cancel_key(key)) return A_CANCEL;
        if ((key == G_LEFT || key == G_UP) && selection > 0U) --selection;
        if ((key == G_RIGHT || key == G_DOWN) && selection + 1U < count) ++selection;
        if (key == G_ENTER) return actions[selection];
    }
    return A_CANCEL;
}

static void gui_release(struct gui_state *s)
{
    c1pkg_set_progress(NULL, NULL);
    if (s->display >= 0) { close(s->display); s->display = -1; }
    c1_app_lease_release(s->lease); s->lease = -1;
    if (s->run_lock >= 0) { c1_app_lease_clear_mode(); c1_app_lease_release(s->run_lock); s->run_lock = -1; }
    if (gui_terminal_saved) { (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &gui_saved_terminal); gui_terminal_saved = 0; }
}

static void manage_gui(struct gui_state *s, const struct c1pkg_config *config)
{
    struct c1pkg_download_item item;
    enum gui_action actions[4];
    size_t count;
    int action, outcome;
    char error[C1PKG_ERROR_MAX] = "";
    if (gui_prefix_unmatched(s)) {
        snprintf(s->status, sizeof(s->status), "无匹配 · 请修改或清空定位后确认");
        return;
    }
    c1pkg_prefix_clear(&s->prefix);
    if (s->count == 0U) return;
    item = s->downloads.items[s->visible[s->selected[s->tab]]];
    count = actions_for(&item, actions);
    /* The installed list is a launcher; management remains on downloads. */
    action = s->tab == 1U && item.installed_version[0] != '\0' ? A_LAUNCH :
             choose_action(s, &item, actions, count, 0U, 0);
    if (action == A_CANCEL) return;
    if (!s->storage_ready) { snprintf(s->status, sizeof(s->status), "存储不可用 · 请检查存储挂载"); return; }
    if (action == A_LAUNCH) {
        gui_release(s);
        execl("/usr/data/c1/bin/c1pkg", "c1pkg", "launch", item.id, (char *)NULL);
        fprintf(stderr, "c1pkg: launch failed: %s\n", strerror(errno));
        s->display_failed = 1;
        interrupted = 1;
        return;
    }
    if (action == A_REMOVE) {
        enum gui_action confirmation[] = {A_REMOVE, A_CANCEL};
        if (choose_action(s, &item, confirmation, 2U, 1U, 1) != A_REMOVE) return;
        snprintf(s->status, sizeof(s->status), "正在卸载应用"); render_gui(s);
        outcome = c1pkg_store_remove_with_run_lock(item.id, s->run_lock, error, sizeof(error));
        snprintf(s->status, sizeof(s->status), outcome == 0 ? "卸载完成" : "卸载失败 · 请检查存储");
    } else {
        /* Cached metadata is browseable but never silently authorizes a new
         * install. Revalidate first and re-resolve by ID, not by stale index. */
        if (!s->verified) {
            refresh_gui(s, config);
            if (!s->verified || s->cancelled || !s->storage_ready) return;
        }
        const struct c1pkg_package *package = c1pkg_repo_find(&s->index, item.id);
        if (package == NULL) { snprintf(s->status, sizeof(s->status), "应用已下架 · 列表已保留"); return; }
        begin_operation(s);
        snprintf(s->status, sizeof(s->status), action == A_UPDATE ? "正在更新并校验应用" : "正在安装并校验应用");
        render_gui(s);
        outcome = c1pkg_store_install(config, package, error, sizeof(error));
        end_operation(s);
        snprintf(s->status, sizeof(s->status), outcome == C1PKG_INSTALL_OK ? "安装完成 · 可在已安装中打开" :
                 outcome == C1PKG_INSTALL_SKIPPED ? "无需安装 · 已保留本地版本" :
                 outcome == C1PKG_INSTALL_CANCELLED || s->cancelled ? "已取消 · 本地应用未替换" :
                 outcome == C1PKG_INSTALL_STORAGE_ERROR ? "存储错误 · 请检查剩余空间" : "安装失败 · 可确认后重试");
    }
    if (error[0]) fprintf(stderr, "c1pkg: %s\n", error);
    reload_local(s);
}

/* Returns nonzero to leave the list. Enter validates the still-visible prefix
 * before clearing it; expiry is painted on the next idle/list frame. */
static int gui_list_key(struct gui_state *s, const struct c1pkg_config *config,
                        int key, uint64_t now_ms)
{
    if (interrupted) return 1;
    if (search_gui(s, key, now_ms)) return 0;
    if (key == G_BACK) {
        if (s->prefix.text[0] == '\0') return 1;
        c1pkg_prefix_clear(&s->prefix);
        return 0;
    }
    if (key == G_NONE) return 0;
    if (key == G_ENTER) {
        manage_gui(s, config);
        return 0;
    }
    c1pkg_prefix_clear(&s->prefix);
    if (key == G_LEFT || key == G_RIGHT) s->tab = key == G_LEFT ? 0U : 1U;
    else if (key == G_UP && s->selected[s->tab] > 0U) --s->selected[s->tab];
    else if (key == G_DOWN && s->selected[s->tab] + 1U < s->count) ++s->selected[s->tab];
    else if (key == G_REFRESH) refresh_gui(s, config);
    select_visible(s);
    return 0;
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
    /* Paint a verified cache immediately before any network waiting. */
    if (c1pkg_repo_load_cached(config, &s->index, NULL, 0U) == 0) reload_local(s);
    refresh_gui(s, config);
    while (!interrupted && !s->display_failed) {
        int key;
        expire_gui_prefix(s, c1pkg_input_now_ms());
        render_gui(s);
        key = gui_key(200);
        if (gui_list_key(s, config, key, c1pkg_input_now_ms())) break;
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
