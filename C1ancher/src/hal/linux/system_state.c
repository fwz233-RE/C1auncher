#define _DEFAULT_SOURCE 1

#include "hal/linux/system_state.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define C1_SYSTEM_VALUE_CAPACITY 64U

static bool read_line(const char *path, char *value, size_t capacity)
{
    FILE *stream = fopen(path, "r");
    size_t length;

    if (stream == NULL || fgets(value, (int)capacity, stream) == NULL) {
        if (stream != NULL) {
            fclose(stream);
        }
        return false;
    }
    if (fclose(stream) != 0) {
        return false;
    }

    length = strlen(value);
    while (length > 0U && (value[length - 1U] == '\n' || value[length - 1U] == '\r')) {
        value[--length] = '\0';
    }
    return true;
}

static bool read_integer(const char *path, int64_t *value)
{
    char text[C1_SYSTEM_VALUE_CAPACITY];
    char *end = NULL;
    long long parsed;

    if (!read_line(path, text, sizeof(text))) {
        return false;
    }
    errno = 0;
    parsed = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = (int64_t)parsed;
    return true;
}

static bool read_online(const char *path, bool *online)
{
    int64_t value;

    if (online == NULL || !read_integer(path, &value) || (value != 0 && value != 1)) {
        return false;
    }
    *online = value == 1;
    return true;
}

bool c1_linux_external_power_read(bool *online)
{
    bool ac_online = false;
    bool usb_online = false;
    bool ac_known;
    bool usb_known;

    if (online == NULL) {
        return false;
    }
    ac_known = read_online("/sys/class/power_supply/ac/online", &ac_online);
    usb_known = read_online("/sys/class/power_supply/usb/online", &usb_online);
    *online = (ac_known && ac_online) || (usb_known && usb_online);
    return ac_known && usb_known;
}

static bool interface_ipv4(const char *interface_name, char *value, size_t capacity)
{
    struct ifreq request;
    int descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    bool found = false;

    if (descriptor < 0) {
        return false;
    }
    fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    memset(&request, 0, sizeof(request));
    if (strlen(interface_name) < sizeof(request.ifr_name)) {
        struct sockaddr_in *address;

        memcpy(request.ifr_name, interface_name, strlen(interface_name) + 1U);
        if (ioctl(descriptor, SIOCGIFADDR, &request) == 0) {
            address = (struct sockaddr_in *)&request.ifr_addr;
            found = inet_ntop(AF_INET,
                              &address->sin_addr,
                              value,
                              (socklen_t)capacity) != NULL;
        }
    }
    close(descriptor);
    return found;
}

bool c1_linux_battery_read(uint32_t *percent)
{
    int64_t value;
    if (!percent || !read_integer("/sys/class/power_supply/battery/capacity", &value) ||
        value < 0 || value > 100) return false;
    *percent = (uint32_t)value;
    return true;
}

bool c1_linux_system_status_read(c1_ui_status *status)
{
    int64_t carrier = -1;
    struct timespec current;
    struct tm local;

    if (status == NULL) {
        return false;
    }
    memset(status, 0, sizeof(*status));

    status->battery_available = c1_linux_battery_read(&status->battery_percent);
    status->external_power_known = c1_linux_external_power_read(&status->external_power);

    read_integer("/sys/class/net/wlan0/carrier", &carrier);
    status->wifi_connected = carrier == 1 &&
                             interface_ipv4("wlan0",
                                            status->wifi_ipv4,
                                            sizeof(status->wifi_ipv4));

    status->time_available = clock_gettime(CLOCK_REALTIME, &current) == 0 &&
                             localtime_r(&current.tv_sec, &local) != NULL;
    if (status->time_available) {
        status->hour = (uint32_t)local.tm_hour;
        status->minute = (uint32_t)local.tm_min;
    }
    return true;
}

