/* Maintenance-only probe linked to production USB suspend handling.
 * Never installed as a launcher; never changes persistent power preferences.
 * No forced USB re-enumeration or full-service restart fallback. */
#define _DEFAULT_SOURCE 1
#include "hal/linux/usb_power.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void show(const char *name, const char *path)
{
    char value[256];
    FILE *f = fopen(path, "r");
    printf("%s=", name);
    if (f != NULL) {
        if (fgets(value, sizeof(value), f) != NULL) fputs(value, stdout);
        else puts("unavailable");
        fclose(f);
    } else puts("unavailable");
}

int main(int argc, char **argv)
{
    c1_usb_power_context context = {0};
    int suspend_requested, suspend_ok = 1, restore_ok = 0;
    if (argc != 2 || (strcmp(argv[1], "--usb-only") != 0 &&
                      strcmp(argv[1], "--suspend-confirmed") != 0)) return 64;
    suspend_requested = strcmp(argv[1], "--suspend-confirmed") == 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    show("boot_before", "/proc/sys/kernel/random/boot_id");
    show("success_before", "/sys/power/suspend_stats/success");
    show("uptime_before", "/proc/uptime");
    puts("stage=prepare");
    if (!c1_usb_power_prepare(&context, &c1_usb_power_default_paths,
                              &c1_usb_power_default_ops)) {
        puts("prepare=failed");
        suspend_ok = 0;
    } else if (suspend_requested) {
        FILE *f;
        puts("stage=entering_mem");
        f = fopen("/sys/power/state", "w");
        if (f == NULL) suspend_ok = 0;
        else {
            if (fputs("mem\n", f) < 0) suspend_ok = 0;
            if (fclose(f) != 0) suspend_ok = 0;
        }
        puts("stage=returned_from_mem");
    }
    for (unsigned int attempt = 0; attempt < 3U; ++attempt) {
        if (c1_usb_power_resume(&context, &c1_usb_power_default_paths,
                                &c1_usb_power_default_ops)) {
            restore_ok = 1;
            break;
        }
        usleep(250000);
    }
    printf("usb_local_ready=%s\n", restore_ok ? "ok" : "failed");
    show("boot_after", "/proc/sys/kernel/random/boot_id");
    show("success_after", "/sys/power/suspend_stats/success");
    show("uptime_after", "/proc/uptime");
    printf("probe_result=%s\n", suspend_ok && restore_ok ? "ok" : "failed");
    return suspend_ok && restore_ok ? 0 : 1;
}
