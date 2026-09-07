/* Private-translation-unit tests: policy is production wifi.c; every external
 * I/O boundary used by these tests is replaced. No socket, network, process,
 * real clock sleep, or persistent config operation occurs. */
#include "../src/services/wifi.c"

#include <assert.h>

static int64_t fake_time;
static char commands[1024][256];
static size_t command_count;
static int selected_id;
static bool authenticated;
static bool wrong_ssid;
static bool wrong_id;
static bool stale_ip;
static bool dhcp_success;
static bool lose_auth_after_dhcp;
static bool fail_save;
static bool fail_restore;
static bool fail_select_reply;
static bool fail_setup;
static bool foreign_dhcp;
static bool slow_commands;
static bool scan_complete;
static bool empty_scan;
static bool repeat_bss_id;
static int scan_rows;
static int dhcp_calls;
static int save_calls;
static int restore_calls;
static int stop_calls;
static int clear_calls;
static bool previous_network;
static int event_waits;
static bool events_attached;
static bool events_closed;
static c1_wifi_phase cancel_phase;
static int progress_calls;
static int64_t hardware_interface_at;
static bool hardware_module_present;
static bool hardware_load_success;
static bool hardware_load_race;
static int64_t cancel_at_time;
static bool hardware_unblock_success;
static bool hardware_enable_success;
static int hardware_load_calls;
static int hardware_unblock_calls;
static int hardware_enable_calls;
static const char *radio_type;
static const char *radio_hard;
static const char *radio_soft;
static bool radio_write_success;
static bool radio_readback_blocked;
static int radio_writes;
static bool legacy_list;
static bool near_limit_list;
static bool incomplete_list;
static bool managed_observation_available;
static bool factory_observation_available;
static bool observation_timeout;
static int observation_calls;
static int observation_max_budget;
static bool saved_target;
static bool saved_security_mismatch;
static bool saved_key_missing;
static bool saved_lookup_failure;
static bool saved_open;
static bool saved_wep;
static const char *saved_ssid;

