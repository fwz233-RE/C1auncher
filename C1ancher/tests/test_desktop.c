#define _DEFAULT_SOURCE 1
#include "ui/preferences.h"
#include "ui/model.h"
#include "services/terminal.h"
#include "ui/terminal_screen.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_text(const char *path, const char *data)
{
    FILE *f = fopen(path, "w"); assert(f);
    assert(fputs(data, f) >= 0); assert(fclose(f) == 0);
}

static void test_preferences_validation(const char *path)
{
    c1_preferences original = c1_preferences_default(), invalid, loaded;
    assert(c1_preferences_save(&original, path));
    assert(!c1_preferences_valid_text(NULL));
    assert(!c1_preferences_valid_text(""));
    assert(!c1_preferences_valid_text("\xe4"));
    assert(!c1_preferences_valid_text("\xe4\xbd"));
    assert(!c1_preferences_valid_text("\xf0\x9f\x99"));
    assert(!c1_preferences_valid_text("\x80"));
    invalid = original; invalid.language = (c1_language)-1;
    assert(!c1_preferences_save(&invalid, path));
    invalid = original; invalid.language = (c1_language)(C1_LANGUAGE_EN + 1);
    assert(!c1_preferences_save(&invalid, path));
    invalid = original; invalid.lock_style = (c1_lock_style)-1;
    assert(!c1_preferences_save(&invalid, path));
    invalid = original; invalid.lock_style = (c1_lock_style)(C1_LOCK_CALENDAR + 1);
    assert(!c1_preferences_save(&invalid, path));
    invalid = original; memset(invalid.lock_text, 'x', sizeof(invalid.lock_text));
    assert(!c1_preferences_valid_text(invalid.lock_text));
    assert(!c1_preferences_save(&invalid, path));
    invalid = original; invalid.lock_text[0] = '\0';
    assert(!c1_preferences_save(&invalid, path));
    invalid = original; strcpy(invalid.lock_text, "valid\xe4\xbd");
    assert(!c1_preferences_save(&invalid, path));
    char oversized_path[1024]; memset(oversized_path, 'x', sizeof(oversized_path));
    assert(!c1_preferences_save(&original, oversized_path));
    assert(!c1_preferences_save(&original, ""));
    assert(c1_preferences_load(&loaded, path));
    assert(loaded.language == original.language && loaded.lock_style == original.lock_style);
    assert(!strcmp(loaded.lock_text, original.lock_text));
    memset(invalid.lock_text, 'x', sizeof(invalid.lock_text) - 1U);
    invalid.lock_text[sizeof(invalid.lock_text) - 1U] = '\0';
    assert(c1_preferences_valid_text(invalid.lock_text));
    assert(c1_preferences_save(&invalid, path));
    assert(c1_preferences_load(&loaded, path));
    assert(!strcmp(loaded.lock_text, invalid.lock_text));
}

