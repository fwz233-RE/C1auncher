#include "pkg.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define TUI_WIDTH 49
#define TUI_HEIGHT 19
#define TUI_ROWS 12U

_Static_assert(TUI_HEIGHT == TUI_ROWS + 7U, "TUI layout must fill the 19-row terminal exactly");

static struct termios saved_terminal;
static int terminal_saved;

static void terminal_write(const char *data, size_t size)
{
    size_t offset = 0U;
    while (offset < size) {
        ssize_t amount = write(STDOUT_FILENO, data + offset, size - offset);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            break;
        }
        offset += (size_t)amount;
    }
}

static void terminal_restore(void)
{
    if (terminal_saved != 0) {
        static const char sequence[] = "\033[?25h\033[0m\n";
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_terminal);
        terminal_write(sequence, sizeof(sequence) - 1U);
        terminal_saved = 0;
    }
}

static int terminal_raw(void)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        tcgetattr(STDIN_FILENO, &saved_terminal) != 0) {
        return -1;
    }
    terminal_saved = 1;
    raw = saved_terminal;
    raw.c_iflag &= (tcflag_t)~(ICRNL | IXON);
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        terminal_saved = 0;
        return -1;
    }
    {
        static const char sequence[] = "\033[?25l\033[2J";
        terminal_write(sequence, sizeof(sequence) - 1U);
    }
    return 0;
}

static void frame_line(const char *format, ...)
{
    char content[256];
    char output[64];
    va_list arguments;
    int length;
    size_t used;

    va_start(arguments, format);
    length = vsnprintf(content, sizeof(content), format, arguments);
    va_end(arguments);
    used = length > 0 ? (size_t)length : 0U;
    if (used > 47U) {
        used = 47U;
    }
    (void)memset(output, ' ', sizeof(output));
    output[0] = '|';
    if (used > 0U) {
        (void)memcpy(output + 1, content, used);
    }
    output[48] = '|';
    output[49] = '\n';
    output[50] = '\0';
    (void)fputs(output, stdout);
}

static void border(void)
{
    (void)fputs("+-----------------------------------------------+\n", stdout);
}

static void final_border(void)
{
    /* A newline after row 19 would scroll the terminal, hide row 1, and leave row 19 blank. */
    (void)fputs("+-----------------------------------------------+", stdout);
}

struct tui_state {
    struct c1pkg_index index;
    struct c1pkg_installed_list installed;
    size_t selected[2];
    size_t offset[2];
    unsigned int tab;
    char status[C1PKG_ERROR_MAX];
};

static size_t item_count(const struct tui_state *state)
{
    return state->tab == 0U ? state->installed.count : state->index.count;
}

static void clamp_selection(struct tui_state *state)
{
    size_t count = item_count(state);
    size_t *selected = &state->selected[state->tab];
    size_t *offset = &state->offset[state->tab];

    if (count == 0U) {
        *selected = 0U;
        *offset = 0U;
    } else {
        if (*selected >= count) {
            *selected = count - 1U;
        }
        if (*selected < *offset) {
            *offset = *selected;
        } else if (*selected >= *offset + TUI_ROWS) {
            *offset = *selected - TUI_ROWS + 1U;
        }
    }
}

static void render(const struct tui_state *state)
{
    size_t row;
    size_t count = item_count(state);
    size_t offset = state->offset[state->tab];

    (void)fputs("\033[H", stdout);
    border();
    frame_line(" c1pkg       %s    %s ", state->tab == 0U ? "[MY APPS]" : " MY APPS ",
               state->tab == 1U ? "[DOWNLOAD]" : " DOWNLOAD ");
    border();
    for (row = 0U; row < TUI_ROWS; ++row) {
        size_t item = offset + row;
        if (item >= count) {
            frame_line("");
        } else if (state->tab == 0U) {
            const struct c1pkg_installed *installed = &state->installed.items[item];
            frame_line("%c %-31.31s %11.11s", item == state->selected[0] ? '>' : ' ',
                       installed->id, installed->version);
        } else {
            const struct c1pkg_package *package = &state->index.packages[item];
            const char *installed_version = NULL;
            const char *mark = c1pkg_store_is_installed(&state->installed, package->id,
                                                        &installed_version) ? "*" : " ";
            frame_line("%c%s %-28.28s %12.12s", item == state->selected[1] ? '>' : ' ',
                       mark, package->name, package->version);
        }
    }
    border();
    frame_line(" r:refresh q:quit");
    frame_line(" %.46s", state->status);
    final_border();
    (void)fflush(stdout);
}