static int64_t fake_now(void) { return fake_time; }
static void fake_sleep(long ms) { fake_time += ms; }
static bool fake_ready(void) { return true; }
static bool fake_available(void) { return !foreign_dhcp; }
static bool fake_secure(void) { return true; }
static bool fake_stop(void) { ++stop_calls; return true; }
static bool fake_clear(void) { ++clear_calls; return true; }
static bool fake_backup(config_backup *backup)
{
    backup->bytes = malloc(4U);
    assert(backup->bytes != NULL);
    memcpy(backup->bytes, "old", 4U);
    backup->size = 3U;
    return true;
}
static bool fake_restore(const config_backup *backup)
{
    assert(backup->size == 3U && memcmp(backup->bytes, "old", 3U) == 0);
    ++restore_calls;
    return !fail_restore;
}
static bool fake_ipv4(char *output, size_t capacity)
{
    if (!stale_ip && dhcp_calls == 0) return false;
    snprintf(output, capacity, "192.168.9.12");
    return true;
}
static bool fake_dhcp(int timeout_ms)
{
    assert(timeout_ms > 0);
    /* The primary invariant: never enter DHCP before target authentication. */
    assert(selected_id == 7 || (selected_id == 42 && authenticated && !wrong_ssid && !wrong_id));
    ++dhcp_calls;
    fake_time += 100;
    if (selected_id == 7) return true;
    if (lose_auth_after_dhcp) authenticated = false;
    return dhcp_success;
}
static bool fake_command(const char *text, char *output, size_t capacity, int timeout_ms)
{
    assert(timeout_ms > 0 && timeout_ms <= 750);
    assert(command_count < sizeof(commands) / sizeof(commands[0]));
    snprintf(commands[command_count++], sizeof(commands[0]), "%s", text);
    assert(strcmp(text, "REMOVE_NETWORK all") != 0);
    fake_time += slow_commands ? timeout_ms : 1;
    if (strcmp(text, "PING") == 0) {
        snprintf(output, capacity, "PONG\n");
    } else if (strcmp(text, "STATUS") == 0) {
        if (selected_id == 7) {
            snprintf(output, capacity, "%s", previous_network
                         ? "bssid=00:11:22:33:44:55\nssid=old\nid=7\nwpa_state=COMPLETED\n"
                         : "wpa_state=DISCONNECTED\n");
        } else {
            snprintf(output, capacity, "bssid=00:11:22:33:44:66\nssid=%s\nid=%d\nwpa_state=%s\n",
                     wrong_ssid ? "old" : "target", wrong_id ? 7 : 42,
                     authenticated ? "COMPLETED" : "4WAY_HANDSHAKE");
        }
    } else if (strcmp(text, "LIST_NETWORKS") == 0 ||
               (legacy_list && strncmp(text, "LIST_NETWORKS LAST_ID=", 22U) == 0)) {
        if (near_limit_list) {
            size_t used = (size_t)snprintf(output, capacity, "network id / ssid / bssid / flags\n");
            int id;
            for (id = 0; id < 25; ++id) {
                int octet;
                used += (size_t)snprintf(output + used, capacity - used, "%d\t", id);
                for (octet = 0; octet < 32; ++octet) {
                    used += (size_t)snprintf(output + used, capacity - used, "\\x01");
                }
                used += (size_t)snprintf(output + used, capacity - used, "\tany\t[DISABLED]\n");
            }
            assert(used > capacity - C1_WIFI_LIST_ROW_BOUND && used < capacity);
        } else {
            snprintf(output, capacity, "network id / ssid / bssid / flags\n%s", previous_network
                         ? "7\told\tany\t[CURRENT]\n8\tother\tany\t[DISABLED]\n9\tbackup\tany\t\n" : "");
        }
        if (saved_target && !near_limit_list) {
            size_t used = strlen(output);
            snprintf(output + used, capacity - used, "42\t%s\tany\t[DISABLED]\n", saved_ssid);
        }
        if (incomplete_list) output[strlen(output) - 1U] = '\0';
    } else if (strncmp(text, "LIST_NETWORKS LAST_ID=", 22U) == 0) {
        snprintf(output, capacity, "network id / ssid / bssid / flags\n%s", near_limit_list
                     ? "25\tlast\tany\t[DISABLED]\n" : "");
    } else if (strcmp(text, "GET_NETWORK 42 key_mgmt") == 0) {
        if (saved_lookup_failure) return false;
        snprintf(output, capacity, "%s\n", saved_security_mismatch ? "WPA-EAP" : saved_open ? "NONE" : "WPA-PSK");
    } else if (strncmp(text, "GET_NETWORK 42 wep_key", 22U) == 0) {
        snprintf(output, capacity, "%s\n", saved_wep && text[22] == '2' ? "*" : "FAIL");
    } else if (strcmp(text, "GET_NETWORK 42 psk") == 0) {
        snprintf(output, capacity, "%s\n", saved_key_missing ? "FAIL" : "*");
    } else if (strcmp(text, "ADD_NETWORK") == 0) {
        snprintf(output, capacity, "42\n");
    } else if (strcmp(text, "SELECT_NETWORK 42") == 0) {
        selected_id = 42;
        if (fail_select_reply) return false;
        snprintf(output, capacity, "OK\n");
    } else if (strcmp(text, "SELECT_NETWORK 7") == 0) {
        selected_id = 7;
        snprintf(output, capacity, "OK\n");
    } else if (strncmp(text, "SET_NETWORK", 11U) == 0 && fail_setup) {
        snprintf(output, capacity, "FAIL\n");
    } else if (strcmp(text, "SAVE_CONFIG") == 0) {
        assert(dhcp_calls > 0 && authenticated && selected_id == 42);
        ++save_calls;
        if (fail_save) return false; /* Lost ACK after possible file replacement. */
        snprintf(output, capacity, "OK\n");
    } else if (strcmp(text, "SCAN") == 0) {
        assert(events_attached);
        snprintf(output, capacity, "OK\n");
    } else if (strcmp(text, "BSS FIRST") == 0 || strncmp(text, "BSS NEXT-", 9U) == 0) {
        int id = strcmp(text, "BSS FIRST") == 0 ? 1 : atoi(text + 9) + 1;
        assert(event_waits >= 3 && scan_complete);
        if (repeat_bss_id) id = 1;
        if (empty_scan || id > scan_rows) {
            output[0] = '\0'; /* Real daemons return an empty datagram at end. */
        } else {
            snprintf(output, capacity,
                     "id=%d\nbssid=00:11:22:33:44:%02x\nfreq=2412\nlevel=%d\nflags=[WPA2-PSK-CCMP][ESS]\nssid=net%02d\n",
                     id, id, -100 + id, id);
        }
    } else if (strcmp(text, "SCAN_RESULTS") == 0) {
        assert(false); /* Never trust the daemon's truncated scan table. */
    } else {
        snprintf(output, capacity, "OK\n");
    }
    return true;
}
static bool fake_observe_status(const char *directory, char *output, size_t capacity, int timeout_ms)
{
    bool available;
    assert(timeout_ms > 0 && timeout_ms <= C1_WIFI_OBSERVE_MS);
    ++observation_calls;
    if (timeout_ms > observation_max_budget) observation_max_budget = timeout_ms;
    if (strcmp(directory, C1_WIFI_CTRL_DIR) == 0) available = managed_observation_available;
    else {
        assert(strcmp(directory, C1_WIFI_FACTORY_CTRL_DIR) == 0);
        available = factory_observation_available;
    }
    if (!available || observation_timeout) {
        fake_time += observation_timeout ? timeout_ms : 1;
        return false;
    }
    return fake_command("STATUS", output, capacity, timeout_ms);
}

static int fake_events_open(void)
{
    events_attached = true;
    return 55;
}
static int fake_events_wait(int descriptor, int timeout_ms)
{
    assert(descriptor == 55 && timeout_ms > 0 && timeout_ms <= 250);
    fake_time += timeout_ms;
    ++event_waits;
    return scan_complete && event_waits >= 3 ? 1 : 0;
}
static void fake_events_close(int descriptor)
{
    assert(descriptor == 55);
    events_closed = true;
}
static bool fake_progress(c1_wifi_phase phase, void *context)
{
    assert(context == &progress_calls);
    ++progress_calls;
    return phase != cancel_phase && fake_time < cancel_at_time;
}

static bool fake_hardware_exists(const char *path)
{
    if (strcmp(path, C1_WIFI_MODULE_SYSFS) == 0) return hardware_module_present;
    assert(strcmp(path, C1_WIFI_INTERFACE_SYSFS) == 0);
    return fake_time >= hardware_interface_at;
}
static bool fake_hardware_load(int timeout_ms)
{
    assert(timeout_ms > 0 && timeout_ms <= 4000);
    ++hardware_load_calls;
    fake_time += timeout_ms < 100 ? timeout_ms : 100;
    if (hardware_load_success || hardware_load_race) hardware_module_present = true;
    return hardware_load_success;
}
static bool fake_hardware_unblock(void)
{
    assert(fake_time >= hardware_interface_at);
    ++hardware_unblock_calls;
    return hardware_unblock_success;
}
static bool fake_hardware_enable(bool enabled)
{
    if (enabled) assert(hardware_unblock_calls > 0 && hardware_unblock_success);
    else assert(stop_calls > 0 && clear_calls > 0);
    ++hardware_enable_calls;
    return hardware_enable_success;
}
static bool fake_radio_read(int descriptor, const char *name, char *value, size_t capacity)
{
    const char *text;
    assert(descriptor == 123);
    if (strcmp(name, "type") == 0) text = radio_type;
    else if (strcmp(name, "hard") == 0) text = radio_hard;
    else {
        assert(strcmp(name, "soft") == 0);
        text = radio_soft;
    }
    snprintf(value, capacity, "%s", text);
    return true;
}
static bool fake_radio_clear(int descriptor)
{
    assert(descriptor == 123 && strcmp(radio_type, "wlan") == 0 && strcmp(radio_hard, "0") == 0);
    ++radio_writes;
    if (radio_write_success && !radio_readback_blocked) radio_soft = "0";
    return radio_write_success;
}