static void test_power_modes(const char *path)
{
    c1_preferences prefs = c1_preferences_default(), loaded;
    static const unsigned locks[] = {1U, 3U, 5U}, shutdowns[] = {2U, 5U, 0U};
    for (unsigned mode = 0U; mode <= C1_POWER_MODE_PERFORMANCE; ++mode) {
        c1_power_settings timing = c1_preferences_power_settings((c1_power_mode)mode);
        assert(timing.lock_minutes == locks[mode] && timing.shutdown_minutes == shutdowns[mode]);
        assert(timing.suspend_on_lock == (mode == C1_POWER_MODE_PERFORMANCE));
        prefs.power_mode = (c1_power_mode)mode;
        prefs.language = C1_LANGUAGE_EN; prefs.network_time = false;
        strcpy(prefs.lock_text, "保持原来的文字 Hello");
        assert(c1_preferences_save(&prefs, path));
        assert(c1_preferences_load(&loaded, path));
        assert(loaded.power_mode == prefs.power_mode && loaded.language == prefs.language);
        assert(!loaded.network_time && !strcmp(loaded.lock_text, prefs.lock_text));
        char contents[4097] = {0};
        FILE *file = fopen(path, "r"); assert(file);
        size_t bytes = fread(contents, 1U, sizeof(contents) - 1U, file);
        assert(bytes > 0U && !ferror(file) && fclose(file) == 0);
        assert(strstr(contents, "power_mode=") && strstr(contents, "idle_minutes="));
        assert(strstr(contents, mode == C1_POWER_MODE_PERFORMANCE ? "suspend_seconds=1\n" : "suspend_seconds=0\n"));
        c1_preferences_cycle(&prefs, C1_SETTING_POWER_MODE, 1);
        assert(prefs.power_mode == (c1_power_mode)((mode + 1U) % 3U));
        c1_preferences_cycle(&prefs, C1_SETTING_POWER_MODE, -1);
        assert(prefs.power_mode == (c1_power_mode)mode);
    }
    c1_preferences invalid = prefs;
    invalid.power_mode = (c1_power_mode)-1; assert(!c1_preferences_save(&invalid, path));
    invalid.power_mode = (c1_power_mode)3; assert(!c1_preferences_save(&invalid, path));
    assert(c1_preferences_load(&loaded, path) && loaded.power_mode == C1_POWER_MODE_PERFORMANCE);
    write_text(path, "language=en\nidle_minutes=0\nsuspend_seconds=20\nshutdown_minutes=120\n"
                     "utc_offset_minutes=-60\nnetwork_time=0\nlock_text=自定义文字\nterminal_enabled=0\nbackground_checks=0\n");
    assert(c1_preferences_load(&loaded, path));
    assert(loaded.power_mode == C1_POWER_MODE_STANDARD && loaded.language == C1_LANGUAGE_EN &&
           loaded.utc_offset_minutes == -60 && !loaded.network_time && !loaded.terminal_enabled &&
           !loaded.background_checks && !strcmp(loaded.lock_text, "自定义文字"));
    write_text(path, "power_mode=saving\nidle_minutes=1440\nshutdown_minutes=10080\nsuspend_seconds=86400\n");
    assert(c1_preferences_load(&loaded, path) && loaded.power_mode == C1_POWER_MODE_SAVING);
    write_text(path, "idle_minutes=0\nsuspend_seconds=0\nshutdown_minutes=0\npower_mode=performance\n");
    assert(c1_preferences_load(&loaded, path) && loaded.power_mode == C1_POWER_MODE_PERFORMANCE);
    write_text(path, "power_mode=custom\nlanguage=en\n");
    assert(!c1_preferences_load(&loaded, path) && loaded.power_mode == C1_POWER_MODE_STANDARD &&
           loaded.language == C1_LANGUAGE_ZH);
    c1_ui_state state = c1_ui_initial_state();
    state.page = C1_UI_PAGE_SETTINGS; state.selection = C1_SETTING_POWER_MODE;
    state = c1_ui_step(state, C1_UI_EVENT_LEFT, NULL).state;
    assert(state.preferences.power_mode == C1_POWER_MODE_SAVING);
    state = c1_ui_step(state, C1_UI_EVENT_ENTER, NULL).state;
    assert(state.preferences.power_mode == C1_POWER_MODE_STANDARD);
    state = c1_ui_step(state, C1_UI_EVENT_RIGHT, NULL).state;
    assert(state.preferences.power_mode == C1_POWER_MODE_PERFORMANCE);
    state.selection = C1_SETTING_LANGUAGE;
    for (unsigned i = 1U; i < C1_SETTING_COUNT; ++i) {
        state = c1_ui_step(state, C1_UI_EVENT_DOWN, NULL).state;
        assert(state.selection == i);
    }
    assert(state.selection == C1_SETTING_ABOUT);
    state = c1_ui_step(state, C1_UI_EVENT_DOWN, NULL).state;
    assert(state.selection == C1_SETTING_ABOUT);
    state = c1_ui_step(state, C1_UI_EVENT_SELECT_NEXT, NULL).state;
    assert(state.selection == C1_SETTING_LANGUAGE);
}

static c1_ui_state edit_lock_text(c1_ui_state state)
{
    state.page = C1_UI_PAGE_SETTINGS;
    state.selection = C1_SETTING_LOCK_TEXT;
    state = c1_ui_step(state, C1_UI_EVENT_ENTER, NULL).state;
    assert(state.page == C1_UI_PAGE_LOCK_TEXT);
    assert(!strcmp(state.lock_text_draft, state.preferences.lock_text));
    assert(state.lock_text_cursor == strlen(state.lock_text_draft));
    return state;
}

