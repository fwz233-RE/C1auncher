#define _DEFAULT_SOURCE 1
#define c1_preferences_save runtime_save_preferences
#include "../src/hal/linux/ui_runtime.c"
#undef c1_preferences_save
#include "protocol.h"
#include <assert.h>
#include <sys/socket.h>

/* Exercise production save transitions without touching the host config. */
static bool save_ok = true;
static unsigned save_calls;
static c1_preferences saved_preferences;
bool runtime_save_preferences(const c1_preferences *preferences, const char *path)
{
    assert(!strcmp(path, C1_DESKTOP_CONFIG));
    ++save_calls;
    saved_preferences = *preferences;
    return save_ok;
}

static bool lease;
static char committed[4096];
static uint32_t forwarded, forwarded_mod;
static c1_terminal_key special_key;
static unsigned specials, scroll_up, scroll_down;
void c1_terminal_screen_scroll_page_up(c1_terminal_screen *s) { (void)s; ++scroll_up; }
void c1_terminal_screen_scroll_page_down(c1_terminal_screen *s) { (void)s; ++scroll_down; }
bool c1_app_run_active(void) { return lease; }
c1_status c1_terminal_write(c1_terminal_session *s, const void *bytes, size_t count)
{
    (void)s; assert(strlen(committed) + count < sizeof(committed));
    size_t used = strlen(committed); memcpy(committed + used, bytes, count); committed[used + count] = 0;
    return C1_STATUS_OK;
}
bool c1_terminal_screen_character(c1_terminal_screen *s, uint32_t cp, unsigned mods)
{ (void)s; forwarded = cp; forwarded_mod = mods; return true; }
bool c1_terminal_screen_special(c1_terminal_screen *s, c1_terminal_key key, unsigned mods)
{ (void)s; special_key = key; forwarded_mod = mods; ++specials; return true; }
void c1_terminal_screen_scroll_reset(c1_terminal_screen *s) { (void)s; }
size_t c1_terminal_screen_take_reply(c1_terminal_screen *s, void *buf, size_t capacity)
{ (void)s; (void)buf; (void)capacity; return 0; }

static c1_ui_state runtime_editor(void)
{
    c1_input_method_close(&desktop_input);
    memset(&input_view, 0, sizeof(input_view));
    c1_ui_state state = c1_ui_initial_state();
    strcpy(state.preferences.lock_text, "original");
    state.page = C1_UI_PAGE_LOCK_TEXT;
    strcpy(state.lock_text_draft, "AB"); state.lock_text_cursor = 1U;
    input_focus = state.page;
    save_calls = 0U; save_ok = true;
    return state;
}

static int fake_input_service(void)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    desktop_input.client.fd = pair[0]; desktop_input.enabled = true;
    desktop_input.view.flags = input_view.flags = C1_IME_READY | C1_IME_CHINESE;
    return pair[1];
}

static void physical_key(c1_ui_state *state, c1_power_policy *policy, uint16_t code, bool shift)
{
    assert(route_lock_text_key(state, policy, code, 1, shift, false));
}

static struct c1_ime_request wire_request(int server)
{
    struct c1_ime_response reply;
    assert(c1_input_method_tick(&desktop_input, &reply, 10) == 0);
    unsigned char bytes[C1_WIRE_HEADER];
    struct c1_ime_request request; uint32_t sequence;
    assert(recv(server, bytes, sizeof(bytes), 0) == (ssize_t)sizeof(bytes));
    assert(c1_decode_request(bytes, sizeof(bytes), &request, &sequence));
    assert(sequence == desktop_input.client.pending_sequence);
    return request;
}

