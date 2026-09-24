/* Real runtime evdev mapping -> libtsm -> PTY -> production GUI decoder.
 * Only hardware/repository storage is substituted by test_pkg_gui.c. */
#define _DEFAULT_SOURCE 1
#include "../src/hal/linux/ui_runtime.c"

int test_physical_key(int master, unsigned short code, int application_cursor)
{
    c1_terminal_screen screen;
    c1_terminal_session session = {.master_fd = master, .child_pid = -1, .control_fd = -1,
                                   .state = C1_TERMINAL_RUNNING};
    bool changed = false;
    if (c1_terminal_screen_init(&screen) != C1_STATUS_OK) return -1;
    if (application_cursor) c1_terminal_screen_feed(&screen, "\033[?1h", 5U);
    c1_status status = send_terminal_key(&session, &screen, code,
                                        C1_UI_KEYBOARD_LOWER, false, false, &changed);
    c1_terminal_screen_destroy(&screen);
    return status == C1_STATUS_OK && changed ? 0 : -1;
}

/* Test-only GUI receiver fixture. Production validates the actual foreground
 * executable/argv in terminal_is_pkg_gui(); test_ui_lifecycle.c exercises those
 * real /proc + PTY checks separately. This bridge substitutes only that receiver
 * identity and runs the production Shift event reducer and PTY write path.
 *
 * test_physical_shift(master, 0, 0): one press/release -> ESC [ 57441 u.
 * test_physical_shift(master, 0, 1): auto-repeat before release -> no bytes.
 * test_physical_shift(master, KEY_SPACE, 0): Shift+Space -> ESC [ 32 ; 2 u.
 * test_physical_shift(master, KEY_Q, 0): existing Shift+Q mapping -> '1'.
 * Other ordinary chord codes use send_terminal_key; no chord emits a Shift tap.
 * Return 0 on success, -1 on initialization/write failure. No state is retained.
 */
int test_physical_shift(int master, unsigned short chord_code, int repeated)
{
    c1_ui_shift_key shift = {0};
    c1_ui_state state = c1_ui_initial_state();
    c1_terminal_session session = {.master_fd = master, .child_pid = -1, .control_fd = -1,
                                   .state = C1_TERMINAL_RUNNING};
    c1_status status = C1_STATUS_OK;
    bool changed;
    state.page = C1_UI_PAGE_TERMINAL;
    state.keyboard_layer = C1_UI_KEYBOARD_LOWER;
    shift_key_focus(&shift, &state, &session);
    (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 1);
    if (repeated) (void)shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 2);
    if (chord_code) {
        (void)shift_key_event(&shift, 0U, chord_code, 1);
        if (chord_code == KEY_SPACE) status = send_shift_space(&session);
        else {
            c1_terminal_screen screen;
            if (c1_terminal_screen_init(&screen) != C1_STATUS_OK) return -1;
            changed = false;
            status = send_terminal_key(&session, &screen, chord_code,
                                       state.keyboard_layer, shift.pressed, false, &changed);
            c1_terminal_screen_destroy(&screen);
        }
        (void)shift_key_event(&shift, 0U, chord_code, 0);
    }
    if (status == C1_STATUS_OK && shift_key_event(&shift, 0U, KEY_LEFTSHIFT, 0))
        status = apply_shift_tap(&state, &session, true, &changed);
    return status == C1_STATUS_OK ? 0 : -1;
}