static void reset(void)
{
    fake_time = 1000;
    command_count = 0U;
    selected_id = 7;
    authenticated = true;
    wrong_ssid = wrong_id = false;
    stale_ip = true;
    dhcp_success = true;
    lose_auth_after_dhcp = false;
    fail_save = fail_restore = fail_select_reply = fail_setup = foreign_dhcp = slow_commands = false;
    scan_complete = true;
    empty_scan = repeat_bss_id = false;
    scan_rows = 1;
    saved_target = saved_security_mismatch = saved_key_missing = saved_lookup_failure = false;
    saved_open = saved_wep = false;
    saved_ssid = "target";
    dhcp_calls = save_calls = restore_calls = event_waits = progress_calls = stop_calls = clear_calls = 0;
    previous_network = true;
    events_attached = events_closed = false;
    cancel_phase = C1_WIFI_PHASE_IDLE;
    current_state = C1_WIFI_READY;
    current_phase = C1_WIFI_PHASE_IDLE;
    cached_connected_ssid[0] = last_error[0] = '\0';
    cached_network_count = 0U;
    memset(cached_networks, 0, sizeof(cached_networks));
    active_options = NULL;
    operation_deadline = 0;
    operation_cancelled = restoring = false;
    wifi_io.now = fake_now;
    wifi_io.sleep = fake_sleep;
    wifi_io.ready = fake_ready;
    wifi_io.command = fake_command;
    wifi_io.observe_status = fake_observe_status;
    observation.valid = false;
    explicitly_disabled = false;
    managed_observation_available = true;
    factory_observation_available = observation_timeout = false;
    observation_calls = observation_max_budget = 0;
    wifi_io.ipv4 = fake_ipv4;
    wifi_io.dhcp_available = fake_available;
    wifi_io.dhcp = fake_dhcp;
    wifi_io.stop_dhcp = fake_stop;
    wifi_io.clear_address = fake_clear;
    wifi_io.secure_config = fake_secure;
    wifi_io.backup_config = fake_backup;
    wifi_io.restore_config = fake_restore;
    wifi_io.events_open = fake_events_open;
    wifi_io.events_wait = fake_events_wait;
    hardware_interface_at = 1000;
    hardware_module_present = hardware_load_race = false;
    cancel_at_time = INT64_MAX;
    hardware_load_success = hardware_unblock_success = hardware_enable_success = true;
    hardware_load_calls = hardware_unblock_calls = hardware_enable_calls = radio_writes = 0;
    radio_type = "wlan";
    radio_hard = "0";
    radio_soft = "1";
    radio_write_success = true;
    radio_readback_blocked = false;
    legacy_list = near_limit_list = incomplete_list = false;
    hardware_io.exists = fake_hardware_exists;
    hardware_io.load_module = fake_hardware_load;
    hardware_io.unblock = fake_hardware_unblock;
    hardware_io.enable = fake_hardware_enable;
    hardware_io.now = fake_now;
    hardware_io.sleep = fake_sleep;
    radio_io.read = fake_radio_read;
    radio_io.clear_soft = fake_radio_clear;
    wifi_io.events_close = fake_events_close;
}

static bool issued(const char *text)
{
    size_t index;
    for (index = 0U; index < command_count; ++index) {
        if (strcmp(commands[index], text) == 0) return true;
    }
    return false;
}
static c1_status connect_target(c1_wifi_snapshot *snapshot)
{
    return c1_wifi_connect_ex("target", "testpass", C1_WIFI_SECURITY_WPA_PSK, NULL, snapshot);
}
static void expect_rollback(void)
{
    assert(issued("REMOVE_NETWORK 42"));
    assert(issued("SELECT_NETWORK 7"));
    assert(issued("ENABLE_NETWORK 7"));
    assert(issued("DISABLE_NETWORK 8"));
    assert(issued("ENABLE_NETWORK 9"));
    assert(selected_id == 7);
    assert(fake_time <= 1000 + C1_WIFI_CONNECT_MS + C1_WIFI_ROLLBACK_MS + 200);
}

static void test_codecs_status(void)
{
    char decoded[33];
    char encoded[65];
    assert(c1_wifi_decode_scan_ssid(" \\xe6\\xb1\\xa4\\\\x ", decoded, sizeof(decoded)));
    assert(strcmp(decoded, " \xe6\xb1\xa4\\x ") == 0);
    assert(c1_wifi_encode_control_ssid("a\"\\b", encoded, sizeof(encoded)));
    assert(strcmp(encoded, "61225c62") == 0);
    assert(!c1_wifi_decode_scan_ssid("bad\\x0", decoded, sizeof(decoded)));
    assert(!c1_wifi_decode_scan_ssid("bad\\x00", decoded, sizeof(decoded)));
    assert(!c1_wifi_decode_scan_ssid("123456789012345678901234567890123", decoded, sizeof(decoded)));
    assert(!completed_status("bssid=target\nid=42\nwpa_state=COMPLETED\n", "target", 42, decoded, sizeof(decoded)));
    assert(!completed_status("ssid=target\nid=42\nwpa_state=COMPLETED_BAD\n", "target", 42, decoded, sizeof(decoded)));
    assert(!completed_status("ssid=target\nid=7\nwpa_state=COMPLETED\n", "target", 42, decoded, sizeof(decoded)));
    assert(completed_status("bssid=other\nssid=target\nid=42\nwpa_state=COMPLETED\n", "target", 42, decoded, sizeof(decoded)));
    assert(!reply_is("NOT OK\n", "OK"));
    assert(reply_is("OK\n", "OK"));
    assert(argument_is("udhcpc\0-i\0wlan0\0-p\0/run/c1/udhcpc.pid\0", 41U, "-i", "wlan0"));
    assert(!argument_is("udhcpc\0-i\0wlan1\0", 17U, "-i", "wlan0"));
}