static void wire_reply(int server, c1_ui_state *state, c1_power_policy *policy,
                        unsigned flags, const char *preedit, const char *commit, unsigned count)
{
    unsigned char bytes[C1_IME_MAX_PACKET] = {0};
    size_t preedit_size = strlen(preedit), commit_size = strlen(commit);
    size_t size = C1_WIRE_REPLY_HEADER;
    c1_put32(bytes, C1_WIRE_MAGIC); c1_put16(bytes + 4, C1_IME_PROTOCOL_VERSION);
    c1_put16(bytes + 6, desktop_input.client.pending_request.operation | C1_WIRE_REPLY);
    c1_put32(bytes + 8, desktop_input.client.pending_sequence);
    c1_put32(bytes + 16, desktop_input.client.pending_request.keysym);
    c1_put32(bytes + 20, flags);
    c1_put16(bytes + 32, (uint16_t)preedit_size); c1_put16(bytes + 34, (uint16_t)commit_size);
    c1_put16(bytes + 36, (uint16_t)count);
    memcpy(bytes + size, preedit, preedit_size); size += preedit_size;
    memcpy(bytes + size, commit, commit_size); size += commit_size;
    for (unsigned i = 0; i < count; ++i) {
        c1_put16(bytes + size, 3); size += 2U;
        memcpy(bytes + size, i ? "呢" : "你", 3); size += 3U;
    }
    c1_put32(bytes + 12, (uint32_t)size);
    assert(send(server, bytes, size, 0) == (ssize_t)size);
    struct c1_ime_response reply;
    assert(c1_input_method_tick(&desktop_input, &reply, 11) == 1);
    assert(deliver_input(state, policy, NULL, NULL, &reply) == C1_STATUS_OK);
}

