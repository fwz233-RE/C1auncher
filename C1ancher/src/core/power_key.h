#ifndef C1_CORE_POWER_KEY_H
#define C1_CORE_POWER_KEY_H

#include <stdbool.h>
#include <stdint.h>

#define C1_POWER_KEY_HOLD_MS 3000

typedef enum {
    C1_POWER_KEY_NONE = 0,
    C1_POWER_KEY_SHORT,
    C1_POWER_KEY_SHUTDOWN
} c1_power_key_action;

typedef struct {
    bool down;
    bool fired;
    unsigned int source;
    unsigned int code;
    int64_t pressed_at;
} c1_power_key;

static inline void c1_power_key_reset(c1_power_key *key)
{
    *key = (c1_power_key){0};
}

static inline c1_power_key_action c1_power_key_tick(c1_power_key *key, int64_t now)
{
    if (key->down && !key->fired && now >= key->pressed_at &&
        now - key->pressed_at >= C1_POWER_KEY_HOLD_MS) {
        key->fired = true;
        return C1_POWER_KEY_SHUTDOWN;
    }
    return C1_POWER_KEY_NONE;
}

/* Call only for KEY_WAKEUP/KEY_POWER, after wake-event suppression. Repeats
 * cannot arm a hold. Only the same input source/key may release it. */
static inline c1_power_key_action c1_power_key_event(c1_power_key *key,
    unsigned int source, unsigned int code, int value, int64_t now)
{
    if (value == 1 && !key->down) {
        key->down = true;
        key->fired = false;
        key->source = source;
        key->code = code;
        key->pressed_at = now;
    } else if (value == 0 && key->down && key->source == source && key->code == code) {
        /* Shutdown is timer-only and requires a still-held physical key.
         * A delayed release must never become a shutdown request. */
        c1_power_key_action action = C1_POWER_KEY_NONE;
        if (!key->fired && now >= key->pressed_at &&
            now - key->pressed_at < C1_POWER_KEY_HOLD_MS)
            action = C1_POWER_KEY_SHORT;
        c1_power_key_reset(key);
        return action;
    }
    return C1_POWER_KEY_NONE;
}

static inline int64_t c1_power_key_deadline(const c1_power_key *key)
{
    return key->down && !key->fired ? key->pressed_at + C1_POWER_KEY_HOLD_MS : -1;
}

#endif
