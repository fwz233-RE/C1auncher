#ifndef C1_SERVICES_WIFI_H
#define C1_SERVICES_WIFI_H

#include "core/status.h"

#include <stdbool.h>
#include <stddef.h>

#define C1_WIFI_MAX_NETWORKS 8U
#define C1_WIFI_SSID_CAPACITY 33U
#define C1_WIFI_IP_CAPACITY 16U
#define C1_WIFI_ERROR_CAPACITY 48U

typedef enum {
    C1_WIFI_DISABLED = 0,
    C1_WIFI_READY,
    C1_WIFI_SCANNING,
    C1_WIFI_CONNECTING,
    C1_WIFI_CONNECTED,
    C1_WIFI_ERROR
} c1_wifi_state;

typedef struct {
    char ssid[C1_WIFI_SSID_CAPACITY];
    int signal_dbm;
    bool secured;
} c1_wifi_network;

typedef struct {
    c1_wifi_state state;
    c1_wifi_network networks[C1_WIFI_MAX_NETWORKS];
    size_t network_count;
    char connected_ssid[C1_WIFI_SSID_CAPACITY];
    char ipv4[C1_WIFI_IP_CAPACITY];
    char error[C1_WIFI_ERROR_CAPACITY];
} c1_wifi_snapshot;

bool c1_wifi_read_snapshot(c1_wifi_snapshot *snapshot);
c1_status c1_wifi_scan(c1_wifi_snapshot *snapshot);
c1_status c1_wifi_connect(const char *ssid, const char *password, c1_wifi_snapshot *snapshot);
c1_status c1_wifi_disable(c1_wifi_snapshot *snapshot);
c1_status c1_wifi_pause(bool *was_enabled, bool *was_connected, bool *was_managed);
c1_status c1_wifi_resume(bool was_enabled, bool was_connected, bool was_managed);
void c1_wifi_adopt_snapshot(const c1_wifi_snapshot *snapshot);

#endif