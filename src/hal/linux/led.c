#include "hal/linux/led.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define C1_LED_FIRST_NUMBER 2U
#define C1_LED_STEP_INTERVAL_MS 180
#define C1_LED_BRIGHTNESS 255U
#define C1_LED_FILE_VALUE_MAX 512U

static bool led_path(char *path,
                     size_t path_size,
                     const c1_linux_led_chaser *chaser,
                     unsigned int index,
                     const char *attribute)
{
    int length = snprintf(path, path_size, "%s/led%u/%s", chaser->root,
                          C1_LED_FIRST_NUMBER + index, attribute);

    return length > 0 && (size_t)length < path_size;
}

static bool read_value(const char *path, char *value, size_t value_size)
{
    int descriptor;
    ssize_t count;

    if (value_size < 2U) {
        return false;
    }
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    count = read(descriptor, value, value_size - 1U);
    if (close(descriptor) != 0 || count <= 0) {
        return false;
    }
    value[count] = '\0';
    while (count > 0 && (value[count - 1] == '\n' || value[count - 1] == '\r' ||
                         value[count - 1] == ' ' || value[count - 1] == '\t')) {
        value[--count] = '\0';
    }
    return count > 0;
}

static bool write_value(const char *path, const char *value)
{
    size_t length = strlen(value);
    int descriptor = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t count;

    if (descriptor < 0) {
        return false;
    }
    if (ftruncate(descriptor, 0) != 0) {
        /* sysfs attributes do not support truncation; regular test files do. */
    }
    count = write(descriptor, value, length);
    if (close(descriptor) != 0) {
        return false;
    }
    return count == (ssize_t)length;
}

static bool read_unsigned(const char *path, unsigned int *result)
{
    char value[32];
    char *end;
    unsigned long parsed;

    if (!read_value(path, value, sizeof(value))) {
        return false;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT_MAX) {
        return false;
    }
    *result = (unsigned int)parsed;
    return true;
}

static bool write_unsigned(const char *path, unsigned int value)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%u", value);

    return length > 0 && (size_t)length < sizeof(text) && write_value(path, text);
}

static bool selected_trigger(const char *available, char *selected, size_t selected_size)
{
    const char *opening = strchr(available, '[');
    const char *closing;
    size_t length;

    if (opening == NULL) {
        return false;
    }
    closing = strchr(opening + 1, ']');
    if (closing == NULL) {
        return false;
    }
    length = (size_t)(closing - opening - 1);
    if (length == 0U || length >= selected_size) {
        return false;
    }
    memcpy(selected, opening + 1, length);
    selected[length] = '\0';
    return true;
}

static bool snapshot_led(c1_linux_led_chaser *chaser, unsigned int index)
{
    c1_linux_led_snapshot *snapshot = &chaser->saved[index];
    char path[C1_LED_ROOT_MAX + 32U];
    char triggers[C1_LED_FILE_VALUE_MAX];

    if (!led_path(path, sizeof(path), chaser, index, "trigger") ||
        !read_value(path, triggers, sizeof(triggers)) ||
        !selected_trigger(triggers, snapshot->trigger, sizeof(snapshot->trigger)) ||
        !led_path(path, sizeof(path), chaser, index, "brightness") ||
        !read_unsigned(path, &snapshot->brightness)) {
        return false;
    }
    if (led_path(path, sizeof(path), chaser, index, "delay_on")) {
        snapshot->has_delay_on = read_unsigned(path, &snapshot->delay_on);
    }
    if (led_path(path, sizeof(path), chaser, index, "delay_off")) {
        snapshot->has_delay_off = read_unsigned(path, &snapshot->delay_off);
    }
    return true;
}

static bool set_led(c1_linux_led_chaser *chaser, unsigned int index, unsigned int brightness)
{
    char path[C1_LED_ROOT_MAX + 32U];

    return led_path(path, sizeof(path), chaser, index, "brightness") &&
           write_unsigned(path, brightness);
}

