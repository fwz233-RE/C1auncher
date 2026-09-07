/* Compile with util.c and tui_model.c. Exercise the actual refresh/update TUI
 * workflow using in-memory store/repository substitutes, without a device. */
#define execl test_execl
#include "../src/pkg/tui.c"
#undef execl
#include <setjmp.h>

static jmp_buf launched;
static char launched_id[C1PKG_ID_MAX + 1U];
int test_execl(const char *path, const char *arg, ...)
{
    va_list args;
    (void)path;
    va_start(args, arg);
    (void)va_arg(args, const char *);
    snprintf(launched_id, sizeof(launched_id), "%s", va_arg(args, const char *));
    va_end(args);
    longjmp(launched, 1);
}

static int failures, refresh_fails, install_fails, installs;
static char installed_id[C1PKG_ID_MAX + 1U];
static struct c1pkg_index repository;
static struct c1pkg_installed_list local;

static void expect(int condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

int c1pkg_repo_refresh(const struct c1pkg_config *config, struct c1pkg_index *index,
                       char *error, size_t size)
{
    (void)config;
    if (refresh_fails) { c1pkg_set_error(error, size, "offline"); return -1; }
    *index = repository;
    return 0;
}

int c1pkg_repo_load_cached(const struct c1pkg_config *config, struct c1pkg_index *index,
                           char *error, size_t size)
{
    (void)config; (void)error; (void)size;
    *index = repository;
    return 0;
}

const struct c1pkg_package *c1pkg_repo_find(const struct c1pkg_index *index, const char *id)
{
    size_t i;
    for (i = 0U; i < index->count; ++i) if (strcmp(index->packages[i].id, id) == 0) return &index->packages[i];
    return NULL;
}

int c1pkg_store_list(struct c1pkg_installed_list *list, char *error, size_t size)
{
    (void)error; (void)size;
    *list = local;
    return 0;
}

int c1pkg_store_install(const struct c1pkg_config *config, const struct c1pkg_package *package,
                        char *error, size_t size)
{
    size_t i;
    (void)config;
    ++installs;
    strcpy(installed_id, package->id);
    if (install_fails && installs == 1) {
        c1pkg_set_error(error, size, "arbitrary human diagnostic");
        return install_fails;
    }
    for (i = 0U; i < local.count; ++i) {
        if (strcmp(local.items[i].id, package->id) == 0) strcpy(local.items[i].version, package->version);
    }
    return 0;
}

int c1pkg_store_remove(const char *id, char *error, size_t size)
{
    (void)id; (void)error; (void)size;
    return 0;
}

static void setup(struct tui_state *state)
{
    const char *ids[] = {"current", "newer", "uninstalled", "unknown", "update", "update-two"};
    const char *versions[] = {"1.0.0", "1.0.0", "1.0.0", "rolling", "2.0.0", "3.0.0"};
    const char *local_ids[] = {"current", "newer", "unknown", "update", "update-two"};
    const char *local_versions[] = {"1.0.0", "2.0.0", "snapshot", "1.0.0", "3.0.0"};
    size_t i;
    memset(state, 0, sizeof(*state));
    memset(&repository, 0, sizeof(repository));
    memset(&local, 0, sizeof(local));
    repository.sequence = 1U; repository.count = 6U; local.count = 5U;
    for (i = 0U; i < repository.count; ++i) {
        strcpy(repository.packages[i].id, ids[i]); strcpy(repository.packages[i].name, ids[i]);
        strcpy(repository.packages[i].version, versions[i]); strcpy(repository.packages[i].author, "Author");
    }
    for (i = 0U; i < local.count; ++i) {
        strcpy(local.items[i].id, local_ids[i]); strcpy(local.items[i].version, local_versions[i]);
    }
    refresh_fails = 0; install_fails = 0; installs = 0; installed_id[0] = '\0';
}

/* Catch only the standalone launcher handoff; all key handling is production. */
static int dispatch_tui_key(struct tui_state *state, const struct c1pkg_config *config,
                           int key, uint64_t now_ms)
{
    if (setjmp(launched) != 0) return 1;
    (void)tui_list_key(state, config, key, now_ms);
    return 0;
}

static void test_exact_and_unmatched_tui(struct tui_state *state, const struct c1pkg_config *config)
{
    static const char *ids[] = {"a", "b", "piano", "piano-tools"};
    static const char *names[] = {"Piano Lessons", "PIANO", "Piano Keyboard", "Piano"};
    size_t i;
    unsigned int tab;
    setup(state);
    state->index.count = state->installed.count = 4U;
    for (i = 0U; i < 4U; ++i) {
        strcpy(state->index.packages[i].id, ids[i]);
        strcpy(state->index.packages[i].name, names[i]);
        strcpy(state->index.packages[i].version, "1.0.0");
        strcpy(state->installed.items[i].id, ids[i]);
        strcpy(state->installed.items[i].version, "1.0.0");
    }
    rebuild_downloads(state);
    for (tab = 0U; tab < 2U; ++tab) {
        const char *input = "PIANO";
        state->tab = tab;
        c1pkg_prefix_clear(&state->prefix);
        for (i = 0U; input[i]; ++i) dispatch_tui_key(state, config, input[i], 100U + i * 100U);
        expect(state->selected[tab] == 2U,
               "TUI exact piano ID beats preceding long name and exact display name on both tabs");
        /* Remove the exact-ID candidate without changing list positions. */
        strcpy(state->installed.items[2].id, "other");
        strcpy(state->index.packages[2].id, "other");
        strcpy(state->downloads.items[2].id, "other");
        dispatch_tui_key(state, config, C1PKG_KEY_ERASE, 600U);
        expect(state->selected[tab] == 0U, "TUI incomplete pian selects first prefix");
        dispatch_tui_key(state, config, 'o', 700U);
        expect(state->selected[tab] == 1U, "TUI exact display name outranks prefix with stable ties");
        strcpy(state->installed.items[2].id, "piano");
        strcpy(state->index.packages[2].id, "piano");
        strcpy(state->downloads.items[2].id, "piano");
        dispatch_tui_key(state, config, C1PKG_KEY_ERASE, 800U);
        dispatch_tui_key(state, config, 'o', 900U);
        dispatch_tui_key(state, config, 'z', 1000U);
        expect(tui_prefix_unmatched(state) && state->selected[tab] == 2U,
               "TUI no-match suffix retains current selection");
        expect(!dispatch_tui_key(state, config, KEY_ENTER, 2499U) &&
               strcmp(state->prefix.text, "pianoz") == 0 && strstr(state->status, "No match") != NULL,
               "TUI Enter before timeout blocks stale launch/management and retains warning");
        expect(!dispatch_tui_key(state, config, KEY_ENTER, 2500U) && strcmp(state->prefix.text, "pianoz") == 0,
               "TUI Enter at timeout cannot silently clear unmatched input");
        expire_tui_prefix(state, 2500U);
        expect(state->prefix.text[0] == '\0' && state->selected[tab] == 2U &&
               strstr(state->status, "timed out; cleared") != NULL,
               "TUI idle timeout explicitly reports clearing and unchanged selection");
        render(state);
    }
    state->tab = 0U;
    dispatch_tui_key(state, config, 'z', 3000U);
    dispatch_tui_key(state, config, C1PKG_KEY_CLEAR, 3100U);
    expect(dispatch_tui_key(state, config, KEY_ENTER, 3200U) && strcmp(launched_id, "piano") == 0,
           "TUI explicit clear then Enter launches selected ID");
    strcpy(state->prefix.text, "pianoz"); state->prefix.updated_ms = 3300U;
    dispatch_tui_key(state, config, C1PKG_KEY_ERASE, 3400U);
    expect(dispatch_tui_key(state, config, KEY_ENTER, 3500U) && strcmp(launched_id, "piano") == 0,
           "TUI corrected prefix enables exact-ID launch");
    dispatch_tui_key(state, config, 'z', 4000U);
    expire_tui_prefix(state, 5500U); render(state);
    expect(dispatch_tui_key(state, config, KEY_ENTER, 5600U) && strcmp(launched_id, "piano") == 0,
           "TUI launch resumes after timeout clearing is displayed");
}

int main(void)
{
    struct tui_state state;
    struct c1pkg_config config = {"http://example.org/c1/v2", "/unused/key"};
    FILE *frames = tmpfile();
    long frame_start, frame_end;
    int lines = 1, character;
    if (frames == NULL || dup2(fileno(frames), STDOUT_FILENO) < 0) return 1;
    setup(&state);
    refresh(&state, &config, 0);
    expect(installs == 0 && state.index.count == 6U, "startup refresh only loads metadata");
    refresh(&state, &config, 1);
    expect(installs == 1 && strcmp(installed_id, "update") == 0, "Space only updates strictly newer installed application");
    expect(strcmp(local.items[1].version, "2.0.0") == 0 && local.count == 5U,
           "refresh leaves newer local versions and uninstalled apps untouched");
    expect(strstr(state.status, "1 app(s) updated") != NULL, "refresh reports completed update count");
    refresh(&state, &config, 1);
    expect(installs == 1 && strstr(state.status, "0 app(s) updated") != NULL,
           "repeated refresh does not redownload current versions");
    setup(&state); refresh_fails = 1;
    refresh(&state, &config, 1);
    expect(installs == 0 && state.index.count == 6U && strstr(state.status, "Offline") != NULL,
           "offline cached index is visible but never auto-updated");
    setup(&state); install_fails = C1PKG_INSTALL_CANCELLED;
    strcpy(repository.packages[5].version, "4.0.0");
    refresh(&state, &config, 1);
    expect(installs == 1 && strstr(state.status, "Cancelled") != NULL &&
           strcmp(local.items[4].version, "3.0.0") == 0, "cancelled batch reports failure and stops remaining updates");
    setup(&state); install_fails = C1PKG_INSTALL_PACKAGE_ERROR;
    strcpy(repository.packages[5].version, "4.0.0");
    refresh(&state, &config, 1);
    expect(installs == 2 && strcmp(local.items[4].version, "4.0.0") == 0 &&
           strstr(state.status, "U:1 F:1") != NULL, "single package failure continues and appears in final summary");
    setup(&state); install_fails = C1PKG_INSTALL_STORAGE_ERROR;
    strcpy(repository.packages[5].version, "4.0.0");
    refresh(&state, &config, 1);
    expect(installs == 1 && strstr(state.status, "Storage stop") != NULL,
           "typed global storage failure stops batch independent of error text");
    setup(&state); install_fails = C1PKG_INSTALL_SKIPPED;
    strcpy(repository.packages[5].version, "4.0.0");
    refresh(&state, &config, 1);
    expect(installs == 2 && strstr(state.status, "U:1 F:0 S:1") != NULL,
           "safely skipped retained package does not block other updates");
    setup(&state);
    refresh(&state, &config, 0);
    frame_start = ftell(stdout);
    render(&state);
    frame_end = ftell(stdout);
    (void)fseek(frames, frame_start, SEEK_SET);
    while (ftell(frames) < frame_end && (character = fgetc(frames)) != EOF) if (character == '\n') ++lines;
    expect(lines == 19, "author detail preserves the 19-row TUI frame");
    expect(state.tab == 0U, "TUI starts on installed apps");
    strcpy(state.index.packages[0].name, "Paint");
    strcpy(state.index.packages[1].name, "Piano");
    strcpy(state.index.packages[3].name, "Piper");
    strcpy(state.index.packages[4].name, "Radio");
    state.selected[0] = 4U;
    {
        int input[2];
        if (pipe(input) != 0 || dup2(input[0], STDIN_FILENO) < 0) return 1;
        close(input[0]);
        expect(write(input[1], "PIp\177R \r", 7U) == 7, "enqueue complete TUI input burst");
        search_tui(&state, read_key(), 100U);
        expect(state.selected[0] == 0U, "TUI P finds first matching display name");
        search_tui(&state, read_key(), 200U);
        expect(state.selected[0] == 1U, "TUI PI selects Piano");
        search_tui(&state, read_key(), 300U);
        expect(state.selected[0] == 2U, "TUI PIP uniquely selects Piper");
        search_tui(&state, read_key(), 400U);
        expect(state.selected[0] == 1U && strcmp(state.prefix.text, "pi") == 0,
               "TUI backspace restores broader prefix");
        search_tui(&state, read_key(), 1900U);
        expect(state.selected[0] == 3U && strcmp(state.prefix.text, "r") == 0,
               "TUI R searches Radio after timeout, never refreshes");
        expect(read_key() == KEY_REFRESH && read_key() == KEY_ENTER, "TUI Space and Enter are retained in burst");
        close(input[1]);
    }
    if (setjmp(launched) == 0) perform_action(&state, &config);
    expect(strcmp(launched_id, "update") == 0, "installed Enter launches the selected ID, not a name or stale index");
    state.tab = 1U;
    rebuild_downloads(&state);
    c1pkg_prefix_clear(&state.prefix);
    search_tui(&state, 'R', 2000U);
    expect(state.selected[1] == 4U, "TUI download list supports the same name prefix");
    test_exact_and_unmatched_tui(&state, &config);
    (void)fclose(frames);
    if (failures != 0) { fprintf(stderr, "%d TUI workflow test(s) failed\n", failures); return 1; }
    fprintf(stderr, "All package TUI workflow tests passed.\n");
    return 0;
}
