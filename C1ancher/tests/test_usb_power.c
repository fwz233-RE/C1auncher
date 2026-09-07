#define _DEFAULT_SOURCE 1
#include "hal/linux/power.h"
#include "platform/liveness.h"
#include "services/wifi.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Exercise the actual power resume retry/error propagation as well as the USB
 * implementation. Only the default operations are replaced; no sysfs is used. */
static c1_usb_power_ops test_usb_ops;
#define c1_usb_power_default_ops test_usb_ops
#include "../src/hal/linux/power.c"
#undef c1_usb_power_default_ops

static unsigned int wifi_calls, terminal_calls, beat_calls;
void c1_liveness_beat(int64_t now) { (void)now; ++beat_calls; }
c1_status c1_wifi_resume(bool enabled, bool connected, bool managed)
{
    assert(enabled && connected && managed);
    ++wifi_calls;
    return C1_STATUS_OK;
}
c1_status c1_wifi_resume_ex(bool enabled, bool connected, bool managed,
                            const c1_wifi_operation_options *options)
{
    unsigned int previous_beats = beat_calls;
    assert(options != NULL && options->progress != NULL);
    assert(options->progress(C1_WIFI_PHASE_RESTORING, options->context));
    assert(beat_calls == previous_beats + 1U);
    return c1_wifi_resume(enabled, connected, managed);
}
c1_status c1_terminal_resume(c1_terminal_session *terminal, bool running)
{
    assert(terminal != NULL && running);
    ++terminal_calls;
    return C1_STATUS_OK;
}

typedef struct {
    bool adb, mtp, has_udc;
    char udc[C1_USB_POWER_UDC_CAPACITY];
    unsigned int elapsed, adb_ready_at, mtp_ready_at;
    unsigned int stops, starts, mtp_starts, unbinds, binds, beats;
    unsigned int fail_start, fail_mtp, fail_bind;
    bool fail_stop, fail_read, fail_unbind, race, lose_bind, probe_error;
    int missing_endpoint;
    char events[1024];
    size_t event_count;
} fixture;
static const c1_usb_power_paths *const paths = &c1_usb_power_default_paths;

static void event(fixture *f, char value)
{
    assert(f->event_count + 1U < sizeof(f->events));
    f->events[f->event_count++] = value;
    f->events[f->event_count] = '\0';
}
static int exists(void *data, const char *path)
{
    fixture *f = data;
    unsigned int i;
    if (f->probe_error) return -1;
    if (strcmp(path, paths->udc) == 0) return f->has_udc;
    if (strcmp(path, paths->adb_link) == 0) return f->adb;
    if (strcmp(path, paths->mtp_link) == 0) return f->mtp;
    for (i = 0; i < 2U; ++i)
        if (strcmp(path, paths->adb_endpoints[i]) == 0)
            return f->adb && f->elapsed >= f->adb_ready_at && f->missing_endpoint != (int)i + 1;
    for (i = 0; i < 3U; ++i)
        if (strcmp(path, paths->mtp_endpoints[i]) == 0)
            return f->mtp && f->elapsed >= f->mtp_ready_at && f->missing_endpoint != (int)i + 3;
    assert(!"unexpected probe");
    return -1;
}
static bool read_udc(void *data, const char *path, char *value, size_t capacity)
{
    fixture *f = data;
    assert(strcmp(path, paths->udc) == 0);
    event(f, 'R');
    if (f->fail_read) return false;
    assert(strlen(f->udc) < capacity);
    strcpy(value, f->udc);
    return true;
}
static bool write_udc(void *data, const char *path, const char *value)
{
    fixture *f = data;
    assert(strcmp(path, paths->udc) == 0);
    assert(!f->adb || f->elapsed >= f->adb_ready_at);
    assert(!f->mtp || f->elapsed >= f->mtp_ready_at);
    if (*value == '\0') {
        ++f->unbinds;
        event(f, 'U');
        if (f->fail_unbind) return false;
    } else {
        assert(f->udc[0] == '\0');
        ++f->binds;
        event(f, 'B');
        if (f->fail_bind != 0U) { --f->fail_bind; return false; }
    }
    strcpy(f->udc, value);
    if (f->lose_bind) f->udc[0] = '\0';
    return true;
}
static bool run(void *data, const char *script, const char *action)
{
    fixture *f = data;
    if (strcmp(script, paths->adb_script) == 0) {
        if (strcmp(action, "stop") == 0) {
            ++f->stops;
            event(f, 'S');
            assert(f->event_count >= 2U && f->events[f->event_count - 2U] == 'R');
            f->adb = false;
            f->udc[0] = '\0'; /* Stop can implicitly unbind UDC. */
            return !f->fail_stop;
        }
        assert(strcmp(action, "start") == 0);
        ++f->starts;
        event(f, 'A');
        f->adb = true;
        if (f->fail_start != 0U) { --f->fail_start; return false; }
        return true;
    }
    assert(strcmp(script, paths->mtp_script) == 0);
    assert(strcmp(action, "suspendoff") == 0 && f->mtp);
    ++f->mtp_starts;
    event(f, 'M');
    if (f->fail_mtp != 0U) { --f->fail_mtp; return false; }
    return true;
}
static void pause_ms(void *data, unsigned int milliseconds)
{
    fixture *f = data;
    f->elapsed += milliseconds;
    if (f->race && f->udc[0] == '\0') strcpy(f->udc, "13500000.otg");
}
static void heartbeat(void *data) { ++((fixture *)data)->beats; }
static void init(fixture *f, c1_usb_power_context *context, bool adb, bool mtp)
{
    memset(f, 0, sizeof(*f));
    memset(context, 0, sizeof(*context));
    f->adb = adb;
    f->mtp = mtp;
    f->has_udc = true;
    strcpy(f->udc, "13500000.otg");
    test_usb_ops = (c1_usb_power_ops){f, exists, read_udc, write_udc, run, pause_ms, heartbeat};
}
static bool prepare(c1_usb_power_context *context)
{
    return c1_usb_power_prepare(context, paths, &test_usb_ops);
}
static bool resume(c1_usb_power_context *context)
{
    return c1_usb_power_resume(context, paths, &test_usb_ops);
}