static bool take_control(c1_linux_led_chaser *chaser, unsigned int index)
{
    char path[C1_LED_ROOT_MAX + 32U];

    return led_path(path, sizeof(path), chaser, index, "trigger") &&
           write_value(path, "none") && set_led(chaser, index, 0U);
}

static void restore_led(c1_linux_led_chaser *chaser, unsigned int index)
{
    const c1_linux_led_snapshot *snapshot = &chaser->saved[index];
    char path[C1_LED_ROOT_MAX + 32U];

    if (!led_path(path, sizeof(path), chaser, index, "trigger")) {
        return;
    }
    (void)write_value(path, "none");
    if (led_path(path, sizeof(path), chaser, index, "brightness")) {
        (void)write_unsigned(path, snapshot->brightness);
    }
    if (strcmp(snapshot->trigger, "none") == 0) {
        return;
    }
    if (!led_path(path, sizeof(path), chaser, index, "trigger") ||
        !write_value(path, snapshot->trigger)) {
        return;
    }
    if (snapshot->has_delay_on && led_path(path, sizeof(path), chaser, index, "delay_on")) {
        (void)write_unsigned(path, snapshot->delay_on);
    }
    if (snapshot->has_delay_off && led_path(path, sizeof(path), chaser, index, "delay_off")) {
        (void)write_unsigned(path, snapshot->delay_off);
    }
}

bool c1_linux_led_chaser_start(c1_linux_led_chaser *chaser,
                               const char *root,
                               int64_t now)
{
    size_t root_length;
    unsigned int index;

    if (chaser == NULL || root == NULL || now < 0) {
        return false;
    }
    memset(chaser, 0, sizeof(*chaser));
    root_length = strlen(root);
    if (root_length == 0U || root_length >= sizeof(chaser->root)) {
        return false;
    }
    memcpy(chaser->root, root, root_length + 1U);
    for (index = 0U; index < C1_LED_CHASER_COUNT; ++index) {
        if (!snapshot_led(chaser, index)) {
            return false;
        }
    }
    for (index = 0U; index < C1_LED_CHASER_COUNT; ++index) {
        if (!take_control(chaser, index)) {
            unsigned int restore_count = index + 1U;

            while (restore_count > 0U) {
                restore_led(chaser, --restore_count);
            }
            return false;
        }
    }
    chaser->active = true;
    chaser->step = 0U;
    if (!set_led(chaser, chaser->step, C1_LED_BRIGHTNESS)) {
        c1_linux_led_chaser_stop(chaser);
        return false;
    }
    chaser->next_step_at = now + C1_LED_STEP_INTERVAL_MS;
    return true;
}

void c1_linux_led_chaser_tick(c1_linux_led_chaser *chaser, int64_t now)
{
    unsigned int next;

    if (chaser == NULL || !chaser->active || now < chaser->next_step_at) {
        return;
    }
    next = (chaser->step + 1U) % C1_LED_CHASER_COUNT;
    if (!set_led(chaser, chaser->step, 0U) || !set_led(chaser, next, C1_LED_BRIGHTNESS)) {
        c1_linux_led_chaser_stop(chaser);
        return;
    }
    chaser->step = next;
    chaser->next_step_at = now + C1_LED_STEP_INTERVAL_MS;
}

int c1_linux_led_chaser_timeout(const c1_linux_led_chaser *chaser, int64_t now)
{
    int64_t remaining;

    if (chaser == NULL || !chaser->active) {
        return -1;
    }
    remaining = chaser->next_step_at - now;
    if (remaining <= 0) {
        return 0;
    }
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

void c1_linux_led_chaser_stop(c1_linux_led_chaser *chaser)
{
    unsigned int index;

    if (chaser == NULL || !chaser->active) {
        return;
    }
    chaser->active = false;
    for (index = 0U; index < C1_LED_CHASER_COUNT; ++index) {
        restore_led(chaser, index);
    }
}