static void test_lock_text_draft(void)
{
    c1_ui_state state = c1_ui_initial_state();
    strcpy(state.preferences.lock_text, "你好");
    assert(!c1_ui_lock_text_append_utf8(&state, "世界"));
    assert(!c1_ui_lock_text_delete(&state));
    assert(!c1_ui_lock_text_append_utf8(NULL, "世界"));
    assert(!c1_ui_lock_text_delete(NULL));
    state = edit_lock_text(state);
    assert(c1_ui_lock_text_delete(&state));
    assert(!strcmp(state.lock_text_draft, "你"));
    assert(!strcmp(state.preferences.lock_text, "你好"));
    assert(c1_ui_lock_text_append_utf8(&state, "好世界"));
    assert(!strcmp(state.lock_text_draft, "你好世界"));
    assert(c1_ui_lock_text_append_utf8(&state, "\xf0\x9f\x99\x82"));
    assert(c1_ui_lock_text_delete(&state));
    assert(!strcmp(state.lock_text_draft, "你好世界"));
    assert(c1_ui_lock_text_append_ascii(&state, '!'));
    assert(c1_ui_lock_text_delete(&state));
    assert(!strcmp(state.lock_text_draft, "你好世界"));

    static const char *const rejected[] = {
        NULL, "", "\xe4", "\xe4\xbd", "\xf0\x9f\x99", "valid\xe4\xbd",
        "valid\x80", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80",
        "valid\n", "\xe2\x80\xae"
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(*rejected); ++i) {
        c1_ui_state before = state;
        assert(!c1_ui_lock_text_append_utf8(&state, rejected[i]));
        assert(!memcmp(state.lock_text_draft, before.lock_text_draft, sizeof(state.lock_text_draft)));
        assert(!memcmp(&state.preferences, &before.preferences, sizeof(state.preferences)));
    }
    assert(!c1_ui_lock_text_append_ascii(&state, '\n'));
    assert(!c1_ui_lock_text_append_ascii(&state, '\0'));
    assert(!c1_ui_lock_text_append_ascii(&state, (char)0x80));

    state = c1_ui_step(state, C1_UI_EVENT_BACK, NULL).state;
    assert(state.page == C1_UI_PAGE_SETTINGS && state.selection == C1_SETTING_LOCK_TEXT);
    assert(!state.lock_text_draft[0] && !strcmp(state.preferences.lock_text, "你好"));
    state = edit_lock_text(state);
    assert(c1_ui_lock_text_append_utf8(&state, "世界"));
    state = c1_ui_step(state, C1_UI_EVENT_HOME, NULL).state;
    assert(state.page == C1_UI_PAGE_DESKTOP && state.selection == 0U);
    assert(!state.lock_text_draft[0] && !strcmp(state.preferences.lock_text, "你好"));

    state = edit_lock_text(state);
    assert(c1_ui_lock_text_delete(&state) && c1_ui_lock_text_delete(&state));
    assert(!state.lock_text_draft[0] && !c1_ui_lock_text_delete(&state));
    state = c1_ui_step(state, C1_UI_EVENT_ENTER, NULL).state;
    assert(state.page == C1_UI_PAGE_LOCK_TEXT && state.wifi_notice[0]);
    assert(!strcmp(state.preferences.lock_text, "你好"));
    strcpy(state.lock_text_draft, "invalid\xe4\xbd");
    state = c1_ui_step(state, C1_UI_EVENT_ENTER, NULL).state;
    assert(state.page == C1_UI_PAGE_LOCK_TEXT && state.wifi_notice[0]);
    assert(!strcmp(state.preferences.lock_text, "你好"));
    assert(!c1_ui_lock_text_delete(&state));
    assert(!c1_ui_lock_text_append_utf8(&state, "test"));
    state.lock_text_draft[0] = '\0';
    assert(c1_ui_lock_text_append_utf8(&state, "中文 Hello"));
    assert(!state.wifi_notice[0]);
    c1_ui_transition saved = c1_ui_step(state, C1_UI_EVENT_ENTER, NULL);
    assert(saved.action == C1_UI_ACTION_NONE);
    state = saved.state;
    assert(state.page == C1_UI_PAGE_SETTINGS && state.selection == C1_SETTING_LOCK_TEXT);
    assert(!strcmp(state.preferences.lock_text, "中文 Hello") && !state.lock_text_draft[0]);

    state = edit_lock_text(state);
    memset(state.lock_text_draft, 'x', sizeof(state.lock_text_draft) - 4U);
    state.lock_text_draft[sizeof(state.lock_text_draft) - 4U] = '\0';
    c1_ui_state before_capacity = state;
    assert(!c1_ui_lock_text_append_utf8(&state, "你好"));
    assert(!memcmp(state.lock_text_draft, before_capacity.lock_text_draft, sizeof(state.lock_text_draft)));
    char unterminated[C1_LOCK_TEXT_BYTES]; memset(unterminated, 'x', sizeof(unterminated));
    assert(!c1_ui_lock_text_append_utf8(&state, unterminated));
    assert(!memcmp(state.lock_text_draft, before_capacity.lock_text_draft, sizeof(state.lock_text_draft)));
    assert(c1_ui_lock_text_append_utf8(&state, "你"));
    assert(strlen(state.lock_text_draft) == C1_LOCK_TEXT_BYTES - 1U);
    c1_ui_state before = state;
    assert(!c1_ui_lock_text_append_ascii(&state, '!'));
    assert(!c1_ui_lock_text_append_utf8(&state, "好"));
    assert(!memcmp(state.lock_text_draft, before.lock_text_draft, sizeof(state.lock_text_draft)));
    assert(c1_ui_lock_text_delete(&state));
    assert(strlen(state.lock_text_draft) == C1_LOCK_TEXT_BYTES - 4U);
    memset(state.lock_text_draft, 'x', sizeof(state.lock_text_draft));
    assert(!c1_ui_lock_text_append_utf8(&state, "好"));
    assert(!c1_ui_lock_text_delete(&state));
    state = c1_ui_step(state, C1_UI_EVENT_ENTER, NULL).state;
    assert(state.page == C1_UI_PAGE_LOCK_TEXT && state.wifi_notice[0]);
    assert(!strcmp(state.preferences.lock_text, "中文 Hello"));

    state = edit_lock_text(state);
    strcpy(state.lock_text_draft, "你");
    assert(c1_ui_lock_text_append_utf8(&state, state.lock_text_draft));
    assert(!strcmp(state.lock_text_draft, "你你"));
    state = c1_ui_step(state, C1_UI_EVENT_SETTINGS, NULL).state;
    assert(!state.lock_text_draft[0] && !strcmp(state.preferences.lock_text, "中文 Hello"));
}