static void test_order_and_preservation(void)
{
    fixture f;
    c1_usb_power_context context;
    unsigned int flags;
    for (flags = 0; flags < 4U; ++flags) {
        unsigned int events;
        init(&f, &context, (flags & 1U) != 0U, (flags & 2U) != 0U);
        assert(prepare(&context));
        assert(strcmp(context.udc, "13500000.otg") == 0);
        f.adb_ready_at = 150U;
        f.mtp_ready_at = 400U;
        /* Model another process having rebound too early. */
        strcpy(f.udc, context.udc);
        assert(resume(&context));
        assert(f.starts == (flags & 1U));
        assert(f.mtp_starts == ((flags >> 1U) & 1U));
        /* Already bound: no forced enumeration, even with ADB/MTP enabled. */
        assert(f.binds == 0U && f.unbinds == 0U && !context.restore_pending);
        assert(f.adb == context.adb_enabled && f.mtp == context.mtp_enabled);
        events = (unsigned int)f.event_count;
        assert(resume(&context) && f.event_count == events); /* Idempotent. */
    }
    init(&f, &context, true, true);
    f.udc[0] = '\0';
    assert(prepare(&context) && resume(&context));
    assert(f.binds == 0U && f.unbinds == 0U && f.udc[0] == '\0');
    init(&f, &context, false, true);
    f.udc[0] = '\0';
    assert(prepare(&context));
    strcpy(f.udc, "13500000.otg");
    assert(!resume(&context)); /* A concurrent new binding is not ours to undo. */
    assert(f.binds == 0U && f.unbinds == 0U && context.restore_pending);
    init(&f, &context, true, true);
    assert(prepare(&context) && resume(&context));
    assert(f.binds == 1U && f.unbinds == 0U);
    assert(strchr(f.events, 'A') < strchr(f.events, 'B'));
    assert(strchr(f.events, 'M') < strchr(f.events, 'B'));
}

static void test_wait_and_retry(void)
{
    fixture f;
    c1_usb_power_context context;
    int endpoint;
    for (endpoint = 1; endpoint <= 5; ++endpoint) {
        init(&f, &context, true, true);
        assert(prepare(&context));
        f.missing_endpoint = endpoint;
        assert(!resume(&context));
        assert(f.elapsed == 5000U && f.beats >= 101U);
        assert(f.binds == 0U && context.restore_pending);
        f.missing_endpoint = 0;
        assert(resume(&context));
        assert(f.starts == 1U && f.mtp_starts == 1U); /* Don't restart ready services. */
    }
    init(&f, &context, true, true);
    assert(prepare(&context));
    f.adb_ready_at = f.mtp_ready_at = 5000U;
    assert(resume(&context) && f.elapsed == 5100U); /* Inclusive final poll. */
    init(&f, &context, true, true);
    assert(prepare(&context));
    f.adb_ready_at = 5001U;
    assert(!resume(&context) && f.elapsed == 5000U && f.binds == 0U);
    assert(resume(&context) && f.starts == 1U);
    init(&f, &context, true, true);
    assert(prepare(&context));
    f.fail_start = 1U;
    assert(!resume(&context) && context.restore_pending && f.mtp_starts == 1U);
    assert(resume(&context) && f.starts == 1U); /* Failed script did restore descriptors. */
    init(&f, &context, true, true);
    assert(prepare(&context));
    f.adb_ready_at = 500U;
    f.fail_start = 1U;
    assert(!resume(&context));
    assert(resume(&context) && f.starts == 2U);
    init(&f, &context, true, true);
    assert(prepare(&context));
    f.fail_mtp = 1U;
    assert(!resume(&context));
    assert(resume(&context) && f.starts == 1U && f.mtp_starts == 2U);
    init(&f, &context, true, true);
    assert(prepare(&context));
    f.fail_bind = 1U;
    assert(!resume(&context) && context.restore_pending);
    assert(resume(&context) && f.binds == 2U && f.starts == 1U);
}