static void test_scan_parser(void)
{
    char output[8192];
    size_t used;
    int index;
    reset();
    used = (size_t)snprintf(output, sizeof(output), "bssid / frequency / signal level / flags / ssid\n");
    for (index = 0; index < 12; ++index) {
        used += (size_t)snprintf(output + used, sizeof(output) - used,
                                "00:11:22:33:44:%02x\t2412\t%d\t[ESS]\tnet%02d\n", index, -100 + index, index);
    }
    snprintf(output + used, sizeof(output) - used,
             "00:11:22:33:44:99\t2412\t-10\t[WPA2-PSK-CCMP][ESS]\t shared \\xe6\\xb1\\xa4\n"
             "00:11:22:33:44:98\t2412\t-20\t[ESS]\t shared \\xe6\\xb1\\xa4\n"
             "00:11:22:33:44:97\t2412\t-5\t[WPA2-PSK-CCMP][ESS]\t shared \\xe6\\xb1\\xa4\n"
             "00:11:22:33:44:96\t2412\t-15\t[WEP][ESS]\twep\n"
             "00:11:22:33:44:95\t2412\t-30\t[ESS]\tbad\\x00\n"
             "00:11:22:33:44:94\t2412\t-30\t[ESS]\t\n");
    assert(parse_scan_results(output, true));
    assert(cached_network_count == 8U);
    assert(cached_networks[0].signal_dbm == -5);
    assert(strcmp(cached_networks[0].ssid, " shared \xe6\xb1\xa4") == 0);
    assert(cached_networks[0].security == C1_WIFI_SECURITY_WPA_PSK && cached_networks[0].supported);
    assert(cached_networks[1].security == C1_WIFI_SECURITY_WEP && !cached_networks[1].supported);
    assert(cached_networks[2].security == C1_WIFI_SECURITY_OPEN);
    assert(strcmp(cached_networks[3].ssid, "net11") == 0);
    assert(strcmp(cached_networks[7].ssid, "net07") == 0);
    assert(parse_security("[WPA2-EAP-CCMP][ESS]") == C1_WIFI_SECURITY_ENTERPRISE);
    assert(parse_security("[WPA2-SAE-CCMP][ESS]") == C1_WIFI_SECURITY_SAE);
    assert(parse_security("[WPA2-PSK+SAE-CCMP][ESS]") == C1_WIFI_SECURITY_WPA_PSK);
    assert(parse_security("[OWE-TRANS][ESS]") == C1_WIFI_SECURITY_OWE);
    assert(parse_security("[WAPI-CERT][ESS]") == C1_WIFI_SECURITY_UNKNOWN);
    assert(parse_security("[ESS][WPS]") == C1_WIFI_SECURITY_OPEN);
}

static void test_snapshot(void)
{
    c1_wifi_snapshot snapshot;
    reset();
    current_state = C1_WIFI_ERROR;
    snprintf(last_error, sizeof(last_error), "AUTH FAILED");
    assert(c1_wifi_read_snapshot(&snapshot));
    assert(snapshot.state == C1_WIFI_ERROR && snapshot.ipv4[0] == '\0');
    current_state = C1_WIFI_DISABLED;
    explicitly_disabled = true;
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_DISABLED);
    explicitly_disabled = false;
    current_state = C1_WIFI_CONNECTING;
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_CONNECTING);
    current_state = C1_WIFI_CONNECTED;
    strcpy(cached_connected_ssid, "target");
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_READY);
    selected_id = 42;
    fake_time += C1_WIFI_OBSERVE_CACHE_MS;
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_CONNECTED);
    authenticated = false;
    fake_time += C1_WIFI_OBSERVE_CACHE_MS;
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_READY);
}