static int reload_installed(struct tui_state *state)
{
    char error[C1PKG_ERROR_MAX] = "";
    if (c1pkg_store_list(&state->installed, error, sizeof(error)) != 0) {
        (void)snprintf(state->status, sizeof(state->status), "ERROR: %.220s", error);
        return -1;
    }
    clamp_selection(state);
    return 0;
}

static void refresh(struct tui_state *state, const struct c1pkg_config *config)
{
    char error[C1PKG_ERROR_MAX] = "";
    struct c1pkg_index fresh;

    (void)snprintf(state->status, sizeof(state->status), "Refreshing signed repository...");
    render(state);
    if (c1pkg_repo_refresh(config, &fresh, error, sizeof(error)) == 0) {
        state->index = fresh;
        (void)snprintf(state->status, sizeof(state->status), "Verified %lu package(s)",
                       (unsigned long)state->index.count);
    } else if (state->index.count == 0U &&
               c1pkg_repo_load_cached(config, &fresh, error, sizeof(error)) == 0) {
        state->index = fresh;
        (void)snprintf(state->status, sizeof(state->status),
                       "Offline: using previously verified cache");
    } else {
        (void)snprintf(state->status, sizeof(state->status), "REFRESH FAILED: %.210s", error);
    }
    (void)reload_installed(state);
    clamp_selection(state);
}

static void check_download_selected(struct tui_state *state,
                                    const struct c1pkg_config *config,
                                    const char *id)
{
    char error[C1PKG_ERROR_MAX] = "";
    struct c1pkg_index fresh;
    const struct c1pkg_package *package;
    const char *installed_version = NULL;
    int installed;

    (void)snprintf(state->status, sizeof(state->status),
                   "Checking signed repository for %.32s...", id);
    render(state);
    if (c1pkg_repo_refresh(config, &fresh, error, sizeof(error)) != 0) {
        (void)snprintf(state->status, sizeof(state->status),
                       "CHECK FAILED: %.217s", error);
        return;
    }
    state->index = fresh;
    package = c1pkg_repo_find(&state->index, id);
    if (package == NULL) {
        (void)snprintf(state->status, sizeof(state->status),
                       "%.32s is no longer available", id);
        clamp_selection(state);
        return;
    }
    state->selected[1] = (size_t)(package - state->index.packages);
    clamp_selection(state);
    if (reload_installed(state) != 0) {
        return;
    }
    installed = c1pkg_store_is_installed(&state->installed, id, &installed_version);
    if (installed && strcmp(installed_version, package->version) == 0) {
        (void)snprintf(state->status, sizeof(state->status),
                       "%.32s %.48s is already the latest version",
                       id, package->version);
        return;
    }

    (void)snprintf(state->status, sizeof(state->status),
                   "%s and verifying %.32s...", installed ? "Updating" : "Installing", id);
    render(state);
    if (c1pkg_store_install(config, package, error, sizeof(error)) != 0) {
        (void)snprintf(state->status, sizeof(state->status), "%s FAILED: %.216s",
                       installed ? "UPDATE" : "INSTALL", error);
    } else {
        (void)snprintf(state->status, sizeof(state->status), "%s %.32s %.48s",
                       installed ? "Updated" : "Installed", id, package->version);
        (void)reload_installed(state);
    }
}

static void launch_selected_app(const char *id)
{
    static const char clear_sequence[] = "\033[2J\033[H";

    /* Leave the manager before handing the terminal to the standalone launcher. */
    terminal_restore();
    terminal_write(clear_sequence, sizeof(clear_sequence) - 1U);
    (void)printf("$ /usr/data/c1/bin/c1pkg launch %s\n", id);
    (void)fflush(stdout);
    execl("/usr/data/c1/bin/c1pkg", "c1pkg", "launch", id, (char *)NULL);
    (void)fprintf(stderr, "c1pkg: cannot start standalone launcher: %s\n", strerror(errno));
    _exit(127);
}

static int perform_action(struct tui_state *state, const struct c1pkg_config *config)
{
    size_t count = item_count(state);

    if (count == 0U) {
        (void)snprintf(state->status, sizeof(state->status), "No item selected");
        return 0;
    }
    if (state->tab == 0U) {
        char id[C1PKG_ID_MAX + 1U];
        (void)strcpy(id, state->installed.items[state->selected[0]].id);
        launch_selected_app(id);
    } else {
        char id[C1PKG_ID_MAX + 1U];
        (void)strcpy(id, state->index.packages[state->selected[1]].id);
        check_download_selected(state, config, id);
    }
    return 0;
}