static void test_failures(void)
{
    fixture f;
    c1_usb_power_context context;
    init(&f, &context, true, true);
    f.fail_read = true;
    assert(!prepare(&context) && f.stops == 0U);
    init(&f, &context, true, true);
    f.probe_error = true;
    assert(!prepare(&context) && f.stops == 0U);
    init(&f, &context, false, false);
    f.has_udc = false;
    assert(prepare(&context) && resume(&context) && f.event_count == 0U);
    init(&f, &context, true, false);
    f.has_udc = false;
    assert(!prepare(&context) && f.stops == 0U);
    init(&f, &context, true, true);
    f.fail_stop = true;
    assert(!prepare(&context) && context.restore_pending);
    assert(resume(&context)); /* Rollback restores partially stopped service. */
    init(&f, &context, false, true);
    assert(prepare(&context));
    f.fail_unbind = true;
    assert(resume(&context) && f.binds == 0U && f.unbinds == 0U);
    init(&f, &context, true, true);
    assert(prepare(&context));
    f.race = true;
    f.adb_ready_at = 50U; /* Vendor daemon rebinds during descriptor wait. */
    assert(resume(&context) && f.binds == 0U && f.unbinds == 0U);
    f.race = false;
    assert(resume(&context));
    init(&f, &context, true, true);
    assert(prepare(&context));
    f.lose_bind = true;
    assert(!resume(&context) && context.restore_pending);
    init(&f, &context, true, true);
    assert(prepare(&context));
    strcpy(f.udc, "other-controller");
    assert(!resume(&context) && f.binds == 0U && f.unbinds == 0U);
    init(&f, &context, false, true);
    assert(prepare(&context));
    f.adb = true;
    assert(!resume(&context) && f.starts == 0U && f.binds == 0U);
    init(&f, &context, true, true);
    assert(prepare(&context));
    f.fail_read = true;
    assert(!resume(&context) && context.restore_pending && f.binds == 0U);
}

static void test_power_propagation(void)
{
    fixture f;
    c1_linux_power_context power = {0};
    c1_terminal_session terminal = {0};
    init(&f, &power.usb, true, true);
    assert(prepare(&power.usb));
    f.missing_endpoint = 5;
    power.wifi_paused = power.wifi_enabled = power.wifi_connected = power.wifi_managed = true;
    power.terminal_paused = power.terminal_running = true;
    wifi_calls = terminal_calls = 0U;
    assert(c1_linux_power_resume(&power, &terminal) == C1_STATUS_IO_ERROR);
    assert(f.elapsed == 15000U && f.beats >= 303U && power.usb.restore_pending);
    assert(wifi_calls == 1U && terminal_calls == 1U && !power.wifi_paused);
    f.missing_endpoint = 0;
    assert(c1_linux_power_resume(&power, &terminal) == C1_STATUS_OK);
    assert(!power.usb.restore_pending && f.starts == 1U && f.mtp_starts == 1U);
    assert(wifi_calls == 1U && terminal_calls == 1U);
    assert(beat_calls >= 4U);
    init(&f, &power.usb, true, true);
    assert(prepare(&power.usb));
    f.fail_mtp = 3U;
    assert(c1_linux_power_resume(&power, &terminal) == C1_STATUS_IO_ERROR);
    assert(f.mtp_starts == 3U && f.starts == 1U && f.binds == 0U);
    assert(power.usb.restore_pending);
    assert(c1_linux_power_resume(&power, &terminal) == C1_STATUS_OK);
    assert(f.mtp_starts == 4U && !power.usb.restore_pending);
}

int main(void)
{
    test_order_and_preservation();
    test_wait_and_retry();
    test_failures();
    test_power_propagation();
    puts("USB power tests passed (readiness, ordering, flags, retry, errors, power integration)");
    return 0;
}