static void test_snapshot_discovery_and_budget(void)
{
    c1_wifi_snapshot snapshot;
    c1_wifi_snapshot disabled;
    int index;
    int64_t start;
    int calls;

    reset();
    current_state = C1_WIFI_DISABLED; /* Actual process-start state. */
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_CONNECTED);
    assert(strcmp(snapshot.connected_ssid, "old") == 0 && snapshot.ipv4[0] != '\0');
    assert(observation_calls == 1 && dhcp_calls == 0 && hardware_load_calls == 0);
    start = fake_time;
    for (index = 0; index < 1000; ++index) {
        assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_CONNECTED);
    }
    assert(fake_time == start && observation_calls == 1);

    reset();
    current_state = C1_WIFI_DISABLED;
    managed_observation_available = false;
    factory_observation_available = true;
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_CONNECTED);
    assert(observation_calls == 2 && dhcp_calls == 0 && hardware_load_calls == 0);

    reset();
    current_state = C1_WIFI_DISABLED;
    previous_network = false; /* Address exists, authentication does not. */
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state != C1_WIFI_CONNECTED);
    assert(snapshot.ipv4[0] == '\0' && snapshot.connected_ssid[0] == '\0');

    reset();
    current_state = C1_WIFI_DISABLED;
    stale_ip = false; /* Authenticated, but no usable address. */
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state != C1_WIFI_CONNECTED);

    reset();
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_CONNECTED);
    assert(c1_wifi_disable(&disabled) == C1_STATUS_OK && explicitly_disabled);
    assert(disabled.state == C1_WIFI_DISABLED && disabled.phase == C1_WIFI_PHASE_DONE);
    calls = observation_calls;
    fake_time += C1_WIFI_OBSERVE_CACHE_MS + 1;
    /* Mock STATUS still says completed and retains an IP: explicit off wins. */
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_DISABLED);
    assert(snapshot.ipv4[0] == '\0' && observation_calls == calls);
    reset();
    c1_wifi_adopt_snapshot(&disabled); /* Parent adopts a worker disable result. */
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_DISABLED);
    assert(observation_calls == 0);
    begin_operation(NULL, 1000);
    current_state = C1_WIFI_READY;
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_CONNECTED);

    reset();
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_CONNECTED);
    observation_timeout = true;
    fake_time += C1_WIFI_OBSERVE_CACHE_MS;
    start = fake_time;
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state != C1_WIFI_CONNECTED);
    assert(snapshot.ipv4[0] == '\0' && snapshot.connected_ssid[0] == '\0');
    assert(fake_time - start == C1_WIFI_OBSERVE_MS);
    calls = observation_calls;
    start = fake_time;
    for (index = 0; index < 1000; ++index) {
        assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state != C1_WIFI_CONNECTED);
    }
    assert(fake_time == start && observation_calls == calls);
    observation_timeout = false;
    fake_time += C1_WIFI_OBSERVE_CACHE_MS;
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_CONNECTED);

    reset();
    current_state = C1_WIFI_DISABLED;
    observation_timeout = true;
    start = fake_time;
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_DISABLED);
    assert(fake_time - start == C1_WIFI_OBSERVE_MS && observation_calls == 2);
    assert(observation_max_budget <= C1_WIFI_OBSERVE_MS);

    reset();
    observation_timeout = true;
    begin_operation(NULL, 7);
    current_state = C1_WIFI_READY;
    start = fake_time;
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state != C1_WIFI_CONNECTED);
    assert(fake_time - start == 7); /* Also obey the enclosing operation budget. */

    reset();
    memset(&snapshot, 0, sizeof(snapshot));
    c1_wifi_adopt_snapshot(&snapshot); /* Zero/IDLE is not a disable completion. */
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_CONNECTED);
    snapshot.state = C1_WIFI_ERROR;
    c1_wifi_adopt_snapshot(&snapshot);
    calls = observation_calls;
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.state == C1_WIFI_ERROR);
    assert(observation_calls == calls && snapshot.ipv4[0] == '\0');
}

static void test_connection(void)
{
    c1_wifi_snapshot snapshot;
    reset();
    observation_timeout = true; /* A verified connect needs no extra UI probe. */
    assert(connect_target(&snapshot) == C1_STATUS_OK);
    assert(observation_calls == 0);
    assert(snapshot.state == C1_WIFI_CONNECTED && strcmp(snapshot.connected_ssid, "target") == 0);
    assert(dhcp_calls == 1 && save_calls == 1 && restore_calls == 0);
    assert(issued("SET_NETWORK 42 key_mgmt WPA-PSK") && issued("DISABLE_NETWORK 8"));
    assert(!issued("REMOVE_NETWORK 7") && !issued("REMOVE_NETWORK 42"));

    reset();
    authenticated = false;
    assert(connect_target(&snapshot) == C1_STATUS_IO_ERROR);
    assert(snapshot.state == C1_WIFI_ERROR && snapshot.ipv4[0] == '\0');
    assert(dhcp_calls == 0 && save_calls == 0);
    expect_rollback();

    reset();
    wrong_ssid = true;
    assert(connect_target(&snapshot) == C1_STATUS_IO_ERROR);
    assert(dhcp_calls == 0 && save_calls == 0);
    expect_rollback();

    reset();
    wrong_id = true;
    assert(connect_target(&snapshot) == C1_STATUS_IO_ERROR);
    assert(dhcp_calls == 0 && save_calls == 0);
    expect_rollback();

    reset();
    dhcp_success = false;
    assert(connect_target(&snapshot) == C1_STATUS_IO_ERROR);
    assert(dhcp_calls == 2 && save_calls == 0); /* Renew original lease during rollback. */
    expect_rollback();

    reset();
    lose_auth_after_dhcp = true;
    assert(connect_target(&snapshot) == C1_STATUS_IO_ERROR && save_calls == 0);
    expect_rollback();

    reset();
    fail_save = true;
    assert(connect_target(&snapshot) == C1_STATUS_IO_ERROR);
    assert(save_calls == 1 && restore_calls == 1);
    expect_rollback();

    reset();
    fail_save = true;
    fail_restore = true;
    assert(connect_target(&snapshot) == C1_STATUS_IO_ERROR);
    assert(strstr(snapshot.error, "RESTORE INCOMPLETE") != NULL);
    expect_rollback();

    reset();
    previous_network = false;
    dhcp_success = false;
    assert(connect_target(&snapshot) == C1_STATUS_IO_ERROR);
    assert(issued("DISCONNECT") && issued("REMOVE_NETWORK 42"));
    assert(dhcp_calls == 1 && stop_calls == 1 && clear_calls == 1);

    reset();
    fail_select_reply = true;
    assert(connect_target(&snapshot) == C1_STATUS_IO_ERROR);
    assert(dhcp_calls == 0);
    expect_rollback();

    reset();
    fail_setup = true;
    assert(connect_target(&snapshot) == C1_STATUS_IO_ERROR);
    assert(issued("REMOVE_NETWORK 42") && !issued("SELECT_NETWORK 42") && selected_id == 7);

    reset();
    foreign_dhcp = true;
    assert(connect_target(&snapshot) == C1_STATUS_UNAVAILABLE && command_count == 0U);

    reset();
    slow_commands = true;
    authenticated = false;
    assert(connect_target(&snapshot) == C1_STATUS_IO_ERROR);
    assert(dhcp_calls == 0);
    expect_rollback();
}