static void test_lock_text_cursor(void)
{
    c1_ui_state state = c1_ui_initial_state();
    assert(!c1_ui_lock_text_move(&state, -1) && !c1_ui_lock_text_move(NULL, 1));
    strcpy(state.preferences.lock_text, "A你é🙂Z");
    state = edit_lock_text(state);
    static const size_t boundaries[] = {0U, 1U, 4U, 6U, 10U, 11U};
    assert(!c1_ui_lock_text_move(&state, 1));
    for (size_t i = 5U; i > 0U; --i) {
        state = c1_ui_step(state, C1_UI_EVENT_LEFT, NULL).state;
        assert(state.lock_text_cursor == boundaries[i - 1U]);
    }
    assert(!c1_ui_lock_text_move(&state, -1) && !c1_ui_lock_text_delete(&state));
    for (size_t i = 1U; i < 6U; ++i) {
        state = c1_ui_step(state, C1_UI_EVENT_RIGHT, NULL).state;
        assert(state.lock_text_cursor == boundaries[i]);
    }
    assert(c1_ui_lock_text_move(&state, -1) && c1_ui_lock_text_move(&state, -1));
    assert(state.lock_text_cursor == 6U);
    assert(c1_ui_lock_text_delete(&state));
    assert(!strcmp(state.lock_text_draft, "A你🙂Z") && state.lock_text_cursor == 4U);
    assert(c1_ui_lock_text_append_utf8(&state, "好中"));
    assert(!strcmp(state.lock_text_draft, "A你好中🙂Z") && state.lock_text_cursor == 10U);
    assert(c1_ui_lock_text_append_ascii(&state, '!'));
    assert(!strcmp(state.lock_text_draft, "A你好中!🙂Z") && state.lock_text_cursor == 11U);
    assert(!strcmp(state.preferences.lock_text, "A你é🙂Z"));
    state.lock_text_cursor = 2U; /* Defensive recovery from stale byte offsets. */
    assert(c1_ui_lock_text_cursor_offset(&state) == 1U);
    assert(c1_ui_lock_text_append_ascii(&state, 'x'));
    assert(!strcmp(state.lock_text_draft, "Ax你好中!🙂Z") && state.lock_text_cursor == 2U);
    state.lock_text_cursor = 0U;
    assert(c1_ui_lock_text_append_utf8(&state, state.lock_text_draft + 2U)); /* Aliasing suffix. */
    assert(!strcmp(state.lock_text_draft, "你好中!🙂ZAx你好中!🙂Z"));
    c1_ui_state before = state;
    assert(!c1_ui_lock_text_append_utf8(&state, "\xe4\xbd"));
    assert(!memcmp(&state, &before, sizeof(state)));
    state = c1_ui_step(state, C1_UI_EVENT_BACK, NULL).state;
    assert(state.lock_text_cursor == 0U && !state.lock_text_draft[0]);
    state = edit_lock_text(state);
    assert(c1_ui_lock_text_move(&state, -1));
    assert(c1_ui_lock_text_append_utf8(&state, "中"));
    state = c1_ui_step(state, C1_UI_EVENT_ENTER, NULL).state;
    assert(!strcmp(state.preferences.lock_text, "A你é🙂中Z"));
    assert(state.lock_text_cursor == 0U && !state.lock_text_draft[0]);
    state = edit_lock_text(state);
    state = c1_ui_step(state, C1_UI_EVENT_HOME, NULL).state;
    assert(state.lock_text_cursor == 0U && !state.lock_text_draft[0]);
}

