#define _DEFAULT_SOURCE 1
#include <sys/ioctl.h>
#include <stdarg.h>
static int test_ioctl(int fd, unsigned long request, ...);
#define ioctl test_ioctl
#include "../src/hal/linux/ui_runtime.c"
#undef ioctl
#include <assert.h>

static unsigned int held_code = KEY_WAKEUP;
static bool physically_held = true;
static int query_error;
static unsigned int queries;

static int test_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    unsigned char *bits;
    (void)request;
    assert(fd == 42);
    ++queries;
    if (query_error) { errno = EIO; return -1; }
    va_start(ap, request);
    bits = va_arg(ap, unsigned char *);
    va_end(ap);
    if (physically_held) bits[held_code / 8U] |= 1U << (held_code % 8U);
    return 0;
}

static void test_key(unsigned int code)
{
    c1_power_key key = {0};
    c1_power_policy policy;
    struct pollfd inputs[C1_UI_INPUT_COUNT] = {{42, POLLIN, 0}, {42, POLLIN, 0}};
    struct input_event input = {.type = EV_KEY, .code = code, .value = 1};
    held_code = code;
    physically_held = true;
    query_error = 0;
    queries = 0;
    c1_power_policy_init(&policy, 100);
    assert(power_key_input(&key, &policy, 1, &input, 100) == C1_POWER_KEY_NONE);
    assert(key.down);
    assert(deadline_timeout(-1, c1_power_key_deadline(&key), 100) == 3000);
    assert(power_key_timer(&key, inputs, 3099) == C1_POWER_KEY_NONE);
    assert(queries == 0);
    assert(power_key_timer(&key, inputs, 3100) == C1_POWER_KEY_SHUTDOWN);
    assert(queries == 1);
    assert(power_key_timer(&key, inputs, 9000) == C1_POWER_KEY_NONE);
    input.value = 0;
    assert(power_key_input(&key, &policy, 1, &input, 9000) == C1_POWER_KEY_NONE);
    assert(!key.down);

    input.value = 1;
    (void)power_key_input(&key, &policy, 1, &input, 10000);
    physically_held = false; /* Release hidden by another process's grab. */
    assert(power_key_timer(&key, inputs, 13000) == C1_POWER_KEY_NONE);
    assert(!key.down);
    physically_held = true;
    (void)power_key_input(&key, &policy, 1, &input, 14000);
    query_error = 1;
    assert(power_key_timer(&key, inputs, 17000) == C1_POWER_KEY_NONE);
    assert(!key.down);
    query_error = 0;

    (void)power_key_input(&key, &policy, 1, &input, 18000);
    input.value = 0; /* Drain queued release before timer, even after deadline. */
    assert(power_key_input(&key, &policy, 1, &input, 22000) == C1_POWER_KEY_NONE);
    assert(power_key_timer(&key, inputs, 22000) == C1_POWER_KEY_NONE);

    input.value = 1;
    (void)power_key_input(&key, &policy, 1, &input, 23000);
    input.value = 0;
    assert(power_key_input(&key, &policy, 1, &input, 23100) == C1_POWER_KEY_SHORT);

    c1_power_policy_resumed(&policy, 24000);
    input.value = 1;
    assert(power_key_input(&key, &policy, 1, &input, 24000) == C1_POWER_KEY_NONE);
    assert(!key.down);
    assert(power_key_timer(&key, inputs, 27000) == C1_POWER_KEY_NONE);
    input.value = 0;
    (void)power_key_input(&key, &policy, 1, &input, 27100);
    input.value = 1;
    (void)power_key_input(&key, &policy, 1, &input, 28000);
    assert(power_key_timer(&key, inputs, 31000) == C1_POWER_KEY_SHUTDOWN);
    physically_held = false;
    assert(power_key_timer(&key, inputs, 32000) == C1_POWER_KEY_NONE);
    assert(!key.down); /* Failed shutdown plus lost release must not block idle forever. */
}

int main(void)
{
    test_key(KEY_WAKEUP);
    test_key(KEY_POWER);
    puts("power runtime tests passed (mock input only; no shutdown commands)");
    return 0;
}