static void test_physical_editor(void)
{
    c1_power_policy policy; c1_power_policy_init(&policy, 0);
    const unsigned ready = C1_IME_READY | C1_IME_CHINESE;
    const uint16_t confirms[] = {KEY_OK, KEY_ENTER};
    for (unsigned variant = 0; variant < 2; ++variant) {
        c1_ui_state state = runtime_editor();
        int server = fake_input_service();
        /* Entire burst arrives before a candidate view: route real Linux codes. */
        physical_key(&state, &policy, KEY_N, false);
        physical_key(&state, &policy, KEY_RIGHT, false);
        physical_key(&state, &policy, confirms[variant], false);
        physical_key(&state, &policy, confirms[1U - variant], false);
        assert(desktop_input.count == 4U && state.page == C1_UI_PAGE_LOCK_TEXT && !save_calls);
        struct c1_ime_request request = wire_request(server);
        assert(request.operation == C1_IME_OP_KEY && request.keysym == 'n');
        wire_reply(server, &state, &policy, ready | C1_IME_COMPOSING | C1_IME_CONSUMED, "n", "", 2U);
        struct c1_ime_response reply;
        assert(c1_input_method_tick(&desktop_input, &reply, 12) == 1);
        assert(deliver_input(&state, &policy, NULL, NULL, &reply) == C1_STATUS_OK);
        assert(input_view.highlighted_candidate == 1U && state.lock_text_cursor == 1U);
        request = wire_request(server);
        assert(request.operation == C1_IME_OP_SELECT && request.value == 1U);
        wire_reply(server, &state, &policy, ready | C1_IME_CONSUMED, "", "呢", 0);
        assert(!strcmp(state.lock_text_draft, "A呢B") && state.lock_text_cursor == 4U);
        request = wire_request(server);
        assert(request.operation == C1_IME_OP_KEY && request.keysym == C1_IME_KEY_RETURN);
        wire_reply(server, &state, &policy, ready, "", "", 0); /* Second queued Enter is unconsumed. */
        assert(state.page == C1_UI_PAGE_LOCK_TEXT && !save_calls && !desktop_input.count);
        assert(!strcmp(state.preferences.lock_text, "original"));
        assert(route_lock_text_key(&state, &policy, confirms[variant], 2, false, false));
        assert(route_lock_text_key(&state, &policy, confirms[variant], 0, false, false));
        assert(!save_calls); /* No auto-repeat/release saves after commit. */
        physical_key(&state, &policy, confirms[variant], false);
        assert(state.page == C1_UI_PAGE_SETTINGS && save_calls == 1U);
        assert(!strcmp(saved_preferences.lock_text, "A呢B") && desktop_input.client.fd < 0);
        close(server);
    }
    c1_ui_state state = runtime_editor();
    int server = fake_input_service();
    state.keyboard_layer = C1_UI_KEYBOARD_UPPER;
    physical_key(&state, &policy, KEY_X, false);
    assert(wire_request(server).keysym == 'X');
    assert(!desktop_input.count && desktop_input.client.pending_sequence && !input_view.preedit[0]);
    physical_key(&state, &policy, KEY_OK, false); /* In-flight only; no candidate has ever appeared. */
    wire_reply(server, &state, &policy, ready, "", "", 0);
    assert(!strcmp(state.lock_text_draft, "AXB") && state.lock_text_cursor == 2U);
    assert(wire_request(server).keysym == C1_IME_KEY_RETURN);
    wire_reply(server, &state, &policy, ready, "", "", 0);
    assert(state.page == C1_UI_PAGE_LOCK_TEXT && !save_calls);
    /* Idle Chinese mode still forwards cursor moves, without editing candidates. */
    physical_key(&state, &policy, KEY_LEFT, false);
    struct c1_ime_response local;
    assert(c1_input_method_tick(&desktop_input, &local, 12) == 1);
    assert(deliver_input(&state, &policy, NULL, NULL, &local) == C1_STATUS_OK);
    assert(state.lock_text_cursor == 1U);
    physical_key(&state, &policy, KEY_ENTER, false);
    assert(save_calls == 1U && !strcmp(saved_preferences.lock_text, "AXB")); close(server);

    state = runtime_editor(); server = fake_input_service();
    assert(c1_input_method_toggle(&desktop_input, NULL, 0) && !desktop_input.enabled);
    physical_key(&state, &policy, KEY_OK, false); /* A pending mode change is input, too. */
    assert(wire_request(server).operation == C1_IME_OP_MODE);
    wire_reply(server, &state, &policy, C1_IME_READY | C1_IME_CONSUMED, "", "", 0);
    assert(wire_request(server).keysym == C1_IME_KEY_RETURN);
    wire_reply(server, &state, &policy, C1_IME_READY, "", "", 0);
    assert(state.page == C1_UI_PAGE_LOCK_TEXT && !save_calls);
    physical_key(&state, &policy, KEY_ENTER, false); assert(save_calls == 1U); close(server);

    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        state = runtime_editor(); server = fake_input_service();
        if (scenario == 0) input_view.flags |= C1_IME_COMPOSING;
        if (scenario == 1) strcpy(input_view.preedit, "n");
        if (scenario == 2) input_view.candidate_count = 1U;
        physical_key(&state, &policy, KEY_OK, false);
        assert(desktop_input.count == 1U && !save_calls && state.page == C1_UI_PAGE_LOCK_TEXT);
        c1_input_method_close(&desktop_input); close(server);
    }
    for (unsigned home = 0; home < 2; ++home) {
        state = runtime_editor(); server = fake_input_service();
        physical_key(&state, &policy, KEY_N, false);
        (void)wire_request(server);
        physical_key(&state, &policy, home ? KEY_HOME : KEY_BACK, false);
        assert(state.page == (home ? C1_UI_PAGE_DESKTOP : C1_UI_PAGE_SETTINGS));
        assert(!state.lock_text_draft[0] && !state.lock_text_cursor && !save_calls);
        assert(!strcmp(state.preferences.lock_text, "original"));
        assert(desktop_input.client.fd < 0 && !desktop_input.count && !input_view.preedit[0]);
        close(server);
    }
    state = runtime_editor();
    physical_key(&state, &policy, KEY_X, false);
    assert(!strcmp(state.lock_text_draft, "AxB") && state.lock_text_cursor == 2U);
    c1_ui_state before = state;
    physical_key(&state, &policy, KEY_LEFT, false);
    assert(!states_equal(&before, &state)); /* Cursor-only edits request redraw. */
    physical_key(&state, &policy, KEY_Q, true);
    assert(!strcmp(state.lock_text_draft, "A1xB"));
    physical_key(&state, &policy, KEY_DELETE, false);
    assert(!strcmp(state.lock_text_draft, "AxB") && state.lock_text_cursor == 1U);
    save_ok = false;
    physical_key(&state, &policy, KEY_OK, false);
    assert(state.page == C1_UI_PAGE_LOCK_TEXT && state.lock_text_cursor == 1U && state.wifi_notice[0]);
    assert(!strcmp(state.lock_text_draft, "AxB") && !strcmp(state.preferences.lock_text, "original"));
    save_ok = true;
    physical_key(&state, &policy, KEY_ENTER, false);
    assert(state.page == C1_UI_PAGE_SETTINGS && save_calls == 2U && !strcmp(saved_preferences.lock_text, "AxB"));
    state = runtime_editor(); state.lock_text_draft[0] = 0; state.lock_text_cursor = 0;
    physical_key(&state, &policy, KEY_OK, false);
    assert(state.page == C1_UI_PAGE_LOCK_TEXT && state.wifi_notice[0] && !save_calls);
    c1_input_method_close(&desktop_input);
    memset(&input_view, 0, sizeof(input_view));
}

