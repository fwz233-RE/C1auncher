/* Production renderer/dispatcher, fork+pipe refresh, real PTY bytes; only the
 * storage, repository and exec boundary is mocked. No device or network I/O. */
#define execl test_execl
#define c1pkg_text observed_text
#include "../src/pkg/gui.c"
#undef execl
#undef c1pkg_text
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>

void c1pkg_text(uint8_t *, int, int, const char *, int, int);
static int failures, storage_fails, refresh_fails, cache_fails, install_calls, refresh_calls;
static int launch_calls, remove_calls, install_result;
static unsigned int refresh_delay;
static int forbidden_worker_fd = -1;
static char launched_id[C1PKG_ID_MAX + 1U], installed_id[C1PKG_ID_MAX + 1U], removed_id[C1PKG_ID_MAX + 1U];
static struct c1pkg_index fixture;
static struct c1pkg_installed_list local;
static struct { unsigned int starts, active, peak, done, cancelled, inherited_fd; } *worker_counts;
struct gui_draw { int x, y, width, black; char text[256]; };
static struct gui_draw drawings[128];
static size_t draw_count;
static void expect(int okay, const char *message)
{
    if (!okay) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
void observed_text(uint8_t *frame, int x, int y, const char *value, int width, int black)
{
    unsigned char before[GUI_BYTES];
    memcpy(before, frame, sizeof(before));
    expect(x >= 0 && y >= 0 && x + width <= 296 && y + 16 <= 152, "every text rectangle fits the display");
    /* Shared chrome deliberately passes a long name to the font's measured
     * clipping API and paints its own dots; package content fits beforehand. */
    expect(y < (int)GUI_HEADER_HEIGHT || c1pkg_text_width(value) <= width,
           "package content is measured and fitted before drawing");
    if (draw_count < 128U) {
        struct gui_draw *d = &drawings[draw_count++];
        d->x = x; d->y = y; d->width = width; d->black = black;
        snprintf(d->text, sizeof(d->text), "%s", value);
    }
    c1pkg_text(frame, x, y, value, width, black);
    int contained = 1;
    for (size_t i = 0U; i < GUI_BYTES; ++i) {
        unsigned char changed = before[i] ^ frame[i];
        if (!changed) continue;
        for (unsigned int bit = 0U; bit < 8U; ++bit)
            if (changed & (0x80U >> bit)) {
                int px = (int)(i % GUI_WIDTH), py = (int)(i / GUI_WIDTH * 8U + bit);
                if (px < x || px >= x + width || py < y || py >= y + 16) contained = 0;
            }
    }
    expect(contained, "every actual changed glyph pixel stays inside its allocated text rectangle");
}
int test_execl(const char *path, const char *arg, ...)
{
    va_list args;
    (void)path; va_start(args, arg); (void)va_arg(args, const char *);
    snprintf(launched_id, sizeof(launched_id), "%s", va_arg(args, const char *));
    va_end(args); ++launch_calls; errno = ENOENT; return -1;
}
int c1pkg_store_list(struct c1pkg_installed_list *list, char *error, size_t size)
{
    if (storage_fails) { snprintf(error, size, "storage missing"); return -1; }
    *list = local; return 0;
}
int c1pkg_repo_bind_transport(const struct c1pkg_config *config)
{ return config && config->repo_base ? 0 : -1; }
int c1pkg_repo_refresh(const struct c1pkg_config *config, struct c1pkg_index *index, char *error, size_t size)
{
    (void)config; ++refresh_calls;
    if (worker_counts && forbidden_worker_fd >= 0 && fcntl(forbidden_worker_fd, F_GETFD) >= 0) ++worker_counts->inherited_fd;
    if (worker_counts) {
        ++worker_counts->starts; ++worker_counts->active;
        if (worker_counts->active > worker_counts->peak) worker_counts->peak = worker_counts->active;
    }
    uint64_t until = c1pkg_input_now_ms() + refresh_delay;
    for (;;) {
        if (c1pkg_progress("等待 60 秒后自动继续")) {
            if (worker_counts) { --worker_counts->active; ++worker_counts->cancelled; }
            errno = ECANCELED; return -1;
        }
        if (c1pkg_input_now_ms() >= until) break;
        (void)poll(NULL, 0U, 5);
    }
    if (worker_counts) { --worker_counts->active; ++worker_counts->done; }
    if (refresh_fails) { snprintf(error, size, "offline"); return -1; }
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
    for (size_t i = 0U; i < index->count; ++i) if (!strcmp(index->packages[i].id, id)) return &index->packages[i];
    return NULL;
}
int c1pkg_store_install(const struct c1pkg_config *config, const struct c1pkg_package *package, char *error, size_t size)
{
    (void)config; (void)error; (void)size;
    strcpy(installed_id, package->id);
    expect(!worker_counts || !worker_counts->active, "install never overlaps a refresh cache writer");
    ++install_calls;
    return c1pkg_progress("第 2/4 次尝试：正在继续下载软件包") ? C1PKG_INSTALL_CANCELLED : install_result;
}
int c1pkg_store_remove_with_run_lock(const char *id, int lock, char *error, size_t size)
{ (void)lock; (void)error; (void)size; strcpy(removed_id, id); ++remove_calls; return 0; }

static void reset_gui(struct gui_state *s)
{
    static const char *names[] = {"中文音乐播放器", "电子书阅读器", "录音与语音备忘", "繁體中文工具"};
    gui_release(s);
    initialize_gui(s);
    memset(&fixture, 0, sizeof(fixture)); memset(&local, 0, sizeof(local));
    s->language = GUI_ZH;
    interrupted = storage_fails = refresh_fails = cache_fails = install_calls = refresh_calls = 0;
    launch_calls = remove_calls = install_result = 0; refresh_delay = 0;
    fixture.sequence = 1U; fixture.count = 4U;
    for (size_t i = 0U; i < fixture.count; ++i) {
        snprintf(fixture.packages[i].id, sizeof(fixture.packages[i].id), "app%lu", (unsigned long)i);
        strcpy(fixture.packages[i].name, names[i]); strcpy(fixture.packages[i].author, "作者 Author");
        strcpy(fixture.packages[i].version, "1.2.0");
    }
}
static void install_fixture(struct gui_state *s, size_t count)
{
    local.count = fixture.count = count;
    for (size_t i = 0U; i < count; ++i) {
        snprintf(fixture.packages[i].id, sizeof(fixture.packages[i].id), "app%lu", (unsigned long)i);
        if (i >= 4U) snprintf(fixture.packages[i].name, sizeof(fixture.packages[i].name), "App %lu", (unsigned long)i);
        strcpy(fixture.packages[i].version, "1.2.0");
        strcpy(local.items[i].id, fixture.packages[i].id); strcpy(local.items[i].version, "1.0.0");
    }
    s->index = fixture; s->verified = 1; reload_local(s);
}
static void finish_refresh(struct gui_state *s)
{
    uint64_t deadline = c1pkg_input_now_ms() + 3000U;
    while (s->refresh.pid > 0 && c1pkg_input_now_ms() < deadline) {
        gui_refresh_tick(s);
        struct pollfd event = {s->refresh.result_fd, POLLIN, 0}; (void)poll(&event, 1U, 5);
    }
    expect(s->refresh.pid == 0, "refresh result is drained and worker reaped within deadline");
}
static size_t descriptor_count(void)
{
    size_t count = 0U;
    DIR *directory = opendir("/proc/self/fd"); struct dirent *entry;
    if (!directory) return 0;
    while ((entry = readdir(directory)) != NULL) if (entry->d_name[0] != '.') ++count;
    closedir(directory); return count;
}
static void queue(int writer, const char *keys)
{ expect(write(writer, keys, strlen(keys)) == (ssize_t)strlen(keys), "queue real input bytes"); }
static void dispatch(struct gui_state *s, const struct c1pkg_config *config, int writer, const char *bytes, size_t count)
{
    queue(writer, bytes);
    for (size_t i = 0; i < count; ++i) {
        int key = gui_key(0);
        expect(key != G_NONE, "queued navigation is decoded immediately");
        (void)gui_ime_input(s, config, key, c1pkg_input_now_ms());
        gui_refresh_tick(s);
    }
}
static void save_pbm(const struct gui_state *s, const char *path)
{
    FILE *out = fopen(path, "wb"); expect(out != NULL, "preview file opens"); if (!out) return;
    fprintf(out, "P1\n296 152\n");
    for (unsigned int y = 0; y < GUI_HEIGHT; ++y) {
        for (unsigned int x = 0; x < GUI_WIDTH; ++x)
            fprintf(out, "%u ", !!(s->frame[y / 8U * GUI_WIDTH + x] & (0x80U >> (y % 8U))));
        fputc('\n', out);
    }
    fclose(out);
}
static int gui_test_pixel(const struct gui_state *s, unsigned int x, unsigned int y)
{
    return !!(s->frame[y / 8U * GUI_WIDTH + x] & (0x80U >> (y % 8U)));
}

static void expect_no_text_overlap(void)
{
    for (size_t i = 0U; i < draw_count; ++i)
        for (size_t j = i + 1U; j < draw_count; ++j) {
            int overlap = drawings[i].x < drawings[j].x + drawings[j].width &&
                          drawings[j].x < drawings[i].x + drawings[i].width &&
                          drawings[i].y < drawings[j].y + 16 && drawings[j].y < drawings[i].y + 16;
            expect(!overlap, "production header, tabs, search, rows, candidates and footer never overlap");
        }
}

static void expect_footer(const struct gui_state *s, const char *expected)
{
    char fitted[C1PKG_ERROR_MAX];
    unsigned int found = 0U;
    gui_fit_text(fitted, sizeof(fitted), expected, 284U);
    for (size_t i = 0U; i < draw_count; ++i) {
        const struct gui_draw *d = &drawings[i];
        if (d->y == (int)GUI_FOOTER_Y) {
            expect(d->x == 6 && d->width == 284 && !strcmp(d->text, fitted),
                   "footer shows the highest-priority actual status within its measured allocation");
            ++found;
        }
        expect(!strstr(d->text, "确认打开") && !strstr(d->text, "确认管理") &&
               !strstr(d->text, "Enter:open") && !strstr(d->text, "Enter:manage") &&
               !strstr(d->text, "Letters jump") && !strstr(d->text, "字母定位"),
               "ordinary lists do not reserve a row for permanent operation tutorials");
    }
    expect(found == 1U, "exactly one information footer is drawn");
    expect(gui_test_pixel(s, 6U, 132U) && gui_test_pixel(s, 289U, 132U) &&
           !gui_test_pixel(s, 6U, 131U) && !gui_test_pixel(s, 6U, 133U),
           "footer separator remains one pixel with clear space above and below");
    expect_no_text_overlap();
}

static void test_catalog_titles(struct gui_state *s)
{
    char title[64];
    reset_gui(s); install_fixture(s, 6U);
    strcpy(s->installed.items[0].id, "c1-ime");
    strcpy(s->index.packages[0].id, "c1-ime");
    s->installed.count = 3;
    s->tab = 0; gui_list_title(s, title, sizeof(title));
    expect(!strcmp(title, "本地2个应用"), "local title excludes the internal service");
    strcpy(s->query, "no matching app"); select_visible(s);
    gui_list_title(s, title, sizeof(title));
    expect(!strcmp(title, "本地2个应用"), "filter does not change total application count");
    s->tab = 1; gui_list_title(s, title, sizeof(title));
    expect(!strcmp(title, "在线5个应用"), "store count includes only visible catalog packages, not local-only apps");
    s->language = GUI_EN; gui_list_title(s, title, sizeof(title));
    expect(!strcmp(title, "Store 5 apps"), "English title also includes count");
    s->index.sequence = 0; gui_list_title(s, title, sizeof(title));
    expect(!strcmp(title, "Store count unknown"), "no catalog is unknown, not a made-up zero");
    s->index.sequence = 1; s->index.count = 0; gui_list_title(s, title, sizeof(title));
    expect(!strcmp(title, "Store 0 apps"), "verified empty catalog is a real zero");
    s->tab = 0; s->storage_ready = 0; gui_list_title(s, title, sizeof(title));
    expect(!strcmp(title, "Local count unknown"), "unavailable local storage is unknown");
    draw_count = 0; render_header(s, "Piano");
    expect(draw_count >= 1 && !strcmp(drawings[0].text, "Piano"), "management detail retains its application name");
}

static void test_toolbar(struct gui_state *s)
{
    reset_gui(s); install_fixture(s, 6U);
    for (int language = GUI_ZH; language <= GUI_EN; ++language) {
        s->language = (enum gui_language)language;
        for (s->tab = 0U; s->tab < 2U; ++s->tab) {
            select_visible(s); draw_count = 0; render_gui(s);
            for (unsigned int y = 18U; y < 36U; ++y)
                expect(!gui_test_pixel(s, 0U, y) && !gui_test_pixel(s, 295U, y) &&
                       !gui_test_pixel(s, 134U, y), "toolbar stays paper-white at the edges and between groups");
            for (unsigned int x = 0U; x < 58U; ++x) {
                expect(!gui_test_pixel(s, 6U + s->tab * 64U + x, 35U) &&
                       !gui_test_pixel(s, 6U + (1U - s->tab) * 64U + x, 35U),
                       "tabs keep the paper background without an underline");
            }
            expect(gui_test_pixel(s, 10U + s->tab * 64U, 18U) &&
                   gui_test_pixel(s, 10U + s->tab * 64U, 34U) &&
                   !gui_test_pixel(s, 10U + s->tab * 64U, 26U) &&
                   !gui_test_pixel(s, 10U + (1U - s->tab) * 64U, 18U),
                   "selected tab uses open corner brackets rather than a solid triangle");
            unsigned int labels = 0U, hints = 0U, headings = 0U;
            for (size_t i = 0U; i < draw_count; ++i) {
                const struct gui_draw *d = &drawings[i];
                expect(!strstr(d->text, "表情") && !strstr(d->text, "Emoji") && !strstr(d->text, "Tab"),
                       "browser never advertises the unverified physical emoji search shortcut");
                if (d->y == 18 && d->x < 128) {
                    expect(d->black, "both tabs use black lettering on white paper");
                    ++labels;
                }
                if (d->y == 18 && d->x == 164 && d->black &&
                    !strcmp(d->text, language == GUI_ZH ? "管理" : "Manage")) ++hints;
                if (d->y == 18 && d->x == 244 && d->black &&
                    !strcmp(d->text, language == GUI_ZH ? "次数" : "Inst")) ++headings;
            }
            expect(labels == 2U && hints == 1U && headings == s->tab,
                   "tabs and Shift management share one compact row; only the store has an installation heading");
            unsigned int selected_y = gui_list_y(s);
            expect(gui_test_pixel(s, 6U, selected_y) && gui_test_pixel(s, 289U, selected_y) &&
                   gui_test_pixel(s, 6U, selected_y + 17U) && gui_test_pixel(s, 289U, selected_y + 17U),
                   "both local and store selected rows retain complete rectangular black corners");
            expect(gui_test_pixel(s, 153U, 19U) && gui_test_pixel(s, 147U, 25U) &&
                   gui_test_pixel(s, 159U, 25U) && gui_test_pixel(s, 153U, 32U),
                   "Shift arrow tip, head and base share a center");
            for (unsigned int y = 18U; y <= 33U; ++y) {
                for (unsigned int x = 147U; x <= 159U; ++x) {
                    expect(gui_test_pixel(s, x, y) == gui_test_pixel(s, 306U - x, y),
                           "every Shift arrow row is horizontally symmetric");
                    if (y == 18U || y == 33U)
                        expect(!gui_test_pixel(s, x, y), "Shift arrow has equal top and bottom margins on label row");
                }
                for (unsigned int x = 160U; x < 164U; ++x)
                    expect(!gui_test_pixel(s, x, y), "Shift arrow leaves four clear pixels before its label");
            }
            expect_footer(s, GUI_LABEL(s, "商店已验证 空格刷新", "Store verified; Space:refresh"));
        }
    }
    s->tab = 0U;
}

static void save_scene(struct gui_state *s, const char *path, const char *suffix)
{
    char output[4096];
    draw_count = 0; render_gui(s); expect_no_text_overlap();
    if (path && snprintf(output, sizeof(output), "%s%s", path, suffix) < (int)sizeof(output))
        save_pbm(s, output);
}

static void test_layout(struct gui_state *s, const char *path)
{
    reset_gui(s); install_fixture(s, 6U);
    for (int language = GUI_ZH; language <= GUI_EN; ++language) {
        s->language = (enum gui_language)language;
        for (int mode = 0; mode < 4; ++mode) {
            s->search_editing = mode > 0 && mode < 3;
            s->input_method.enabled = mode == 2;
            strcpy(s->query, mode == 3 ? "app" : "");
            if (mode == 2) {
                strcpy(s->ime_view.preedit, "zhongwenchangchangdepininyin");
                s->ime_view.candidate_count = C1_IME_MAX_CANDIDATES;
                for (size_t i = 0U; i < C1_IME_MAX_CANDIDATES; ++i)
                    strcpy(s->ime_view.candidates[i], "中文长候选");
            }
            select_visible(s); draw_count = 0; render_gui(s);
            expect(GUI_HEADER_HEIGHT == 18U && gui_rows(s) == (mode == 2 ? 2U : mode ? 4U : 5U),
                   "browse/search/IME allocate five/four/two rows without reserving an idle status line");
            expect(gui_list_y(s) == (mode ? 54U : 37U), "only an active search or filter moves the list down");
            int header_items = 0, list_names = 0, searches = 0, missing_counts = 0;
            char expected_title[64];
            snprintf(expected_title, sizeof(expected_title), GUI_LABEL(s, "本地%zu个应用", "Local %zu apps"),
                     gui_tab_count(s, 0U));
            for (size_t i = 0U; i < draw_count; ++i) {
                const struct gui_draw *d = &drawings[i];
                if (d->y < 18) {
                    ++header_items;
                    expect(!strcmp(d->text, expected_title) || strchr(d->text, '%'),
                           "header shows the current local/store application count and telemetry");
                }
                if (d->x == 10 && d->y >= (int)gui_list_y(s) && d->y < 131) ++list_names;
                if (d->x == 6 && d->y == (int)GUI_SEARCH_Y) ++searches;
                if (d->x == 244 && d->y > 18 && !strcmp(d->text, "--")) ++missing_counts;
                expect(strstr(d->text, "Tab") == NULL, "physical emoji key is never labelled Tab");
            }
            expect(header_items == 2, "header has exactly two text regions");
            expect(list_names == (int)gui_rows(s) && missing_counts == 0,
                   "every local row is used without reserving unknown-statistic placeholders");
            expect(searches == (mode ? 1 : 0), "search row appears only while editing or filtering");
            unsigned int y = gui_list_y(s);
            for (unsigned int x = 6U; x < 290U; ++x)
                expect(gui_test_pixel(s, x, y) && gui_test_pixel(s, x, y + 17U) &&
                       !gui_test_pixel(s, x, y + 18U), "selection top and bottom are uninterrupted black with a white row gap");
            for (unsigned int edge_y = y; edge_y < y + 18U; ++edge_y)
                expect(gui_test_pixel(s, 6U, edge_y) && gui_test_pixel(s, 289U, edge_y),
                       "selection left and right edges remain solid, including all four right-angle corners");
            expect_footer(s, GUI_LABEL(s, "商店已验证 空格刷新", "Store verified; Space:refresh"));
            gui_ime_close(s);
        }
    }
    s->language = GUI_ZH; s->query[0] = '\0'; s->search_editing = 0;
    select_visible(s); save_scene(s, path, "");
    s->tab = 1U; select_visible(s); save_scene(s, path, ".store.pbm");
    s->refresh.pid = 123; strcpy(s->refresh_status, "等待 60 秒后自动继续");
    save_scene(s, path, ".refresh.pbm");
    s->search_editing = 1; strcpy(s->query, "app"); select_visible(s);
    save_scene(s, path, ".search.pbm");
    s->input_method.enabled = true;
    strcpy(s->ime_view.preedit, "zhongwen");
    s->ime_view.candidate_count = 5U; s->ime_view.highlighted_candidate = 1U;
    for (size_t i = 0U; i < 5U; ++i) strcpy(s->ime_view.candidates[i], i == 1U ? "中午" : "中文");
    select_visible(s); save_scene(s, path, ".ime.pbm");
    s->language = GUI_EN; strcpy(s->refresh_status, "Retry in 60 s; auto-resume");
    save_scene(s, path, ".ime-en.pbm");
    s->refresh.pid = 0; gui_ime_close(s); s->query[0] = '\0'; s->search_editing = 0;
    strcpy(s->status, "Installation failed: insufficient storage; please check the mount and available space");
    select_visible(s); save_scene(s, path, ".error-en.pbm");
    s->status[0] = '\0'; save_scene(s, path, ".store-en.pbm");
    s->language = GUI_ZH;
    strcpy(s->status, "安装失败：存储空间不足，请检查挂载状态和可用空间后重新尝试");
    save_scene(s, path, ".error.pbm");
    s->count = 0U; save_scene(s, path, ".empty.pbm");
    s->status[0] = '\0'; s->tab = 0U; select_visible(s);
    if (path) {
        char detail_path[4096];
        if (snprintf(detail_path, sizeof(detail_path), "%s.details.pbm", path) < (int)sizeof(detail_path)) {
            enum gui_action actions[] = {A_LAUNCH, A_UPDATE, A_REMOVE, A_CANCEL};
            draw_count = 0; render_detail(s, &s->downloads.items[0], actions, 4U, 3U, 0);
            expect_no_text_overlap(); save_pbm(s, detail_path);
        }
    }
}

static void test_long_text_layout(struct gui_state *s)
{
    reset_gui(s); install_fixture(s, 6U);
    for (int language = GUI_ZH; language <= GUI_EN; ++language) {
        s->language = (enum gui_language)language;
        const char *name = GUI_LABEL(s, "中文中文中文中文中文中文中", "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMN");
        strcpy(s->downloads.items[0].name, name);
        s->metrics.available = 1; s->metrics.count = 1U;
        strcpy(s->metrics.items[0].id, "app0"); s->metrics.items[0].installations = UINT64_MAX;
        for (s->tab = 0U; s->tab < 2U; ++s->tab) {
            select_visible(s);
            draw_count = 0; render_gui(s); expect_no_text_overlap();
            int names = 0, counts = 0;
            for (size_t i = 0U; i < draw_count; ++i) {
                const struct gui_draw *d = &drawings[i];
                if (d->y == (int)gui_list_y(s) + 1 && (d->x == 10 || d->x == 244)) {
                    size_t length = strlen(d->text);
                    expect(length >= 3U && !strcmp(d->text + length - 3U, "..."),
                           "long bilingual names and store statistics are visibly clipped in their own columns");
                    if (d->x == 10) {
                        expect(d->width == (s->tab ? 150 : 200),
                               "local names reclaim the unused statistics column without changing store layout");
                        ++names;
                    } else ++counts;
                }
            }
            expect(names == 1 && counts == (int)s->tab,
                   "both tabs fit long names, while only the store renders a statistic");
        }
        s->tab = 0U;
        s->search_editing = 1; strcpy(s->query, name); select_visible(s);
        draw_count = 0; render_gui(s); expect_no_text_overlap();
        int searches = 0;
        for (size_t i = 0U; i < draw_count; ++i)
            if (drawings[i].x == 6 && drawings[i].y == (int)GUI_SEARCH_Y) ++searches;
        expect(searches == 1, "a full-byte-budget query keeps a separate measured search field");
        s->query[0] = '\0'; s->search_editing = 0; select_visible(s);
    }
}

static void test_footer_status(struct gui_state *s)
{
    reset_gui(s); install_fixture(s, 6U);
    for (int language = GUI_ZH; language <= GUI_EN; ++language) {
        s->language = (enum gui_language)language;
        s->status[0] = '\0'; s->verified = 0; s->index.sequence = 0U;
        draw_count = 0; render_gui(s);
        expect_footer(s, GUI_LABEL(s, "商店状态未知 空格刷新", "Store unknown; Space:refresh"));
        s->index.sequence = 1U;
        draw_count = 0; render_gui(s);
        expect_footer(s, GUI_LABEL(s, "离线列表 空格刷新", "Offline catalog; Space:refresh"));
        strcpy(s->status, GUI_LABEL(s, "安装完成", "Installed"));
        draw_count = 0; render_gui(s); expect_footer(s, s->status);
        s->refresh.pid = 123;
        strcpy(s->refresh_status, GUI_LABEL(s, "后台刷新 可继续选择", "Refreshing; navigation available"));
        draw_count = 0; render_gui(s); expect_footer(s, s->refresh_status);
        s->busy = 1;
        gui_progress_label(s, "第 2/4 次尝试：正在继续下载软件包");
        draw_count = 0; render_gui(s); expect_footer(s, s->status);
        s->busy = 0;
        s->search_editing = 1; s->input_method.enabled = true; select_visible(s);
        s->storage_ready = 0;
        draw_count = 0; render_gui(s);
        expect_footer(s, GUI_LABEL(s, "存储不可用 请检查挂载", "Storage unavailable; check mount"));
        s->ime_notice = 1;
        strcpy(s->status, GUI_LABEL(s, "搜索输入错误：文字过长，无法提交，请缩短搜索文字后重新输入",
                                   "Search error: input is too long to submit; shorten the query and try again"));
        draw_count = 0; render_gui(s); expect_footer(s, s->status);
        expect(strlen(drawings[draw_count - 1U].text) >= 3U &&
               !strcmp(drawings[draw_count - 1U].text + strlen(drawings[draw_count - 1U].text) - 3U, "..."),
               "long bilingual failures end in a visible ellipsis rather than touching the display edge");
        s->refresh.pid = 0; s->storage_ready = 1; s->ime_notice = 0; s->search_editing = 0;
        gui_ime_close(s); select_visible(s);
    }
}
static void test_search(struct gui_state *s, const struct c1pkg_config *config, int writer)
{
    reset_gui(s); install_fixture(s, 6U);
    strcpy(s->downloads.items[0].name, "Piano Lessons");
    strcpy(s->downloads.items[1].name, "钢琴 Piano");
    dispatch(s, config, writer, "PI", 2);
    expect(!s->query[0] && s->selected[0] == 0U && s->count == 6U,
           "letters in browse mode jump to the matching English name without entering search");
    dispatch(s, config, writer, "\tPI", 3);
    expect(s->search_editing && !strcmp(s->query, "pi") && s->count == 2U, "legacy search key opens persistent case-folded substring filter");
    dispatch(s, config, writer, "\033[57441u", 1);
    expect(s->search_editing && !strcmp(s->query, "pi") && !launch_calls && !install_calls && !remove_calls,
           "standalone Shift while editing cannot open management or discard the query");
    dispatch(s, config, writer, "\033[B", 1);
    expect(s->selected[0] == 1 && !strcmp(s->query, "pi"), "arrows navigate matching rows without clearing query");
    (void)gui_list_key(s, config, G_NONE, 900000U);
    expect(!strcmp(s->query, "pi"), "query persists after arbitrary idle time");
    dispatch(s, config, writer, "\r", 1);
    expect(!s->search_editing && !launch_calls && !install_calls && s->count == 2U, "Enter applies filter and cannot launch from search field");
    dispatch(s, config, writer, "\tzz\r\r", 5);
    expect(!s->count && !launch_calls && !install_calls, "empty filtered results never execute an old selection");
    expect(!gui_list_key(s, config, G_BACK, 0U) && !s->query[0] && s->count == 6U, "Back clears applied filter first");
    expect(gui_list_key(s, config, G_BACK, 0U), "Back exits only after filter layer is cleared");
    dispatch(s, config, writer, "\t", 1);
    expect(gui_append_commit(s, "钢琴", 0U) && s->count == 1U, "Chinese and English names coexist and Chinese substring filters");
    search_gui(s, C1PKG_KEY_ERASE, 0U); expect(!strcmp(s->query, "钢"), "erase removes complete UTF-8 character");
    search_gui(s, G_REFRESH, 0U); expect(!strcmp(s->query, "钢 "), "Space inserts text in explicit search, never refreshes");
}
static void test_letter_jump_and_hidden_service(struct gui_state *s, const struct c1pkg_config *config)
{
    reset_gui(s);
    fixture.count = local.count = 3U;
    strcpy(fixture.packages[0].id, "app-z1");
    strcpy(fixture.packages[0].name, "中文音乐");
    strcpy(fixture.packages[1].id, "app-z2");
    strcpy(fixture.packages[1].name, "中文工具");
    strcpy(fixture.packages[2].id, "c1-ime");
    strcpy(fixture.packages[2].name, "C1-IME");
    for (size_t i = 0U; i < fixture.count; ++i) {
        strcpy(fixture.packages[i].version, "1.2.0");
        strcpy(local.items[i].id, fixture.packages[i].id);
        strcpy(local.items[i].version, "1.0.0");
    }
    s->index = fixture;
    s->verified = 1;
    reload_local(s);
    expect(s->count == 2U && !c1pkg_is_internal_id(s->downloads.items[s->visible[0]].id),
           "internal c1-ime is hidden from the local application page");
    s->tab = 1U;
    select_visible(s);
    expect(s->count == 2U && !c1pkg_is_internal_id(s->downloads.items[s->visible[0]].id),
           "internal c1-ime is hidden from the store application page");
    s->tab = 0U;
    s->selected[0] = 0U;
    gui_list_key(s, config, 'z', 100U);
    expect(s->selected[0] == 0U, "the first Chinese pinyin initial selects the first matching app");
    gui_list_key(s, config, 'z', 200U);
    expect(s->selected[0] == 1U, "repeating an initial cycles to the next Chinese app");
    gui_list_key(s, config, 'z', 300U);
    expect(s->selected[0] == 0U, "repeating an initial wraps to the first Chinese app");
    strcpy(s->downloads.items[s->visible[0]].name, "Alpha");
    strcpy(s->downloads.items[s->visible[1]].name, "Beta");
    gui_list_key(s, config, 'B', 2000U);
    expect(s->selected[0] == 1U, "uppercase English letters jump by display-name initial");
    gui_list_key(s, config, 'A', 3000U);
    expect(s->selected[0] == 0U, "a different English letter starts a new jump cycle");
    strcpy(s->downloads.items[s->visible[1]].name, "Music");
    gui_list_key(s, config, 'M', 4000U);
    expect(s->selected[0] == 1U && !launch_calls && !install_calls && !remove_calls,
           "M remains an application initial and never opens management");

    /* Browse keys form a real prefix instead of performing three unrelated
     * first-letter jumps. A short PIN prefix must select the stable `pinao`
     * ID even when an unrelated Chinese app appears before it. */
    reset_gui(s);
    fixture.count = local.count = 3U;
    strcpy(fixture.packages[0].id, "netease-cloud-music");
    strcpy(fixture.packages[0].name, "网易云音乐");
    strcpy(fixture.packages[1].id, "pinao");
    strcpy(fixture.packages[1].name, "Pinao");
    strcpy(fixture.packages[2].id, "picture");
    strcpy(fixture.packages[2].name, "图片浏览");
    for (size_t i = 0U; i < fixture.count; ++i) {
        strcpy(fixture.packages[i].version, "1.0.0");
        strcpy(local.items[i].id, fixture.packages[i].id);
        strcpy(local.items[i].version, "1.0.0");
    }
    s->index = fixture; s->verified = 1; reload_local(s);
    gui_list_key(s, config, 'P', 100U);
    gui_list_key(s, config, 'i', 150U);
    gui_list_key(s, config, 'N', 200U);
    expect(!strcmp(s->downloads.items[s->visible[s->selected[0]]].id, "pinao"),
           "PiN selects Pinao by the complete ASCII prefix, not a first-letter fallback");
}

static void test_async_navigation(struct gui_state *s, const struct c1pkg_config *config, int writer)
{
    reset_gui(s); install_fixture(s, 6U); refresh_delay = 800U;
    int inherited = open("/dev/null", O_RDONLY);
    forbidden_worker_fd = fcntl(inherited, F_DUPFD, 100); close(inherited);
    size_t before = descriptor_count();
    uint64_t start = c1pkg_input_now_ms();
    refresh_gui(s, config);
    pid_t worker = s->refresh.pid;
    expect(worker > 0 && !s->busy, "refresh starts a separate worker, never occupies navigation");
    for (int i = 0; i < 20; ++i) refresh_gui(s, config);
    expect(s->refresh.pid == worker, "repeated refresh requests coalesce into one worker");
    dispatch(s, config, writer, "\033[B\033[B\033[B\033[B\033[B", 5);
    expect(s->selected[0] == 5U && s->offset[0] == 1U, "real Down bytes scroll the five-row local list during network delay");
    dispatch(s, config, writer, "\033[C\033[B\033[B\033[D", 4);
    expect(s->tab == 0U && s->selected[0] == 5U && s->selected[1] == 2U, "left/right and both page selections work during refresh");
    expect(c1pkg_input_now_ms() - start < 250U && s->refresh.pid > 0, "navigation completes before delayed refresh result");
    dispatch(s, config, writer, "\033[57441uq", 1);
    expect(!launch_calls && !install_calls && !remove_calls && s->refresh.pid > 0 &&
           c1pkg_input_now_ms() - start < 350U,
           "Shift management opens and cancels promptly during a network refresh");
    finish_refresh(s);
    expect(s->verified && s->selected[0] == 5U && s->selected[1] == 2U, "signed result preserves both selected application IDs");
    expect(descriptor_count() == before, "successful worker leaves no pipe descriptors");
    expect(!worker_counts->inherited_fd, "worker closes inherited GUI/IME/lease descriptors before repository I/O");
    int status;
    expect(waitpid(worker, &status, WNOHANG) == -1 && errno == ECHILD, "successful worker is already reaped");
    /* A current local app still launches during refresh and cancels only its
     * own worker. An app displaying an update is covered by prompt tests. */
    strcpy(local.items[5].version, "1.2.0"); reload_local(s);
    int live[2]; expect(pipe(live) == 0, "create unrelated child channel");
    pid_t unrelated = fork();
    if (!unrelated) { char byte; close(live[1]); ssize_t n = read(live[0], &byte, 1); _exit(n == 0 ? 0 : 1); }
    close(live[0]); refresh_delay = 2000U; refresh_gui(s, config);
    dispatch(s, config, writer, "\r", 1);
    expect(launch_calls == 1 && !strcmp(launched_id, "app5") && s->refresh.pid == 0, "local Enter executes while refresh was delayed and cleans its worker");
    expect(waitpid(unrelated, &status, WNOHANG) == 0, "refresh cancellation does not signal unrelated or install children");
    close(live[1]); expect(waitpid(unrelated, &status, 0) == unrelated, "unrelated child exits normally");
    interrupted = 0; s->display_failed = 0;
    expect(descriptor_count() == before, "cancel/launch leaves no refresh descriptors");
    for (int i = 0; i < 16; ++i) {
        refresh_gui(s, config); gui_refresh_dispose(&s->refresh);
        expect(descriptor_count() == before, "repeated immediate cancellation has stable fd count");
    }
    close(forbidden_worker_fd); forbidden_worker_fd = -1;
    /* Drop a worker before it can deliver a full verified result. */
    s->verified = 0; cache_fails = 1;
    refresh_gui(s, config);
    expect(kill(s->refresh.pid, SIGKILL) == 0, "simulate refresh child crash");
    finish_refresh(s);
    expect(!s->verified && s->index.sequence == 1U && s->count == 6U,
           "crashed/partial pipe never verifies data and keeps old library");
    /* A clean successor must retain selected ID, not its obsolete array index. */
    refresh_delay = 0;
    struct c1pkg_package swap = fixture.packages[0];
    fixture.packages[0] = fixture.packages[5]; fixture.packages[5] = swap;
    fixture.sequence = 2U;
    refresh_gui(s, config); finish_refresh(s);
    expect(s->verified && !strcmp(s->downloads.items[s->visible[s->selected[0]]].id, "app5"),
           "catalog reorder preserves selected application by stable ID");
}
static void test_workflow(struct gui_state *s, const struct c1pkg_config *config, int writer)
{
    reset_gui(s); reload_local(s); refresh_gui(s, config); finish_refresh(s);
    expect(s->tab == 0 && !s->count && s->verified && !install_calls, "first entry stays in empty local list and never installs");
    gui_list_key(s, config, G_MANAGE, 0U);
    expect(!install_calls && !remove_calls && !launch_calls, "Shift on an empty page cannot act on an old selection");
    gui_list_key(s, config, G_RIGHT, 0U);
    queue(writer, "q"); gui_list_key(s, config, G_ENTER, 0U);
    expect(!install_calls && !launch_calls, "store Enter opens cancellable management instead of installing or launching");
    queue(writer, "\033[D\r"); gui_list_key(s, config, G_ENTER, 0U);
    expect(install_calls == 1, "store installation requires choosing Install and a second explicit confirmation");
    s->verified = 0; refresh_fails = 1;
    queue(writer, "\033[D\r"); gui_list_key(s, config, G_ENTER, 0U); finish_refresh(s);
    expect(!s->verified && install_calls == 1 && s->count == 4U, "failed verification preserves signed browse data but blocks install");
    refresh_fails = 0; refresh_delay = 200U;
    queue(writer, "\033[D\r"); gui_list_key(s, config, G_ENTER, 0U); finish_refresh(s);
    expect(s->verified && install_calls == 1, "background verification never auto-installs a deferred selection");
    gui_list_key(s, config, G_DOWN, 0U);
    queue(writer, "\033[D\r"); gui_list_key(s, config, G_ENTER, 0U);
    expect(install_calls == 2, "fresh confirmation acts on current selection after validation");
    for (int outcome = C1PKG_INSTALL_STORAGE_ERROR; outcome <= C1PKG_INSTALL_SKIPPED; ++outcome) {
        install_result = outcome; s->verified = 1;
        queue(writer, "\033[D\r"); gui_list_key(s, config, G_ENTER, 0U);
        expect(!s->busy && s->status[0], "every install result restores interactive state with explicit status");
    }
    queue(writer, "\033[D\rq"); s->verified = 1;
    gui_list_key(s, config, G_ENTER, 0U);
    expect(s->cancelled && !s->busy, "installation cancel still uses the parent's existing operation callback");
    reset_gui(s); install_fixture(s, 6U);
    dispatch(s, config, writer, "\033[57441u\r", 1);
    expect(!launch_calls && !install_calls && !remove_calls, "decoded Shift opens management with Cancel selected");
    dispatch(s, config, writer, "\033[57441u\033[D\033[D\r", 1);
    expect(install_calls == 1, "Shift management Update uses original verified install interface");
    dispatch(s, config, writer, "\033[57441u\033[D\r\rq", 1);
    expect(!remove_calls, "Shift management uninstall confirmation defaults to Cancel");
    dispatch(s, config, writer, "\033[57441u\033[D\r\033[D\r", 1);
    expect(remove_calls == 1, "Shift management double confirmation removes application");
    s->tab = 1U; select_visible(s);
    queue(writer, "\r"); gui_list_key(s, config, G_ENTER, 0U);
    expect(!launch_calls && install_calls == 1 && remove_calls == 1,
           "store Enter on an installed app opens management, never implicitly launches or updates");
    storage_fails = 1; reload_local(s); s->tab = 0U; select_visible(s);
    queue(writer, "\033[D\033[D\033[D\r"); gui_list_key(s, config, G_ENTER, 0U);
    expect(s->count == 6U && !launch_calls && install_calls == 1,
           "storage errors preserve the update prompt but still prohibit an explicit Open action");
    reset_gui(s); install_fixture(s, 6U); memset(&s->index, 0, sizeof(s->index));
    cache_fails = refresh_fails = 1; reload_local(s); refresh_gui(s, config); finish_refresh(s);
    expect(s->count == 6U && !s->verified, "fully offline local library remains available without signed metadata");
    queue(writer, "q"); gui_list_key(s, config, G_MANAGE, 0U);
    expect(!launch_calls && !install_calls && !remove_calls, "offline local Shift still opens a cancellable management page");
    gui_list_key(s, config, G_ENTER, 0U);
    expect(launch_calls == 1, "local confirmation directly opens the app, including offline");
}
static void test_management_after_resize(struct gui_state *s, const struct c1pkg_config *config, int writer)
{
    reset_gui(s); install_fixture(s, 8U);
    dispatch(s, config, writer, "\033[B\033[B\033[B\033[B\033[B\033[B\033[B", 7U);
    expect(s->selected[0] == 7U && s->offset[0] == 3U, "five-row list scrolls to the last app");
    dispatch(s, config, writer, "/", 1U);
    expect(gui_rows(s) == 4U && s->selected[0] == 7U && s->offset[0] == 4U,
           "opening search reduces visible rows while keeping the selected app in view");
    dispatch(s, config, writer, "\r", 1U);
    expect(gui_rows(s) == 5U && s->selected[0] == 7U && s->offset[0] == 3U && !launch_calls,
           "applying an empty filter restores the full last page without launching");
    dispatch(s, config, writer, "\033[57441u\r", 1U);
    expect(!launch_calls && !install_calls && !remove_calls && s->selected[0] == 7U,
           "decoded Shift after layout changes still defaults management to Cancel");
    dispatch(s, config, writer, "\033[57441u\033[D\033[D\r", 1U);
    expect(install_calls == 1 && !strcmp(installed_id, "app7"),
           "Shift Update targets the actual scrolled selection after dynamic row changes");
    draw_count = 0; render_gui(s); expect_footer(s, s->status);
    dispatch(s, config, writer, "\033[57441u\033[D\r\r\033[D\033[D\r", 1U);
    expect(!remove_calls && install_calls == 2 && !strcmp(installed_id, "app7"),
           "cancelled removal returns to management, where a new explicit Update remains usable");
    dispatch(s, config, writer, "\033[57441u\033[D\r\033[D\r", 1U);
    expect(remove_calls == 1 && !strcmp(removed_id, "app7"),
           "removal requires both explicit confirmations and keeps the stable selected ID");
    draw_count = 0; render_gui(s); expect_footer(s, GUI_LABEL(s, "卸载完成", "App removed"));
    dispatch(s, config, writer, "\033[C\r\r", 2U);
    expect(s->tab == 1U && !launch_calls && install_calls == 2 && remove_calls == 1,
           "store Enter opens management and its default Enter cancels without performing actions");
    dispatch(s, config, writer, "\033[D\rq", 2U);
    expect(s->tab == 0U && !launch_calls && install_calls == 2 && remove_calls == 1,
           "returning local with a verified update opens cancellable management instead of launching");
    s->verified = 0;
    dispatch(s, config, writer, "\rq", 1U);
    expect(!launch_calls && s->selected[0] == 7U, "offline cached update still prompts for the original scrolled app");
    strcpy(local.items[7].version, "1.2.0"); reload_local(s);
    dispatch(s, config, writer, "\r", 1U);
    expect(launch_calls == 1 && !strcmp(launched_id, "app7"),
           "an up-to-date local app launches directly even offline and after scrolling");
}

static void test_local_update_prompt(struct gui_state *s, const struct c1pkg_config *config, int writer)
{
    enum gui_action actions[] = {A_LAUNCH, A_UPDATE, A_REMOVE, A_CANCEL};
    reset_gui(s); install_fixture(s, 4U);
    expect(gui_update_available(&s->downloads.items[s->visible[0]]),
           "the displayed update status routes local confirmation to management");
    queue(writer, "\r"); gui_list_key(s, config, G_ENTER, 0U);
    expect(!launch_calls && !install_calls, "local Enter shows management with safe Cancel selected");
    queue(writer, "\033[D\033[D\r"); gui_list_key(s, config, G_ENTER, 0U);
    expect(install_calls == 1 && !strcmp(installed_id, "app0"),
           "local confirmation followed by explicit Update uses verified transaction");
    for (int mode = 0; mode < 4; ++mode) {
        reset_gui(s); install_fixture(s, 4U);
        if (mode == 0) s->verified = 0; /* Signed browse data, no online result. */
        if (mode == 1) { refresh_delay = 800U; refresh_gui(s, config); }
        if (mode == 2) { refresh_fails = 1; refresh_gui(s, config); finish_refresh(s); }
        if (mode == 3) {
            /* Exercise the initial cache publication, not just a toggled flag.
             * The signature API is mocked, the worker and pipe are production. */
            memset(&s->index, 0, sizeof(s->index)); s->verified = 0; reload_local(s);
            refresh_delay = 800U; refresh_fails = 1; refresh_gui(s, config);
            uint64_t deadline = c1pkg_input_now_ms() + 3000U;
            while (!s->index.sequence && s->refresh.pid > 0 && c1pkg_input_now_ms() < deadline) {
                gui_refresh_tick(s);
                struct pollfd event = {s->refresh.result_fd, POLLIN, 0}; (void)poll(&event, 1U, 5);
            }
            expect(s->index.sequence == fixture.sequence && !s->verified && s->refresh.pid > 0,
                   "initial re-verified cache is visible before the delayed online result");
        }
        expect(gui_update_available(&s->downloads.items[s->visible[0]]),
               "offline cache, pending refresh and failed refresh all retain the displayed update prompt");
        for (int language = GUI_ZH; language <= GUI_EN; ++language) {
            s->language = (enum gui_language)language;
            draw_count = 0; render_detail(s, &s->downloads.items[s->visible[0]], actions, 4U, 3U, 0);
            unsigned int prompts = 0U;
            for (size_t i = 0U; i < draw_count; ++i)
                if (drawings[i].y == 91 && !strcmp(drawings[i].text,
                    GUI_LABEL(s, "有可用更新，是否更新？", "Update available; update?"))) ++prompts;
            expect(prompts == 1U, "local management asks whether to update in both languages regardless of online freshness");
        }
        dispatch(s, config, writer, "\r\r", 1U);
        expect(!launch_calls && !install_calls && !remove_calls,
               "real Enter bytes open management with Cancel selected, never silently launch a cached update");
        queue(writer, "\033[D\033[D\r"); gui_list_key(s, config, G_ENTER, 0U);
        expect(!install_calls && !launch_calls && s->refresh.pid > 0,
               "choosing a cached or refreshing Update requests verification, not installation");
        finish_refresh(s);
        expect(!install_calls && !launch_calls && !remove_calls,
               "successful or failed verification cannot execute a deferred update");
        if (mode < 2) {
            expect(s->verified, "online success restores the original installation gate");
            queue(writer, "\033[D\033[D\r"); gui_list_key(s, config, G_ENTER, 0U);
            expect(install_calls == 1 && !strcmp(installed_id, "app0"),
                   "only a new explicit confirmation after verification reaches installation");
        } else expect(!s->verified, "failed refresh never authorizes installation");
        queue(writer, "\033[D\033[D\033[D\r"); gui_list_key(s, config, G_ENTER, 0U);
        expect(launch_calls == 1 && !strcmp(launched_id, "app0"),
               "an explicit Open choice remains available even with an offline update prompt");
    }
    /* A stale detail/list snapshot can still say update, but its prompt never
     * bypasses the existing package identity/version and storage checks. */
    for (int mode = 0; mode < 4; ++mode) {
        reset_gui(s); install_fixture(s, 4U);
        if (mode == 0) strcpy(s->index.packages[0].version, "9.9.9");
        if (mode == 1) strcpy(s->index.packages[0].name, "Changed app");
        if (mode == 2) s->index.count = 0U;
        if (mode == 3) s->storage_ready = 0;
        queue(writer, "\033[D\033[D\r"); gui_list_key(s, config, G_ENTER, 0U);
        expect(!install_calls && !launch_calls && s->status[0],
               "update prompt cannot authorize changed, removed or storage-unavailable packages");
    }
    static const enum c1pkg_download_status no_updates[] = {C1PKG_DOWNLOAD_CURRENT,
        C1PKG_DOWNLOAD_VERSION_UNKNOWN, C1PKG_DOWNLOAD_INSTALLED_NEWER, C1PKG_DOWNLOAD_REMOVED};
    for (size_t i = 0; i < sizeof(no_updates) / sizeof(*no_updates); ++i) {
        reset_gui(s); install_fixture(s, 4U);
        s->verified = 0;
        s->downloads.items[s->visible[0]].status = no_updates[i];
        expect(!gui_update_available(&s->downloads.items[s->visible[0]]),
               "current, unordered, newer-local and removed states do not display an update prompt");
        draw_count = 0; render_detail(s, &s->downloads.items[s->visible[0]], actions, 4U, 3U, 0);
        for (size_t j = 0U; j < draw_count; ++j)
            expect(strcmp(drawings[j].text, "有可用更新，是否更新？") != 0, "non-update details never claim an update");
        gui_list_key(s, config, G_ENTER, 0U);
        expect(launch_calls == 1 && !strcmp(launched_id, "app0") && !install_calls,
               "local confirmation directly launches when no update is displayed, including offline");
    }
}

static void test_metrics_retry_schedule(struct gui_state *s)
{
    const struct c1pkg_config invalid = {NULL, NULL};
    enum gui_action actions[] = {A_LAUNCH, A_CANCEL};
    for (unsigned int tab = 0U; tab < 2U; ++tab) {
        reset_gui(s); install_fixture(s, 4U); s->tab = tab; select_visible(s);
        gui_metrics_tick(s, &invalid, 1000U);
        expect(s->metrics_job == NULL, "invalid repository creates no worker or network request");
        expect(s->metrics_at == 31000U, "both tabs recheck pending reports after 30 seconds, not 15 minutes");
        gui_metrics_tick(s, &invalid, 30999U);
        expect(s->metrics_at == 31000U, "GUI does not continuously spawn before retry time");
        gui_metrics_tick(s, &invalid, 31000U);
        expect(s->metrics_at == 61000U, "pending reports get a bounded automatic retry opportunity on either tab");
        s->metrics_at = 0U; /* Same reset performed after successful installation. */
        gui_metrics_tick(s, &invalid, 32000U);
        expect(s->metrics_at == 62000U, "install triggers an immediate worker opportunity while worker enforces durable limits");
        draw_count = 0; render_gui(s);
        draw_count = 0; render_detail(s, &s->downloads.items[0], actions, 2U, 1U, 0);
        expect(s->metrics_at == 62000U && s->metrics_job == NULL,
               "showing or hiding list/detail statistics never changes report scheduling");
        s->tab = 1U - tab; select_visible(s);
        gui_metrics_tick(s, &invalid, 61999U);
        expect(s->metrics_at == 62000U, "switching tabs preserves the pending report deadline");
        gui_metrics_tick(s, &invalid, 62000U);
        expect(s->metrics_at == 92000U, "reports remain eligible after switching to either tab");
    }
}

static void test_metrics_visibility(struct gui_state *s)
{
    reset_gui(s); install_fixture(s, 4U);
    enum gui_action actions[] = {A_LAUNCH, A_CANCEL};
    enum gui_action removal[] = {A_REMOVE, A_CANCEL};
    for (int language = GUI_ZH; language <= GUI_EN; ++language) {
        s->language = (enum gui_language)language;
        for (s->tab = 0U; s->tab < 2U; ++s->tab) {
            select_visible(s);
            for (int sample = 0; sample < 5; ++sample) {
                /* Unavailable snapshot, missing app, explicit zero, seven and
                 * the full uint64 range are different presentation cases. */
                uint64_t count = sample == 4 ? UINT64_MAX : sample == 3 ? 7U : 0U;
                char row_label[32], detail_label[160], fitted[32], actual[160];
                memset(&s->metrics, 0, sizeof(s->metrics));
                s->metrics.available = sample != 0;
                s->metrics.count = 1U;
                strcpy(s->metrics.items[0].id, sample == 1 ? "app1" : "app0");
                s->metrics.items[0].installations = count;
                if (sample < 2) {
                    strcpy(row_label, "--");
                    snprintf(detail_label, sizeof(detail_label), "%s",
                             GUI_LABEL(s, "安装次数：暂无统计", "Installs: unavailable"));
                } else {
                    snprintf(row_label, sizeof(row_label), "%llu%s", (unsigned long long)count,
                             GUI_LABEL(s, "次", ""));
                    snprintf(detail_label, sizeof(detail_label), "%s %llu%s",
                             GUI_LABEL(s, "安装次数", "Installs:"), (unsigned long long)count,
                             GUI_LABEL(s, "次", ""));
                }
                gui_metrics_row_label(s, "app0", actual, sizeof(actual));
                expect(!strcmp(actual, row_label), "row labels distinguish unavailable, zero and known installation counts");
                gui_metrics_label(s, "app0", actual, sizeof(actual));
                expect(!strcmp(actual, detail_label), "detail labels consistently use installation-count wording");
                gui_fit_text(fitted, sizeof(fitted), row_label, 42U);
                draw_count = 0; render_gui(s); expect_no_text_overlap();
                unsigned int headings = 0U, rows = 0U, first_rows = 0U, names = 0U, states = 0U;
                for (size_t i = 0U; i < draw_count; ++i) {
                    const struct gui_draw *d = &drawings[i];
                    expect(!strstr(d->text, "台") && !strstr(d->text, "设备") && !strstr(d->text, "Devices"),
                           "list statistics never use legacy device labels or units");
                    if (!s->tab)
                        expect(!strstr(d->text, "次数") && !strstr(d->text, "Inst"),
                               "local lists never expose installation statistics, including column headings");
                    if (d->x == 244) {
                        if (d->y == 18) {
                            expect(!strcmp(d->text, GUI_LABEL(s, "次数", "Inst")),
                                   "store heading uses the compact installation-count label");
                            ++headings;
                        } else {
                            ++rows;
                            if (d->y == (int)gui_list_y(s) + 1) {
                                expect(!strcmp(d->text, fitted), "store first row renders the actual count or unknown marker");
                                ++first_rows;
                            }
                        }
                    }
                    if (d->y == (int)gui_list_y(s) + 1 && d->x == 10) {
                        expect(d->width == (s->tab ? 150 : 200), "local names gain 50 pixels; store name width is unchanged");
                        ++names;
                    }
                    if (d->y == (int)gui_list_y(s) + 1 && d->x == (s->tab ? 164 : 214)) {
                        expect(d->width == 72, "application state keeps its original width on both tabs");
                        ++states;
                    }
                }
                expect(headings == s->tab && first_rows == s->tab && rows == (s->tab ? 4U : 0U),
                       "only the store renders an installation heading and one statistic per application");
                expect(names == 1U && states == 1U, "local and store rows retain their name and state columns");
                draw_count = 0; render_detail(s, &s->downloads.items[0], actions, 2U, 1U, 0);
                expect_no_text_overlap();
                unsigned int details = 0U;
                for (size_t i = 0U; i < draw_count; ++i) {
                    const struct gui_draw *d = &drawings[i];
                    expect(!strstr(d->text, "台") && !strstr(d->text, "设备") && !strstr(d->text, "Devices"),
                           "detail statistics never use legacy device labels or units");
                    if (!s->tab)
                        expect(!strstr(d->text, "次数") && !strstr(d->text, "Installs"),
                               "local management contains no installation statistics");
                    if (d->y == 91) {
                        if (s->tab)
                            expect(d->x == 6 && d->width == 284 && !strcmp(d->text, detail_label),
                                   "store details show exact wording and full counts, including all 64-bit digits");
                        else
                            expect(!strcmp(d->text, GUI_LABEL(s, "有可用更新，是否更新？",
                                                              "Update available; update?")),
                                   "local details show the update prompt, not store statistics");
                        ++details;
                    }
                }
                expect(details == 1U, "both local update prompt and store statistics occupy their own detail row");
                draw_count = 0; render_detail(s, &s->downloads.items[0], removal, 2U, 1U, 1);
                expect_no_text_overlap();
                unsigned int warnings = 0U;
                for (size_t i = 0U; i < draw_count; ++i) {
                    const struct gui_draw *d = &drawings[i];
                    expect(!strstr(d->text, "次数") && !strstr(d->text, "Installs"),
                           "removal confirmation never substitutes statistics for its warning");
                    if (d->y == 91) {
                        expect(!strcmp(d->text, GUI_LABEL(s, "删除该应用全部已装版本", "Removes all installed versions")),
                               "removal warning survives statistics visibility changes on both tabs");
                        ++warnings;
                    }
                }
                expect(warnings == 1U, "both tabs retain exactly one removal warning");
            }
        }
    }
    s->tab = 0U;
    memset(&s->metrics, 0, sizeof(s->metrics));
}

#ifdef C1_TEST_PHYSICAL_KEYS
#include <linux/input.h>
int test_physical_key(int master, unsigned short code, int application_cursor);
int test_physical_shift(int master, unsigned short chord_code, int repeated);
static void test_physical_navigation(struct gui_state *s, const struct c1pkg_config *config)
{
    int saved = dup(STDIN_FILENO);
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    expect(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0, "create real bridge PTY");
    if (master < 0) return;
    int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    struct termios raw;
    expect(slave >= 0 && tcgetattr(slave, &raw) == 0, "open bridge slave");
    cfmakeraw(&raw);
    expect(tcsetattr(slave, TCSANOW, &raw) == 0 && dup2(slave, STDIN_FILENO) >= 0, "GUI receives raw PTY input");
    close(slave);
    for (int application = 0; application < 2; ++application) {
        reset_gui(s); install_fixture(s, 6U); refresh_delay = 2000U;
        refresh_gui(s, config);
        static const unsigned short codes[] = {KEY_DOWN, KEY_DOWN, KEY_UP, KEY_RIGHT, KEY_DOWN, KEY_LEFT,
                                                KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_TAB};
        static const int keys[] = {G_DOWN, G_DOWN, G_UP, G_RIGHT, G_DOWN, G_LEFT,
                                   G_PAGE_DOWN, G_PAGE_UP, G_SEARCH};
        uint64_t started = c1pkg_input_now_ms();
        for (size_t i = 0; i < sizeof(codes) / sizeof(*codes); ++i) {
            expect(test_physical_key(master, codes[i], application) == 0, "runtime writes physical key through libtsm");
            int key = gui_key(100);
            expect(key == keys[i], "production GUI decodes real physical key PTY sequence");
            (void)gui_ime_input(s, config, key, c1pkg_input_now_ms());
        }
        expect(s->tab == 0 && s->selected[0] == 1 && s->selected[1] == 1 && s->search_editing,
               "physical directions and legacy search bytes work while refreshing in CSI and SS3 modes");
        s->search_editing = 0;
        expect(test_physical_shift(master, 0U, 0) == 0, "standalone physical Shift release crosses the real PTY");
        queue(master, "q");
        int shifted = gui_key(100);
        expect(shifted == G_MANAGE, "GUI decodes the production Shift tap sequence as management");
        (void)gui_ime_input(s, config, shifted, c1pkg_input_now_ms());
        expect(!launch_calls && !install_calls && !remove_calls && !s->search_editing,
               "physical Shift opens and cancels management without any implicit action");
        expect(test_physical_shift(master, 0U, 1) == 0 && gui_key(0) == G_NONE,
               "physical Shift repeat never emits a management tap");
        expect(test_physical_shift(master, KEY_SPACE, 0) == 0 && gui_key(100) == G_IME_TOGGLE && gui_key(0) == G_NONE,
               "Shift Space emits only the IME chord, no management key on release");
        expect(test_physical_shift(master, KEY_Q, 0) == 0 && gui_key(100) == '1' && gui_key(0) == G_NONE,
               "Shift numeric keycap still selects candidate 1, with no extra management event");
        expect(test_physical_shift(master, KEY_T, 0) == 0 && gui_key(100) == '5' && gui_key(0) == G_NONE,
               "Shift numeric keycap still selects candidate 5, with no extra management event");
        expect(s->refresh.pid > 0 && c1pkg_input_now_ms() - started < 500U,
               "physical navigation never waits for delayed refresh");
        gui_release(s);
    }
    expect(dup2(saved, STDIN_FILENO) >= 0, "restore original test input");
    close(saved); close(master);
}
#endif

int main(int argc, char **argv)
{
    int input[2], zero = open("/dev/zero", O_RDWR);
    struct gui_state *s = calloc(1U, sizeof(*s));
    struct c1pkg_config config = {"http://example.invalid", "/unused"};
    if (!s || pipe(input) || dup2(input[0], STDIN_FILENO) < 0) return 2;
    close(input[0]); initialize_gui(s);
    worker_counts = mmap(NULL, sizeof(*worker_counts), PROT_READ | PROT_WRITE, MAP_SHARED, zero, 0);
    close(zero); if (worker_counts == MAP_FAILED) return 2;
    test_metrics_visibility(s);
    test_metrics_retry_schedule(s);
    test_catalog_titles(s);
    test_toolbar(s);
    test_layout(s, argc > 1 ? argv[1] : NULL);
    test_long_text_layout(s);
    test_footer_status(s);
    test_search(s, &config, input[1]);
    test_letter_jump_and_hidden_service(s, &config);
    test_async_navigation(s, &config, input[1]);
    test_workflow(s, &config, input[1]);
    test_local_update_prompt(s, &config, input[1]);
    test_management_after_resize(s, &config, input[1]);
#ifdef C1_TEST_PHYSICAL_KEYS
    test_physical_navigation(s, &config);
#endif
    expect(worker_counts->peak <= 1U && worker_counts->active == 0U, "refresh workers never overlap or survive GUI cleanup");
    gui_release(s); free(s); close(input[1]); munmap(worker_counts, sizeof(*worker_counts)); worker_counts = NULL;
    if (failures) return 1;
    puts("PASS: bilingual GUI layout, persistent filtering, real delayed navigation, verified install, cancellation/fd/reaping");
    return 0;
}
