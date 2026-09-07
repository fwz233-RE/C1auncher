/* Standalone host regression: includes input.c to exercise internal timing
 * without evdev devices. Compile with -DCHICHU_HOST -Isrc. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/platform/input.c"

static uint64_t clock_ms;
uint64_t now_ms(void) { return clock_ms; }

static int drain_count(void) {
    key_event_t event;
    int count = 0;
    while (input_get(&event)) count++;
    return count;
}

int main(void) {
    assert(input_init() == 0);
    q_push((key_event_t){K_OK, 0, false});
    input_cleanup();
    assert(input_init() == 0);
    assert(drain_count() == 0); /* reopening must not replay queued actions */

    input_set_repeat(500, 150);
    s_held[K_DOWN] = true;
    s_held_since[K_DOWN] = 1000;
    clock_ms = 60000; /* display stall or suspend while a key was held */
    input_poll(0);
    assert(drain_count() <= 2);
    input_poll(0);
    assert(drain_count() == 0); /* repeated polls must not replay old repeats */
    clock_ms += 150;
    input_poll(0);
    assert(drain_count() == 1);
    puts("PASS: input lifecycle and repeat backlog");
    return 0;
}