static void real_drain(c1_ui_state *state, c1_power_policy *policy)
{
    int64_t deadline = monotonic_milliseconds() + 10000;
    while (desktop_input.count || desktop_input.client.pending_sequence) {
        assert(monotonic_milliseconds() < deadline);
        struct c1_ime_response reply;
        int result = c1_input_method_tick(&desktop_input, &reply, monotonic_milliseconds());
        assert(result >= 0 && !desktop_input.failed);
        if (result == 1) assert(deliver_input(state, policy, NULL, NULL, &reply) == C1_STATUS_OK);
        struct pollfd fd = {desktop_input.client.fd, c1_input_method_poll_events(&desktop_input), 0};
        if (desktop_input.count || desktop_input.client.pending_sequence) assert(poll(&fd, 1, 100) >= 0);
    }
}

static void real_service(const char *path)
{
    c1_ui_state state = runtime_editor();
    state.lock_text_draft[0] = 0; state.lock_text_cursor = 0;
    c1_power_policy policy; c1_power_policy_init(&policy, 0);
    c1_input_method_init(&desktop_input);
    /* Test-only cold deployment wait; the actual UI has a bounded 5s timeout. */
    assert(c1_ime_connect(&desktop_input.client, path) == 0);
    struct c1_ime_request ready = {C1_IME_OP_STATUS, 0, 0, 0};
    assert(c1_ime_send(&desktop_input.client, &ready) == 1);
    struct pollfd startup = {desktop_input.client.fd, POLLIN, 0};
    assert(poll(&startup, 1, 120000) == 1);
    struct c1_ime_response initial;
    assert(c1_ime_receive(&desktop_input.client, &initial) == 1 && (initial.flags & C1_IME_READY));
    assert(c1_input_method_toggle(&desktop_input, path, monotonic_milliseconds()));
    static const uint16_t keys[] = {KEY_N, KEY_I, KEY_H, KEY_A, KEY_O, KEY_OK, KEY_ENTER};
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) physical_key(&state, &policy, keys[i], false);
    real_drain(&state, &policy);
    assert(!strcmp(state.lock_text_draft, "你好"));
    assert(state.page == C1_UI_PAGE_LOCK_TEXT && !save_calls); /* Fast second confirm must not save. */
    /* Move through the committed Chinese code points, then commit in the middle. */
    physical_key(&state, &policy, KEY_LEFT, false);
    real_drain(&state, &policy);
    assert(state.lock_text_cursor == 3U);
    physical_key(&state, &policy, KEY_N, false);
    physical_key(&state, &policy, KEY_VOLUMEUP, false);
    physical_key(&state, &policy, KEY_RIGHT, false);
    physical_key(&state, &policy, KEY_RIGHT, false);
    real_drain(&state, &policy);
    assert(input_view.candidate_count == 5 && input_view.highlighted_candidate == 2);
    assert(state.lock_text_cursor == 3U);
    char expected[C1_IME_TEXT_CAPACITY], middle[C1_IME_TEXT_CAPACITY + 16];
    strcpy(expected, input_view.candidates[2]);
    snprintf(middle, sizeof(middle), "你%s好", expected);
    physical_key(&state, &policy, KEY_OK, false);
    real_drain(&state, &policy);
    assert(!strcmp(state.lock_text_draft, middle) && state.page == C1_UI_PAGE_LOCK_TEXT && !save_calls);
    assert(state.lock_text_cursor == 3U + strlen(expected));
    physical_key(&state, &policy, KEY_ENTER, false);
    assert(state.page == C1_UI_PAGE_SETTINGS && save_calls == 1U && !strcmp(saved_preferences.lock_text, middle));
    assert(c1_input_method_toggle(&desktop_input, path, monotonic_milliseconds()));
    /* Commit to the terminal uses exactly the same queue, not shell key replay. */
    state.page = C1_UI_PAGE_TERMINAL;
    committed[0] = 0;
    assert(c1_input_method_key(&desktop_input, 'n', 0));
    assert(c1_input_method_key(&desktop_input, C1_IME_KEY_PAGE_DOWN, 0));
    assert(c1_input_method_key(&desktop_input, C1_IME_KEY_PAGE_UP, 0));
    real_drain(&state, &policy);
    assert(input_view.candidate_count == 5 && input_view.highlighted_candidate == 0);
    strcpy(expected, input_view.candidates[4]);
    assert(c1_input_method_key(&desktop_input, input_keysym(KEY_T, state.keyboard_layer, true, false), 0));
    real_drain(&state, &policy);
    assert(!strcmp(committed, expected));
    c1_input_method_close(&desktop_input);
    puts("real Rime -> desktop asynchronous queue -> UTF-8 lock text passed");
    puts("real Rime -> volume next/previous, highlighted Enter and Shift+5 terminal commit passed");
}
int main(int argc, char **argv)
{
    if (argc == 2) { real_service(argv[1]); return 0; }
    test_physical_editor();
    c1_ui_state state = c1_ui_initial_state();
    c1_power_policy policy; c1_power_policy_init(&policy, 0);
    c1_input_method_init(&desktop_input);
    input_terminal_allowed = true;
    state.page = C1_UI_PAGE_TERMINAL;
    unsigned columns, rows;
    terminal_geometry(&state, &columns, &rows); assert(columns == 37 && rows == 8);
    desktop_input.enabled = true;
    terminal_geometry(&state, &columns, &rows); assert(columns == 37 && rows == 6);
    desktop_input.enabled = false; desktop_input.failed = true;
    terminal_geometry(&state, &columns, &rows); assert(columns == 37 && rows == 6);
    desktop_input.failed = false;
    terminal_geometry(&state, &columns, &rows); assert(columns == 37 && rows == 8);
    struct c1_ime_response reply = {0};
    reply.flags = C1_IME_READY | C1_IME_CONSUMED;
    reply.request.operation = C1_IME_OP_KEY;
    strcpy(reply.commit, "你好");
    assert(deliver_input(&state, &policy, NULL, NULL, &reply) == C1_STATUS_OK);
    assert(!strcmp(committed, "你好") && forwarded == 0);
    reply.flags = C1_IME_READY; reply.request.keysym = 'c'; reply.request.modifiers = C1_IME_MOD_CONTROL;
    assert(deliver_input(&state, &policy, NULL, NULL, &reply) == C1_STATUS_OK);
    assert(!strcmp(committed, "你好你好") && forwarded == 'c' && forwarded_mod == C1_TERMINAL_MOD_CONTROL);
    reply.commit[0] = 0; reply.request.keysym = C1_IME_KEY_RETURN;
    assert(deliver_input(&state, &policy, NULL, NULL, &reply) == C1_STATUS_OK);
    assert(specials == 1 && special_key == C1_TERMINAL_KEY_ENTER);
    assert(input_keysym(KEY_VOLUMEDOWN, C1_UI_KEYBOARD_LOWER, false, false) == C1_IME_KEY_PAGE_UP);
    assert(input_keysym(KEY_VOLUMEUP, C1_UI_KEYBOARD_LOWER, false, false) == C1_IME_KEY_PAGE_DOWN);
    assert(map_page_key(C1_UI_PAGE_BATTERY, KEY_VOLUMEDOWN) == C1_UI_EVENT_VIEW_PREVIOUS);
    assert(map_page_key(C1_UI_PAGE_BATTERY, KEY_VOLUMEUP) == C1_UI_EVENT_VIEW_NEXT);
    assert(map_page_key(C1_UI_PAGE_BATTERY, KEY_OK) == C1_UI_EVENT_ENTER);
    assert(map_page_key(C1_UI_PAGE_BATTERY, KEY_ENTER) == C1_UI_EVENT_ENTER);
    assert(map_page_key(C1_UI_PAGE_WIFI, KEY_VOLUMEUP) == C1_UI_EVENT_NONE);
    c1_ui_status battery_status = {0};
    battery_status.battery_history.count = 2;
    battery_status.battery_history.samples[0].timestamp = C1_BATTERY_MIN_TIME;
    battery_status.battery_history.samples[1].timestamp = C1_BATTERY_MIN_TIME + 60;
    c1_ui_state battery_state = c1_ui_initial_state();
    battery_state.page = C1_UI_PAGE_BATTERY;
    battery_state = c1_ui_step(battery_state, map_page_key(battery_state.page, KEY_LEFT), &battery_status).state;
    assert(battery_state.battery_selected_at == C1_BATTERY_MIN_TIME && battery_state.battery_view == C1_BATTERY_VIEW_DAY);
    battery_state = c1_ui_step(battery_state, map_page_key(battery_state.page, KEY_VOLUMEUP), &battery_status).state;
    assert(battery_state.battery_view == C1_BATTERY_VIEW_HOUR && battery_state.battery_selected_at == C1_BATTERY_MIN_TIME);
    for (unsigned key = 0; key < 2U; ++key) {
        c1_ui_transition unchanged = c1_ui_step(battery_state,
            map_page_key(battery_state.page, key ? KEY_ENTER : KEY_OK), &battery_status);
        assert(unchanged.state.battery_view == C1_BATTERY_VIEW_HOUR &&
               unchanged.state.battery_selected_at == C1_BATTERY_MIN_TIME && unchanged.action == C1_UI_ACTION_NONE);
    }
    battery_state = c1_ui_step(battery_state, map_page_key(battery_state.page, KEY_VOLUMEUP), &battery_status).state;
    assert(battery_state.battery_view == C1_BATTERY_VIEW_MINUTE && battery_state.battery_selected_at == C1_BATTERY_MIN_TIME);
    battery_state = c1_ui_step(battery_state, map_page_key(battery_state.page, KEY_VOLUMEDOWN), &battery_status).state;
    assert(battery_state.battery_view == C1_BATTERY_VIEW_HOUR && battery_state.battery_selected_at == C1_BATTERY_MIN_TIME);
    battery_state = c1_ui_step(battery_state, map_page_key(battery_state.page, KEY_RIGHT), &battery_status).state;
    assert(!battery_state.battery_selected_at && battery_state.selection == 0 && battery_state.battery_view == C1_BATTERY_VIEW_HOUR);
    const uint16_t number_caps[] = {KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T};
    for (unsigned i = 0; i < 5; ++i)
        assert(input_keysym(number_caps[i], C1_UI_KEYBOARD_LOWER, true, false) == '1' + i);
    reply.request.keysym = C1_IME_KEY_PAGE_DOWN;
    assert(deliver_input(&state, &policy, NULL, NULL, &reply) == C1_STATUS_OK);
    assert(scroll_up == 1 && scroll_down == 0 && specials == 1);
    reply.request.keysym = C1_IME_KEY_PAGE_UP;
    assert(deliver_input(&state, &policy, NULL, NULL, &reply) == C1_STATUS_OK);
    assert(scroll_up == 1 && scroll_down == 1 && specials == 1);
    reply.flags |= C1_IME_CONSUMED;
    assert(deliver_input(&state, &policy, NULL, NULL, &reply) == C1_STATUS_OK);
    assert(scroll_up == 1 && scroll_down == 1); /* Candidate page never scrolls terminal. */
    bool changed = false;
    assert(send_terminal_key(NULL, NULL, KEY_VOLUMEUP, C1_UI_KEYBOARD_LOWER, false, false, &changed) == C1_STATUS_OK);
    assert(changed && special_key == C1_TERMINAL_KEY_PAGE_DOWN);
    assert(send_terminal_key(NULL, NULL, KEY_VOLUMEDOWN, C1_UI_KEYBOARD_LOWER, false, false, &changed) == C1_STATUS_OK);
    assert(special_key == C1_TERMINAL_KEY_PAGE_UP);
    state.page = C1_UI_PAGE_LOCK_TEXT; state.lock_text_draft[0] = 0;
    reply.flags |= C1_IME_CONSUMED; strcpy(reply.commit, "中文");
    assert(deliver_input(&state, &policy, NULL, NULL, &reply) == C1_STATUS_OK);
    assert(!strcmp(state.lock_text_draft, "中文"));
    state.page = C1_UI_PAGE_WIFI_PASSWORD;
    strcpy(input_view.preedit, "secret"); desktop_input.enabled = true;
    input_focus_update(&state);
    assert(!desktop_input.enabled && input_view.preedit[0] == 0);
    state.page = C1_UI_PAGE_TERMINAL; input_focus = state.page;
    desktop_input.enabled = true; lease = true;
    input_focus_update(&state); assert(!desktop_input.enabled); lease = false;
    c1_ui_state before = c1_ui_initial_state();
    c1_ui_transition next = c1_ui_step(before, C1_UI_EVENT_SELECT_NEXT, NULL);
    c1_service_worker worker = {.pid = 123};
    before = next.state; next = c1_ui_step(before, C1_UI_EVENT_ENTER, NULL);
    assert(!ignore_settings_update(&before, C1_UI_EVENT_ENTER, &next, &worker));
    assert(next.state.page == C1_UI_PAGE_TERMINAL);
    puts("desktop input runtime tests passed: geometry, forwarding, privacy, focus, navigation");
    return 0;
}
