#ifndef C1_HAL_LINUX_LED_H
#define C1_HAL_LINUX_LED_H

#include <stdbool.h>
#include <stdint.h>

#define C1_LED_CHASER_COUNT 4U
#define C1_LED_TRIGGER_MAX 64U
#define C1_LED_ROOT_MAX 256U

typedef struct {
    char trigger[C1_LED_TRIGGER_MAX];
    unsigned int brightness;
    unsigned int delay_on;
    unsigned int delay_off;
    bool has_delay_on;
    bool has_delay_off;
} c1_linux_led_snapshot;

typedef struct {
    c1_linux_led_snapshot saved[C1_LED_CHASER_COUNT];
    char root[C1_LED_ROOT_MAX];
    unsigned int step;
    int64_t next_step_at;
    bool active;
} c1_linux_led_chaser;

bool c1_linux_led_chaser_start(c1_linux_led_chaser *chaser,
                               const char *root,
                               int64_t now);
void c1_linux_led_chaser_tick(c1_linux_led_chaser *chaser, int64_t now);
int c1_linux_led_chaser_timeout(const c1_linux_led_chaser *chaser, int64_t now);
void c1_linux_led_chaser_stop(c1_linux_led_chaser *chaser);

#endif