static void test_cancellation_and_security(void)
{
    c1_wifi_snapshot snapshot;
    c1_wifi_operation_options options = {fake_progress, &progress_calls};
    c1_wifi_phase phases[] = {C1_WIFI_PHASE_PREPARING, C1_WIFI_PHASE_AUTHENTICATING,
                             C1_WIFI_PHASE_ACQUIRING_ADDRESS, C1_WIFI_PHASE_SAVING};
    size_t index;
    for (index = 0U; index < sizeof(phases) / sizeof(phases[0]); ++index) {
        reset();
        cancel_phase = phases[index];
        assert(c1_wifi_connect_ex("target", "testpass", C1_WIFI_SECURITY_WPA_PSK, &options, &snapshot)
               == C1_STATUS_INTERRUPTED);
        assert(snapshot.phase == C1_WIFI_PHASE_CANCELLED && save_calls == 0 && progress_calls > 0);
        if (index > 0U) expect_rollback();
    }
    reset();
    assert(c1_wifi_connect_ex("target", "testpass", C1_WIFI_SECURITY_SAE, NULL, &snapshot) == C1_STATUS_UNSUPPORTED);
    assert(command_count == 0U && dhcp_calls == 0);
    assert(c1_wifi_connect_ex("target", "bad", C1_WIFI_SECURITY_WPA_PSK, NULL, &snapshot) == C1_STATUS_INVALID_ARGUMENT);
    assert(command_count == 0U);
    assert(c1_wifi_connect_ex("target", "testpass", C1_WIFI_SECURITY_OPEN, NULL, &snapshot) == C1_STATUS_INVALID_ARGUMENT);
    assert(command_count == 0U);
    cached_network_count = 2U;
    strcpy(cached_networks[0].ssid, "target");
    strcpy(cached_networks[1].ssid, "target");
    cached_networks[0].security = C1_WIFI_SECURITY_OPEN;
    cached_networks[1].security = C1_WIFI_SECURITY_WPA_PSK;
    assert(c1_wifi_connect("target", "testpass", &snapshot) == C1_STATUS_UNSUPPORTED);
    assert(command_count == 0U);
    reset();
    assert(c1_wifi_connect_ex("target", "", C1_WIFI_SECURITY_OPEN, NULL, &snapshot) == C1_STATUS_OK);
    assert(issued("SET_NETWORK 42 key_mgmt NONE"));
}

static void test_scan_flow(void)
{
    c1_wifi_snapshot snapshot;
    c1_wifi_operation_options options = {fake_progress, &progress_calls};
    reset();
    assert(c1_wifi_scan(&snapshot) == C1_STATUS_OK);
    assert(snapshot.network_count == 1U && event_waits == 3 && events_closed);
    reset();
    scan_rows = 80;
    assert(c1_wifi_scan(&snapshot) == C1_STATUS_OK);
    assert(snapshot.network_count == 8U && strcmp(snapshot.networks[0].ssid, "net80") == 0);
    assert(strcmp(snapshot.networks[7].ssid, "net73") == 0 && issued("BSS NEXT-80"));
    reset();
    repeat_bss_id = true;
    assert(c1_wifi_scan(&snapshot) == C1_STATUS_IO_ERROR && snapshot.network_count == 0U);
    assert(events_closed);
    reset();
    scan_rows = 80;
    slow_commands = true;
    assert(c1_wifi_scan(&snapshot) == C1_STATUS_IO_ERROR && snapshot.network_count == 0U);
    assert(fake_time <= 1000 + C1_WIFI_SCAN_MS);
    reset();
    empty_scan = true;
    assert(c1_wifi_scan(&snapshot) == C1_STATUS_OK && snapshot.network_count == 0U);
    reset();
    scan_complete = false;
    assert(c1_wifi_scan(&snapshot) == C1_STATUS_IO_ERROR);
    assert(!issued("SCAN_RESULTS") && events_closed && fake_time <= 1000 + C1_WIFI_SCAN_MS);
    reset();
    cancel_phase = C1_WIFI_PHASE_SCANNING;
    assert(c1_wifi_scan_ex(&options, &snapshot) == C1_STATUS_INTERRUPTED);
    assert(!events_attached && !issued("BSS FIRST"));
}

static unsigned int resume_progress_calls;
static bool resume_progress(c1_wifi_phase phase, void *context)
{
    (void)phase;
    assert(context == &resume_progress_calls);
    ++resume_progress_calls;
    return true;
}

static void test_resume(void)
{
    c1_wifi_operation_options options = {resume_progress, &resume_progress_calls};
    reset();
    resume_progress_calls = 0;
    strcpy(cached_connected_ssid, "old");
    assert(c1_wifi_resume_ex(true, true, true, &options) == C1_STATUS_OK);
    assert(resume_progress_calls > 0);
    reset();
    resume_progress_calls = 0;
    strcpy(cached_connected_ssid, "target");
    assert(c1_wifi_resume_ex(true, true, true, &options) == C1_STATUS_UNAVAILABLE);
    assert(resume_progress_calls > 1);
    assert(fake_time <= 1000 + 30000);
    reset();
    resume_progress_calls = 0;
    assert(c1_wifi_resume_ex(false, false, false, &options) == C1_STATUS_OK);
    assert(resume_progress_calls == 0 && command_count == 0U);
    reset();
    strcpy(cached_connected_ssid, "old");
    assert(c1_wifi_resume(true, true, true) == C1_STATUS_OK);
    assert(current_state == C1_WIFI_CONNECTED && dhcp_calls == 1);
    reset();
    strcpy(cached_connected_ssid, "target");
    assert(c1_wifi_resume(true, true, true) == C1_STATUS_UNAVAILABLE);
    assert(current_state == C1_WIFI_ERROR && dhcp_calls == 0);
    assert(fake_time <= 1000 + 30000);
    reset();
    assert(c1_wifi_resume(true, true, false) == C1_STATUS_UNAVAILABLE);
    assert(command_count == 0U && dhcp_calls == 0);
}