static void assert_secret_cleared(const c1_ui_state *state)
{
    assert(state->secret_length == 0U && !state->secret_visible);
    for (size_t i = 0; i < sizeof(state->secret); ++i) assert(state->secret[i] == '\0');
}

static void test_freeze_password_redraw(void)
{
    for (unsigned int scenario = 0; scenario < 7U; ++scenario) {
        c1_ui_state state = c1_ui_initial_state();
        state.preferences.lock_style = C1_LOCK_FREEZE;
        state.page = scenario < 3U ? C1_UI_PAGE_WIFI_PASSWORD : C1_UI_PAGE_TERMINAL;
        if (scenario == 0U || scenario == 3U) {
            strcpy(state.secret, "sensitive"); state.secret_length = 9U;
        }
        if (scenario == 1U) state.secret_visible = true;
        if (scenario == 4U) state.secret[sizeof(state.secret) - 1U] = 'x';
        if (scenario == 5U) state.secret_length = 1U;
        if (scenario == 6U) state.secret_visible = true;
        assert(c1_ui_enter_lock(&state));
        assert(state.page == C1_UI_PAGE_LOCK && !state.frozen_lock);
        assert_secret_cleared(&state);
        assert(c1_ui_unlock(&state));
        assert(state.page == (scenario < 3U ? C1_UI_PAGE_WIFI : C1_UI_PAGE_TERMINAL));
    }
    c1_ui_state state = c1_ui_initial_state();
    state.page = C1_UI_PAGE_TERMINAL;
    state.preferences.lock_style = C1_LOCK_FREEZE;
    assert(c1_ui_enter_lock(&state) && state.frozen_lock);
    assert(c1_ui_unlock(&state) && !state.frozen_lock);
}