enum input_key {
    KEY_NONE,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ENTER,
    KEY_PREVIOUS_LIST,
    KEY_NEXT_LIST,
    KEY_REFRESH,
    KEY_QUIT
};

static enum input_key read_key(void)
{
    unsigned char bytes[8];
    struct pollfd input;
    ssize_t amount;

    input.fd = STDIN_FILENO;
    input.events = POLLIN;
    input.revents = 0;
    if (poll(&input, 1U, -1) <= 0) {
        return errno == EINTR ? KEY_NONE : KEY_QUIT;
    }
    amount = read(STDIN_FILENO, bytes, sizeof(bytes));
    if (amount <= 0) {
        return KEY_NONE;
    }
    if (bytes[0] == 27U &&
        (amount < 3 ||
         (amount == 3 && bytes[1] == (unsigned char)'[' &&
          (bytes[2] == (unsigned char)'5' || bytes[2] == (unsigned char)'6')))) {
        struct pollfd continuation;
        continuation.fd = STDIN_FILENO;
        continuation.events = POLLIN;
        continuation.revents = 0;
        if (poll(&continuation, 1U, 40) > 0) {
            ssize_t more = read(STDIN_FILENO, bytes + amount, sizeof(bytes) - (size_t)amount);
            if (more > 0) {
                amount += more;
            }
        }
    }
    if (bytes[0] == 27U) {
        if (amount >= 4 && bytes[1] == (unsigned char)'[' && bytes[3] == (unsigned char)'~') {
            if (bytes[2] == (unsigned char)'5') return KEY_PREVIOUS_LIST;
            if (bytes[2] == (unsigned char)'6') return KEY_NEXT_LIST;
        }
        if (amount >= 3 && bytes[1] == (unsigned char)'[') {
            if (bytes[2] == (unsigned char)'A') return KEY_UP;
            if (bytes[2] == (unsigned char)'B') return KEY_DOWN;
            if (bytes[2] == (unsigned char)'C') return KEY_RIGHT;
            if (bytes[2] == (unsigned char)'D') return KEY_LEFT;
        }
        return KEY_QUIT;
    }
    if (bytes[0] == (unsigned char)'q') return KEY_QUIT;
    if (bytes[0] == (unsigned char)'r') return KEY_REFRESH;
    if (bytes[0] == (unsigned char)'\r' || bytes[0] == (unsigned char)'\n') return KEY_ENTER;
    return KEY_NONE;
}

int c1pkg_tui(const struct c1pkg_config *config)
{
    struct tui_state state;
    int done = 0;

    (void)memset(&state, 0, sizeof(state));
    (void)snprintf(state.status, sizeof(state.status), "Starting");
    if (terminal_raw() != 0) {
        (void)fprintf(stderr, "c1pkg: TUI requires an ANSI terminal\n");
        return 1;
    }
    (void)atexit(terminal_restore);
    (void)reload_installed(&state);
    refresh(&state, config);
    while (done == 0) {
        enum input_key key;
        size_t count;
        render(&state);
        key = read_key();
        count = item_count(&state);
        if (key == KEY_QUIT) {
            done = 1;
        } else if (key == KEY_PREVIOUS_LIST || key == KEY_NEXT_LIST) {
            state.tab = key == KEY_PREVIOUS_LIST ? 0U : 1U;
            clamp_selection(&state);
        } else if (key == KEY_LEFT || key == KEY_RIGHT) {
            state.tab = key == KEY_LEFT ? 0U : 1U;
            clamp_selection(&state);
        } else if (key == KEY_UP && count > 0U) {
            if (state.selected[state.tab] > 0U) {
                --state.selected[state.tab];
            }
            clamp_selection(&state);
        } else if (key == KEY_DOWN && count > 0U) {
            if (state.selected[state.tab] + 1U < count) {
                ++state.selected[state.tab];
            }
            clamp_selection(&state);
        } else if (key == KEY_REFRESH) {
            refresh(&state, config);
        } else if (key == KEY_ENTER && perform_action(&state, config) != 0) {
            done = 1;
        }
    }
    terminal_restore();
    return 0;
}