static void test_cold_start_hardware(void)
{
    c1_wifi_operation_options options = {fake_progress, &progress_calls};
    reset();
    begin_operation(NULL, 20000);
    assert(prepare_wifi_hardware());
    assert(hardware_load_calls == 0 && hardware_unblock_calls == 1 && hardware_enable_calls == 1);

    reset();
    hardware_interface_at = 1700;
    begin_operation(NULL, 20000);
    assert(prepare_wifi_hardware());
    assert(hardware_load_calls == 1 && hardware_module_present && hardware_enable_calls == 1);
    assert(fake_time == 1700);

    reset();
    hardware_interface_at = 1700;
    hardware_module_present = true;
    begin_operation(NULL, 20000);
    assert(prepare_wifi_hardware() && hardware_load_calls == 0);

    reset();
    hardware_interface_at = 1700;
    hardware_load_success = false;
    hardware_load_race = true;
    begin_operation(NULL, 20000);
    assert(prepare_wifi_hardware() && hardware_load_calls == 1);

    reset();
    hardware_interface_at = INT64_MAX;
    hardware_load_success = false;
    begin_operation(NULL, 20000);
    assert(!prepare_wifi_hardware());
    assert(hardware_load_calls == 1 && hardware_unblock_calls == 0 && hardware_enable_calls == 0);

    reset();
    hardware_interface_at = INT64_MAX;
    begin_operation(NULL, 20000);
    assert(!prepare_wifi_hardware());
    assert(hardware_load_calls == 1 && hardware_unblock_calls == 0);
    assert(fake_time == 1000 + C1_WIFI_HARDWARE_MS);

    reset();
    hardware_interface_at = INT64_MAX;
    hardware_module_present = true;
    begin_operation(NULL, 50);
    assert(!prepare_wifi_hardware());
    assert(fake_time == 1050 && hardware_load_calls == 0 && hardware_enable_calls == 0);

    reset();
    hardware_unblock_success = false;
    begin_operation(NULL, 20000);
    assert(!prepare_wifi_hardware() && hardware_enable_calls == 0);

    reset();
    hardware_enable_success = false;
    begin_operation(NULL, 20000);
    assert(!prepare_wifi_hardware() && hardware_unblock_calls == 1);

    reset();
    cancel_phase = C1_WIFI_PHASE_PREPARING;
    begin_operation(&options, 20000);
    assert(!prepare_wifi_hardware());
    assert(operation_cancelled && hardware_load_calls == 0 && hardware_unblock_calls == 0);

    reset();
    hardware_interface_at = INT64_MAX;
    cancel_at_time = 1400;
    begin_operation(&options, 20000);
    assert(!prepare_wifi_hardware());
    assert(operation_cancelled && hardware_load_calls == 1 && hardware_unblock_calls == 0);
    assert(fake_time == 1400);
}

static void test_radio_scope(void)
{
    assert(rfkill_entry_name("rfkill0") && rfkill_entry_name("rfkill12"));
    assert(!rfkill_entry_name("rfkill") && !rfkill_entry_name("rfkill1/soft"));
    assert(!rfkill_entry_name("../rfkill0") && !rfkill_entry_name("bluetooth0"));
    reset();
    assert(unblock_radio_entry(123) && radio_writes == 1);
    assert(unblock_radio_entry(123) && radio_writes == 1); /* Already unblocked. */
    reset();
    radio_type = "bluetooth";
    assert(!unblock_radio_entry(123) && radio_writes == 0);
    reset();
    radio_hard = "1";
    assert(!unblock_radio_entry(123) && radio_writes == 0);
    reset();
    radio_soft = "unknown";
    assert(!unblock_radio_entry(123) && radio_writes == 0);
    reset();
    radio_write_success = false;
    assert(!unblock_radio_entry(123) && radio_writes == 1);
    reset();
    radio_readback_blocked = true;
    assert(!unblock_radio_entry(123) && radio_writes == 1);
}

static void test_legacy_network_list(void)
{
    connection_backup backup;
    c1_wifi_snapshot snapshot;
    reset();
    legacy_list = true;
    assert(connect_target(&snapshot) == C1_STATUS_OK);
    assert(!issued("LIST_NETWORKS LAST_ID=9"));
    reset();
    incomplete_list = true;
    assert(!capture_networks(&backup));
    assert(!issued("ADD_NETWORK"));
    reset();
    near_limit_list = true;
    assert(capture_networks(&backup));
    assert(backup.count == 26U && issued("LIST_NETWORKS LAST_ID=24"));
    reset();
    near_limit_list = legacy_list = true;
    assert(!capture_networks(&backup));
    assert(issued("LIST_NETWORKS LAST_ID=24"));
    assert(!issued("ADD_NETWORK"));
}

