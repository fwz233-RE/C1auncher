/* Hardware-free tests exercise the production graphical workflow and pixels. */
#define execl test_execl
#include "../src/pkg/gui.c"
#undef execl
#include <stdarg.h>

static int launch_calls;
static char launched_id[C1PKG_ID_MAX + 1U];
int test_execl(const char *path, const char *arg, ...)
{
    va_list args;
    (void)path; (void)arg;
    va_start(args, arg);
    (void)va_arg(args, const char *); /* launch */
    snprintf(launched_id, sizeof(launched_id), "%s", va_arg(args, const char *));
    va_end(args);
    ++launch_calls;
    errno = ENOENT;
    return -1;
}

static int failures, storage_fails, refresh_fails, cache_fails, install_calls;
static struct c1pkg_index fixture;
static struct c1pkg_installed_list local;
static void expect(int okay, const char *message)
{
    if (!okay) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
int c1pkg_store_list(struct c1pkg_installed_list *list, char *error, size_t size)
{
    if (storage_fails) { snprintf(error, size, "storage missing"); return -1; }
    *list = local; return 0;
}
int c1pkg_repo_refresh(const struct c1pkg_config *config, struct c1pkg_index *index, char *error, size_t size)
{
    (void)config;
    if (refresh_fails) { errno = 0; snprintf(error, size, "offline"); return -1; }
    if (c1pkg_progress("等待 60 秒后自动继续") != 0) { errno = ECANCELED; return -1; }
    *index = fixture; return 0;
}
int c1pkg_repo_load_cached(const struct c1pkg_config *config, struct c1pkg_index *index, char *error, size_t size)
{
    (void)config; (void)error; (void)size;
    if (cache_fails) return -1;
    *index = fixture; return 0;
}
const struct c1pkg_package *c1pkg_repo_find(const struct c1pkg_index *index, const char *id)
{
    size_t i;
    for (i = 0U; i < index->count; ++i) if (strcmp(index->packages[i].id, id) == 0) return &index->packages[i];
    return NULL;
}
int c1pkg_store_install(const struct c1pkg_config *config, const struct c1pkg_package *package, char *error, size_t size)
{
    (void)config; (void)package; (void)error; (void)size;
    ++install_calls; return 0;
}
int c1pkg_store_remove_with_run_lock(const char *id, int lock, char *error, size_t size)
{ (void)id; (void)lock; (void)error; (void)size; return 0; }

static void reset_gui(struct gui_state *s)
{
    static const char *names[] = {"中文音乐播放器", "电子书阅读器", "录音与语音备忘", "繁體中文工具"};
    size_t i;
    initialize_gui(s); memset(&fixture, 0, sizeof(fixture)); memset(&local, 0, sizeof(local));
    interrupted = storage_fails = refresh_fails = cache_fails = install_calls = 0;
    fixture.sequence = 1U; fixture.count = 4U;
    for (i = 0U; i < fixture.count; ++i) {
        snprintf(fixture.packages[i].id, sizeof(fixture.packages[i].id), "app%lu", (unsigned long)i);
        strcpy(fixture.packages[i].name, names[i]);
        strcpy(fixture.packages[i].author, "中文作者");
        strcpy(fixture.packages[i].version, "1.2.0");
    }
}

static void save_pbm(const struct gui_state *s, const char *path)
{
    unsigned int x, y;
    FILE *out = fopen(path, "wb");
    expect(out != NULL, "preview file opens");
    if (!out) return;
    fprintf(out, "P1\n296 152\n");
    for (y = 0U; y < GUI_HEIGHT; ++y) {
        for (x = 0U; x < GUI_WIDTH; ++x)
            fprintf(out, "%u ", !!(s->frame[y / 8U * GUI_WIDTH + x] & (0x80U >> (y % 8U))));
        fputc('\n', out);
    }
    fclose(out);
}

static void test_exact_and_unmatched_gui(struct gui_state *s, const struct c1pkg_config *config)
{
    static const char *ids[] = {"a", "b", "piano", "piano-tools"};
    static const char *names[] = {"Piano Lessons", "PIANO", "钢琴", "Piano"};
    size_t i;
    unsigned int tab;
    reset_gui(s); launch_calls = 0;
    local.count = 4U;
    for (i = 0U; i < local.count; ++i) {
        strcpy(fixture.packages[i].id, ids[i]);
        strcpy(fixture.packages[i].name, names[i]);
        strcpy(local.items[i].id, ids[i]);
        strcpy(local.items[i].version, "1.2.0");
    }
    refresh_gui(s, config);
    for (tab = 0U; tab < 2U; ++tab) {
        const char *input = "PIANO";
        s->tab = tab; select_visible(s);
        c1pkg_prefix_clear(&s->prefix);
        for (i = 0U; input[i]; ++i) gui_list_key(s, config, input[i], 100U + i * 100U);
        expect(s->selected[tab] == 2U,
               "GUI exact piano ID wins over earlier Piano Lessons and exact display-name entries on both tabs");
        strcpy(s->downloads.items[2].id, "other");
        search_gui(s, C1PKG_KEY_ERASE, 600U);
        expect(s->selected[tab] == 0U, "GUI incomplete pian selects the first prefix");
        gui_list_key(s, config, 'o', 700U);
        expect(s->selected[tab] == 1U, "GUI exact name wins over longer prefix, with stable ties");
        strcpy(s->downloads.items[2].id, "piano");
        gui_list_key(s, config, C1PKG_KEY_ERASE, 800U);
        gui_list_key(s, config, 'o', 900U);
        gui_list_key(s, config, 'z', 1000U);
        expect(gui_prefix_unmatched(s) && s->selected[tab] == 2U, "GUI unmatched suffix retains prior selection");
        gui_list_key(s, config, G_ENTER, 2499U);
        expect(launch_calls == 0 && install_calls == 0 && strcmp(s->prefix.text, "pianoz") == 0 &&
               strstr(s->status, "无匹配") != NULL,
               "GUI Enter before timeout blocks launch/details and preserves the no-match warning");
        gui_list_key(s, config, G_ENTER, 2500U);
        expect(launch_calls == 0 && strcmp(s->prefix.text, "pianoz") == 0,
               "GUI Enter at timeout boundary cannot clear an as-yet-visible unmatched prefix");
        expire_gui_prefix(s, 2500U);
        expect(s->prefix.text[0] == '\0' && s->selected[tab] == 2U && strstr(s->status, "超时清空") != NULL,
               "GUI idle expiry explicitly reports clearing and preserves selection");
        render_gui(s);
    }
    /* A corrected prefix can launch; an explicitly cleared prefix can too. */
    s->tab = 1U; select_visible(s);
    gui_list_key(s, config, 'z', 3000U);
    gui_list_key(s, config, C1PKG_KEY_ERASE, 3100U);
    gui_list_key(s, config, G_ENTER, 3200U);
    expect(launch_calls == 1 && strcmp(launched_id, "piano") == 0,
           "GUI clear then Enter launches the retained selected ID");
    interrupted = 0; s->display_failed = 0;
    strcpy(s->prefix.text, "pianoz"); s->prefix.updated_ms = 3300U;
    gui_list_key(s, config, C1PKG_KEY_ERASE, 3400U);
    gui_list_key(s, config, G_ENTER, 3500U);
    expect(launch_calls == 2 && strcmp(launched_id, "piano") == 0,
           "GUI correcting unmatched suffix enables exact-ID launch");
    interrupted = 0; s->display_failed = 0;
    gui_list_key(s, config, 'z', 4000U);
    expire_gui_prefix(s, 5500U); render_gui(s);
    gui_list_key(s, config, G_ENTER, 5600U);
    expect(launch_calls == 3 && strcmp(launched_id, "piano") == 0,
           "GUI Enter may launch after idle timeout clearing was displayed");
}

int main(int argc, char **argv)
{
    struct gui_state *s = calloc(1U, sizeof(*s));
    struct c1pkg_config config = {"http://example.test", "/unused"};
    enum gui_action actions[4];
    int input[2];
    if (!s || pipe(input) != 0 || dup2(input[0], STDIN_FILENO) < 0) return 1;
    close(input[0]);
    reset_gui(s);
    refresh_gui(s, &config);
    expect(s->storage_ready && s->verified && s->count == 0U && s->tab == 1U,
           "first use stays in installed list even with no installed apps");
    expect(install_calls == 0, "refresh is read-only and never updates installed apps");
    s->tab = 0U; select_visible(s);
    expect(s->count == 4U, "available catalog remains reachable with left key");
    s->selected[0] = 3U; select_visible(s);
    expect(s->offset[0] == 1U, "selection scrolls within three visible rows");
    s->tab = 1U; select_visible(s);
    expect(s->count == 0U && s->selected[1] == 0U, "empty installed tab has safe selection");
    s->tab = 0U; select_visible(s);
    expect(s->selected[0] == 3U, "tabs preserve independent selections");
    storage_fails = 1; reload_local(s);
    expect(!s->storage_ready && s->count == 4U, "storage errors cannot hide server catalog");
    refresh_fails = 1; refresh_gui(s, &config);
    expect(!s->verified && s->count == 4U && strstr(s->status, "离线") != NULL,
           "failed refresh preserves verified old catalog but disables silent stale installs");
    reset_gui(s); refresh_fails = cache_fails = 1; refresh_gui(s, &config);
    expect(s->count == 0U && strstr(s->status, "刷新失败") != NULL, "true empty offline state is explicit");
    reset_gui(s);
    expect(write(input[1], "q", 1U) == 1, "enqueue cancel");
    refresh_gui(s, &config);
    expect(s->cancelled && strstr(s->status, "取消") != NULL && !s->busy,
           "waiting cancellation exits operation without retry or cached install");
    reset_gui(s); refresh_gui(s, &config);
    s->tab = 0U; select_visible(s);
    expect(write(input[1], "\033[A", 3U) == 3, "enqueue navigation sequence");
    begin_operation(s);
    expect(gui_progress("等待 60 秒后自动继续", s) == 0, "arrow escape sequence does not cancel waiting");
    expect(strstr(s->status, "60") != NULL, "countdown text reaches graphical status");
    if (argc > 1) save_pbm(s, argv[1]);
    end_operation(s);
    expect(actions_for(&s->downloads.items[0], actions) == 2U && actions[0] == A_INSTALL,
           "uninstalled app offers install and return");
    strcpy(s->downloads.items[0].installed_version, "1.0.0");
    s->downloads.items[0].status = C1PKG_DOWNLOAD_UPDATE_AVAILABLE;
    expect(actions_for(&s->downloads.items[0], actions) == 4U && actions[0] == A_LAUNCH && actions[2] == A_REMOVE,
           "installed app offers open, update, uninstall and return");
    expect(write(input[1], "\r", 1U) == 1, "enqueue default confirmation");
    { enum gui_action confirm[] = {A_REMOVE, A_CANCEL};
      expect(choose_action(s, &s->downloads.items[0], confirm, 2U, 1U, 1) == A_CANCEL,
             "uninstall confirmation defaults to cancel"); }
    /* Exercise the same byte decoder and prefix selection used by the list. */
    reset_gui(s);
    strcpy(fixture.packages[0].name, "Paint");
    strcpy(fixture.packages[1].name, "Piano");
    strcpy(fixture.packages[2].name, "Piper");
    strcpy(fixture.packages[3].name, "Radio");
    local.count = 4U;
    for (size_t i = 0U; i < local.count; ++i) {
        strcpy(local.items[i].id, fixture.packages[i].id);
        strcpy(local.items[i].version, "1.2.0");
    }
    refresh_gui(s, &config);
    expect(s->tab == 1U && s->count == 4U, "refresh preserves installed default with real entries");
    s->selected[1] = 3U; select_visible(s);
    expect(write(input[1], "PIp", 3U) == 3, "enqueue rapid mixed-case prefix");
    search_gui(s, gui_key(0), 100U);
    expect(s->selected[1] == 0U && s->offset[1] == 0U, "P chooses first P app and scrolls backwards");
    search_gui(s, gui_key(0), 200U);
    expect(s->selected[1] == 1U, "PI chooses Piano");
    search_gui(s, gui_key(0), 300U);
    expect(s->selected[1] == 2U, "PIP uniquely chooses Piper");
    search_gui(s, 'z', 400U);
    expect(s->selected[1] == 2U && strcmp(s->prefix.text, "pipz") == 0,
           "no match keeps selection and prefix for correction");
    search_gui(s, C1PKG_KEY_ERASE, 500U);
    expect(strcmp(s->prefix.text, "pip") == 0, "GUI backspace edits rather than exits");
    search_gui(s, 'R', 2000U);
    expect(s->selected[1] == 3U && s->offset[1] == 1U && install_calls == 0,
           "R after timeout selects Radio without refreshing or installing");
    expect(write(input[1], " \r", 2U) == 2 && gui_key(0) == G_REFRESH && gui_key(0) == G_ENTER,
           "GUI Space refresh and Enter decode independently");
    s->storage_ready = 0;
    manage_gui(s, &config);
    expect(launch_calls == 0, "storage error blocks direct launch without a modal dialog");
    s->storage_ready = 1;
    manage_gui(s, &config);
    expect(launch_calls == 1 && strcmp(launched_id, "app3") == 0 && s->display_failed,
           "installed Enter launches selected ID directly and handles exec failure");
    interrupted = 0; s->display_failed = 0;
    s->tab = 0U; select_visible(s);
    expect(write(input[1], "q", 1U) == 1, "enqueue detail cancel");
    manage_gui(s, &config);
    expect(launch_calls == 1, "download tab still opens management, Q cancels only there");
    test_exact_and_unmatched_gui(s, &config);
    free(s); close(input[1]);
    if (failures) return 1;
    puts("All graphical package workflow tests passed.");
    return 0;
}