static void test_navigation(void)
{
    static const c1_ui_page targets[] = {C1_UI_PAGE_TERMINAL, C1_UI_PAGE_TERMINAL,
        C1_UI_PAGE_WIFI, C1_UI_PAGE_BATTERY, C1_UI_PAGE_SETTINGS};
    for (unsigned i = 0; i < 5; ++i) {
        c1_ui_state state = c1_ui_initial_state();
        for (unsigned j = 0; j < i; ++j) {
            c1_ui_transition move = c1_ui_step(state, C1_UI_EVENT_DOWN, NULL);
            assert(move.state.page == C1_UI_PAGE_DESKTOP && move.action == C1_UI_ACTION_NONE);
            state = move.state;
        }
        assert(state.selection == i);
        state = c1_ui_step(state, C1_UI_EVENT_ENTER, NULL).state;
        assert(state.page == targets[i]);
        state = c1_ui_step(state, C1_UI_EVENT_HOME, NULL).state;
        assert(state.page == C1_UI_PAGE_DESKTOP && state.selection == i);
        state = c1_ui_step(state, C1_UI_EVENT_DOWN, NULL).state;
        assert(state.selection == (i + 1U) % 5U);
    }
    static const c1_ui_event arrows[] = {C1_UI_EVENT_UP, C1_UI_EVENT_DOWN,
        C1_UI_EVENT_LEFT, C1_UI_EVENT_RIGHT};
    for (unsigned i = 0; i < 5; ++i) {
        for (unsigned key = 0; key < 4; ++key) {
            c1_ui_state state = c1_ui_initial_state();
            state.selection = state.desktop_selection = i;
            c1_ui_transition move = c1_ui_step(state, arrows[key], NULL);
            unsigned expected = (i + (key == 0 || key == 2 ? 4U : 1U)) % 5U;
            assert(move.state.selection == expected && move.state.desktop_selection == expected);
            assert(move.state.page == C1_UI_PAGE_DESKTOP && move.action == C1_UI_ACTION_NONE);
            move = c1_ui_step(move.state, C1_UI_EVENT_ENTER, NULL);
            assert(move.state.page == targets[expected]);
            move = c1_ui_step(move.state, C1_UI_EVENT_HOME, NULL);
            assert(move.state.page == C1_UI_PAGE_DESKTOP && move.state.selection == expected);
        }
    }
}

static void test_power_and_about(const char *path)
{
    c1_ui_state state = c1_ui_initial_state();
    c1_ui_status status = {0};
    state.page = C1_UI_PAGE_SETTINGS; state.selection = C1_SETTING_UPDATE;
    state = c1_ui_step(state, C1_UI_EVENT_DOWN, &status).state;
    assert(state.selection == C1_SETTING_ABOUT);
    state = c1_ui_step(state, C1_UI_EVENT_ENTER, &status).state;
    assert(state.page == C1_UI_PAGE_ABOUT);
    state = c1_ui_step(state, C1_UI_EVENT_BACK, &status).state;
    assert(state.page == C1_UI_PAGE_SETTINGS && state.selection == C1_SETTING_ABOUT);
    state.page = C1_UI_PAGE_BATTERY; state.selection = 0;
    state = c1_ui_step(state, C1_UI_EVENT_ENTER, &status).state;
    assert(state.page == C1_UI_PAGE_BATTERY && state.preferences.terminal_enabled);
    state = c1_ui_step(state, C1_UI_EVENT_DOWN, &status).state;
    assert(state.selection == 0U);
    assert(state.preferences.network_time && state.preferences.background_checks);
    state.page = C1_UI_PAGE_SETTINGS; state.selection = C1_SETTING_NETWORK_TIME;
    state = c1_ui_step(state, C1_UI_EVENT_ENTER, &status).state;
    assert(!state.preferences.network_time && state.preferences.background_checks);
    assert(c1_preferences_save(&state.preferences, path));
    c1_preferences loaded;
    assert(c1_preferences_load(&loaded, path));
    assert(loaded.terminal_enabled && !loaded.network_time && loaded.background_checks);
    write_text(path, "language=zh\n");
    assert(c1_preferences_load(&loaded, path) && loaded.terminal_enabled && loaded.background_checks);
    state.page = C1_UI_PAGE_SETTINGS; state.selection = C1_SETTING_UPDATE;
    status.update_available = true; status.wifi_connected = true; strcpy(status.wifi_ipv4, "1.2.3.4");
    assert(c1_ui_step(state, C1_UI_EVENT_ENTER, &status).action == C1_UI_ACTION_UPDATE_REFRESH);
    status.update_prepared = true; status.wifi_connected = false;
    assert(c1_ui_step(state, C1_UI_EVENT_ENTER, &status).action == C1_UI_ACTION_TERMINAL_UPDATE);
}