static void test_saved_connections(void)
{
    c1_wifi_snapshot snapshot;
    c1_wifi_operation_options options = {fake_progress, &progress_calls};
    reset();
    saved_target = true;
    saved_ssid = "net01";
    assert(c1_wifi_scan(&snapshot) == C1_STATUS_OK);
    assert(snapshot.network_count == 1U && snapshot.networks[0].saved);
    assert(issued("GET_NETWORK 42 key_mgmt") && issued("GET_NETWORK 42 psk"));
    /* Saved metadata survives the worker-to-parent snapshot adoption. */
    c1_wifi_adopt_snapshot(&snapshot);
    assert(c1_wifi_read_snapshot(&snapshot) && snapshot.networks[0].saved);
    reset();
    saved_target = true;
    saved_ssid = "net01";
    scan_rows = 80;
    assert(c1_wifi_scan(&snapshot) == C1_STATUS_OK && snapshot.network_count == 8U);
    assert(snapshot.networks[0].saved && strcmp(snapshot.networks[0].ssid, "net01") == 0);
    assert(strcmp(snapshot.networks[1].ssid, "net80") == 0);
    reset();
    saved_target = true;
    saved_ssid = "net01";
    saved_security_mismatch = true;
    assert(c1_wifi_scan(&snapshot) == C1_STATUS_OK && !snapshot.networks[0].saved);
    reset();
    saved_target = true;
    saved_ssid = "net01";
    saved_key_missing = true;
    assert(c1_wifi_scan(&snapshot) == C1_STATUS_OK && !snapshot.networks[0].saved);
    reset();
    saved_target = true;
    saved_ssid = "net01";
    saved_lookup_failure = true;
    assert(c1_wifi_scan(&snapshot) != C1_STATUS_OK && snapshot.network_count == 0U);

    reset();
    cached_network_count = 1U;
    snprintf(cached_networks[0].ssid, sizeof(cached_networks[0].ssid), "target");
    cached_networks[0].security = C1_WIFI_SECURITY_WPA_PSK;
    assert(c1_wifi_connect_ex("target", "password123", C1_WIFI_SECURITY_WPA_PSK, NULL, &snapshot) == C1_STATUS_OK);
    assert(snapshot.networks[0].saved && save_calls == 1);

    reset();
    saved_target = true;
    assert(c1_wifi_connect_saved_ex("target", C1_WIFI_SECURITY_WPA_PSK, &options, &snapshot) == C1_STATUS_OK);
    assert(snapshot.state == C1_WIFI_CONNECTED && dhcp_calls == 1);
    assert(issued("SELECT_NETWORK 42") && !issued("ADD_NETWORK") && !issued("REMOVE_NETWORK 42"));
    assert(!issued("DISABLE_NETWORK 42") && issued("DISABLE_NETWORK 8"));
    assert(save_calls == 0 && restore_calls == 0);
    for (size_t i = 0; i < command_count; ++i) assert(strncmp(commands[i], "SET_NETWORK", 11U) != 0);
    reset();
    saved_target = saved_open = true;
    assert(c1_wifi_connect_saved_ex("target", C1_WIFI_SECURITY_OPEN, NULL, &snapshot) == C1_STATUS_OK);
    assert(save_calls == 0 && !issued("ADD_NETWORK"));
    reset();
    saved_target = saved_open = saved_wep = true;
    assert(c1_wifi_connect_saved_ex("target", C1_WIFI_SECURITY_OPEN, NULL, &snapshot) != C1_STATUS_OK);
    assert(!issued("SELECT_NETWORK 42") && dhcp_calls == 0);

    reset();
    assert(c1_wifi_connect_saved_ex("target", C1_WIFI_SECURITY_WPA_PSK, NULL, &snapshot) != C1_STATUS_OK);
    assert(!issued("ADD_NETWORK") && !issued("SELECT_NETWORK 42") && dhcp_calls == 0);
    reset();
    saved_target = true;
    saved_security_mismatch = true;
    assert(c1_wifi_connect_saved_ex("target", C1_WIFI_SECURITY_WPA_PSK, NULL, &snapshot) != C1_STATUS_OK);
    assert(!issued("SELECT_NETWORK 42") && dhcp_calls == 0);

    reset();
    saved_target = true;
    authenticated = false;
    assert(c1_wifi_connect_saved_ex("target", C1_WIFI_SECURITY_WPA_PSK, NULL, &snapshot) != C1_STATUS_OK);
    assert(issued("SELECT_NETWORK 7") && issued("DISABLE_NETWORK 42"));
    assert(!issued("REMOVE_NETWORK 42") && save_calls == 0 && dhcp_calls == 0);
    reset();
    saved_target = true;
    fail_select_reply = true;
    assert(c1_wifi_connect_saved_ex("target", C1_WIFI_SECURITY_WPA_PSK, NULL, &snapshot) != C1_STATUS_OK);
    assert(issued("SELECT_NETWORK 7") && !issued("REMOVE_NETWORK 42"));
    reset();
    saved_target = true;
    dhcp_success = false;
    assert(c1_wifi_connect_saved_ex("target", C1_WIFI_SECURITY_WPA_PSK, NULL, &snapshot) != C1_STATUS_OK);
    assert(issued("SELECT_NETWORK 7") && !issued("REMOVE_NETWORK 42") && dhcp_calls == 2);
    reset();
    saved_target = true;
    cancel_phase = C1_WIFI_PHASE_ACQUIRING_ADDRESS;
    assert(c1_wifi_connect_saved_ex("target", C1_WIFI_SECURITY_WPA_PSK, &options, &snapshot) == C1_STATUS_INTERRUPTED);
    assert(issued("SELECT_NETWORK 7") && !issued("REMOVE_NETWORK 42") && save_calls == 0);
}

int main(void)
{
    test_saved_connections();
    test_codecs_status();
    test_scan_parser();
    test_snapshot();
    test_snapshot_discovery_and_budget();
    test_connection();
    test_cancellation_and_security();
    test_scan_flow();
    test_resume();
    test_cold_start_hardware();
    test_radio_scope();
    test_legacy_network_list();
    puts("Wi-Fi host tests passed (startup discovery, snapshot budgets, connection, scan, rollback, cancellation, hardware, legacy daemon)");
    return 0;
}
