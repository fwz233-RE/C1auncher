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
    for (i = 0U; i < index->count; ++i)
        if (strcmp(index->packages[i].id, id) == 0) return &index->packages[i];
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
    for (i = 0U; i < local.count; ++i)
        if (strcmp(local.items[i].id, package->id) == 0) strcpy(local.items[i].version, package->version);
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
        strcpy(repository.packages[i].id, ids[i]);
        strcpy(repository.packages[i].name, ids[i]);
        strcpy(repository.packages[i].version, versions[i]);
        strcpy(repository.packages[i].author, "Author");
    }
    for (i = 0U; i < local.count; ++i) {
        strcpy(local.items[i].id, local_ids[i]);
        strcpy(local.items[i].version, local_versions[i]);
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

static void test_persistent_search_tui(struct tui_state *state, const struct c1pkg_config *config)
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
        state->tab = tab;
        state->query[0] = '\0';
        rebuild_visible(state);
        for (i = 0U; i < 5U; ++i)
            dispatch_tui_key(state, config, "PIANO"[i], 100U + i * 100U);
        expect(strcmp(state->query, "piano") == 0 && item_count(state) == 4U && state->selected[tab] == 0U,
               "TUI uses persistent case-insensitive substring search for mixed-language names");
        dispatch_tui_key(state, config, KEY_DOWN, 10000U);
        expect(state->selected[tab] == 1U && strcmp(state->query, "piano") == 0,
               "TUI navigation preserves the search filter indefinitely");
        dispatch_tui_key(state, config, KEY_QUIT, 10001U);
        expect(state->query[0] == '\0' && item_count(state) == 4U,
               "TUI Back clears the filter before leaving the page");
        for (i = 0U; i < 6U; ++i)
            dispatch_tui_key(state, config, "pianoz"[i], 10100U + i);
        expect(tui_query_unmatched(state) && item_count(state) == 0U,
               "TUI empty results are explicit and cannot retain a stale match");
        expect(!dispatch_tui_key(state, config, KEY_ENTER, 20000U) && strstr(state->status, "No match") != NULL,
               "TUI Enter refuses to execute when the persistent filter has no result");
        dispatch_tui_key(state, config, KEY_QUIT, 20001U);
        expect(state->query[0] == '\0' && item_count(state) == 4U,
               "TUI clear-and-retry restores the full list");
    }
}

static void test_hidden_service_tui(struct tui_state *state)
{
    setup(state);
    state->index.count = state->installed.count = 1U;
    strcpy(state->index.packages[0].id, "c1-ime");
    strcpy(state->index.packages[0].name, "C1-IME");
    strcpy(state->index.packages[0].version, "1.0.0");
    strcpy(state->installed.items[0].id, "c1-ime");
    strcpy(state->installed.items[0].version, "1.0.0");
    rebuild_downloads(state);
    expect(state->visible_count[0] == 0U && state->visible_count[1] == 0U,
           "TUI also hides the internal input service from both package lists");
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
    while (ftell(frames) < frame_end && (character = fgetc(frames)) != EOF)
        if (character == '\n') ++lines;
    expect(lines == 19, "author detail preserves the 19-row TUI frame");
    expect(state.tab == 0U, "TUI starts on installed apps");
    strcpy(state.index.packages[0].name, "Paint");
    strcpy(state.index.packages[1].name, "Piano");
    strcpy(state.index.packages[3].name, "Piper");
    strcpy(state.index.packages[4].name, "Radio");
    rebuild_visible(&state);
    state.query[0] = '\0';
    state.selected[0] = 4U;
    rebuild_visible(&state);
    {
        int input[2];
        if (pipe(input) != 0 || dup2(input[0], STDIN_FILENO) < 0) return 1;
        close(input[0]);
        expect(write(input[1], "PIP\177R \r", 7U) == 7, "enqueue complete TUI input burst");
        search_tui(&state, read_key(), 100U);
        expect(state.selected[0] == 0U && strcmp(state.query, "p") == 0, "TUI P filters display names");
        search_tui(&state, read_key(), 200U);
        expect(state.selected[0] == 0U && strcmp(state.query, "pi") == 0, "TUI PI keeps matching names");
        search_tui(&state, read_key(), 300U);
        expect(state.selected[0] == 0U && strcmp(state.query, "pip") == 0, "TUI PIP filters Piper without prefix ranking");
        search_tui(&state, read_key(), 400U);
        expect(state.selected[0] == 0U && strcmp(state.query, "pi") == 0, "TUI backspace restores the broader filter");
        search_tui(&state, read_key(), 1900U);
        expect(state.selected[0] == 0U && strcmp(state.query, "pir") == 0,
               "TUI search remains persistent after idle time");
        expect(read_key() == KEY_REFRESH && read_key() == KEY_ENTER, "TUI Space and Enter are retained in burst");
        close(input[1]);
    }
    state.query[0] = '\0';
    rebuild_visible(&state);
    state.query[0] = 'r'; state.query[1] = 'a'; state.query[2] = 'd'; state.query[3] = 'i'; state.query[4] = 'o'; state.query[5] = '\0';
    rebuild_visible(&state);
    if (setjmp(launched) == 0) perform_action(&state, &config);
    expect(strcmp(launched_id, "update") == 0, "installed Enter launches the selected filtered ID");
    state.tab = 1U;
    rebuild_downloads(&state);
    state.query[0] = 'r'; state.query[1] = 'a'; state.query[2] = 'd'; state.query[3] = 'i'; state.query[4] = 'o'; state.query[5] = '\0';
    rebuild_visible(&state);
    expect(state.visible_count[1] == 1U, "TUI download list uses the same persistent filter");
    test_persistent_search_tui(&state, &config);
    test_hidden_service_tui(&state);
    (void)fclose(frames);
    if (failures != 0) { fprintf(stderr, "%d TUI workflow test(s) failed.\n", failures); return 1; }
    fprintf(stderr, "All package TUI workflow tests passed.\n");
    return 0;
}