int main(void)
{
    char root[] = "/tmp/c1-desktop-test-XXXXXX", file[256], link[256];
    assert(mkdtemp(root));
    snprintf(file, sizeof(file), "%s/desktop.conf", root);
    snprintf(link, sizeof(link), "%s/link", root);
    c1_preferences p, loaded;
    assert(!c1_preferences_load(&p, file));
    assert(p.power_mode == C1_POWER_MODE_STANDARD && p.utc_offset_minutes == 480);
    assert(c1_preferences_valid_text("你好，世界 / Hello"));
    assert(!c1_preferences_valid_text("a\nb"));
    assert(!c1_preferences_valid_text("\xc0\xaf"));
    assert(!c1_preferences_valid_text("\xed\xa0\x80"));
    assert(!c1_preferences_valid_text("\xf4\x90\x80\x80"));
    assert(!c1_preferences_valid_text("a\xe2\x80\xae" "b"));
    strcpy(p.lock_text, "你好世界 Hello"); p.language = C1_LANGUAGE_EN;
    p.lock_style = C1_LOCK_CALENDAR;
    assert(c1_preferences_save(&p, file));
    assert(c1_preferences_load(&loaded, file));
    assert(loaded.language == p.language && loaded.lock_style == p.lock_style && !strcmp(loaded.lock_text, p.lock_text));
    struct stat st; assert(stat(file, &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(symlink(file, link) == 0);
    assert(!c1_preferences_load(&loaded, link));
    write_text(file, "idle_minutes=999999999999999999999\nlanguage=en\n");
    assert(!c1_preferences_load(&loaded, file));
    assert(loaded.power_mode == C1_POWER_MODE_STANDARD && loaded.language == C1_LANGUAGE_ZH);
    write_text(file, "utc_offset_minutes=17\n");
    assert(!c1_preferences_load(&loaded, file));
    c1_preferences_cycle(&p, C1_SETTING_POWER_MODE, 1); assert(p.power_mode == C1_POWER_MODE_PERFORMANCE);
    c1_preferences_cycle(&p, C1_SETTING_POWER_MODE, -1); assert(p.power_mode == C1_POWER_MODE_STANDARD);
    p.utc_offset_minutes = 840; c1_preferences_cycle(&p, C1_SETTING_TIME_ZONE, 1); assert(p.utc_offset_minutes == -720);

    c1_ui_state state = c1_ui_initial_state();
    state.page = C1_UI_PAGE_TERMINAL;
    state.preferences.lock_style = C1_LOCK_FREEZE;
    assert(c1_ui_enter_lock(&state) && state.frozen_lock);
    assert(c1_ui_step(state, C1_UI_EVENT_HOME, NULL).state.page == C1_UI_PAGE_LOCK);
    assert(c1_ui_unlock(&state) && state.page == C1_UI_PAGE_TERMINAL);
    state.page = C1_UI_PAGE_WIFI_PASSWORD;
    state.preferences.lock_style = C1_LOCK_WALLPAPER;    strcpy(state.secret, "sensitive"); state.secret_length = 9;
    assert(c1_ui_enter_lock(&state) && !state.frozen_lock && state.secret_length == 0);
    assert(c1_ui_unlock(&state) && state.page == C1_UI_PAGE_WIFI);
    state = c1_ui_initial_state();
    for (unsigned i = 0; i < 4; ++i) state = c1_ui_step(state, C1_UI_EVENT_DOWN, NULL).state;
    assert(state.selection == 4U);
    state = c1_ui_step(state, C1_UI_EVENT_ENTER, NULL).state;
    assert(state.page == C1_UI_PAGE_SETTINGS && state.selection == 0);
    state = c1_ui_step(state, C1_UI_EVENT_RIGHT, NULL).state;
    assert(state.preferences.language == C1_LANGUAGE_EN);

    test_preferences_validation(file);
    test_power_modes(file);
    test_lock_text_draft();
    test_lock_text_cursor();
    test_freeze_password_redraw();
    test_navigation();
    test_power_and_about(file);

    /* Time-worker policy, applet probing and lifecycle coverage live in
     * test_time_sync.c, with injected kernel evidence and private proc fixtures.
     * Desktop tests must not depend on the host's real clock or NTP services. */
    unlink(link); unlink(file); rmdir(root);
    puts("desktop preferences, lock and navigation tests passed");
    return 0;
}
