#define _DEFAULT_SOURCE 1

#include "services/wifi.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define C1_WIFI_RUN_DIR "/run/c1"
#define C1_WIFI_CTRL_DIR "/run/c1/wpa_ctrl"
#define C1_WIFI_FACTORY_CTRL_DIR "/var/run/wpa_supplicant"
#define C1_WIFI_DATA_DIR "/usr/data/c1/wifi"
#define C1_WIFI_CONFIG "/usr/data/c1/wifi/wpa_supplicant.conf"
#define C1_WIFI_PID "/run/c1/wpa_supplicant.pid"
#define C1_WIFI_DHCP_PID "/run/c1/udhcpc.pid"
#define C1_WIFI_OUTPUT_CAPACITY 4096U

static c1_wifi_state current_state = C1_WIFI_DISABLED;
static c1_wifi_network cached_networks[C1_WIFI_MAX_NETWORKS];
static size_t cached_network_count;
static char cached_connected_ssid[C1_WIFI_SSID_CAPACITY];
static char last_error[C1_WIFI_ERROR_CAPACITY];

static void secure_clear(void *memory, size_t size)
{
    volatile unsigned char *cursor = memory;

    while (size-- > 0U) {
        *cursor++ = 0U;
    }
}

static void set_error(const char *message)
{
    current_state = C1_WIFI_ERROR;
    snprintf(last_error, sizeof(last_error), "%s", message);
}

static void sleep_milliseconds(long milliseconds)
{
    struct timespec interval;

    interval.tv_sec = milliseconds / 1000L;
    interval.tv_nsec = (milliseconds % 1000L) * 1000000L;
    while (nanosleep(&interval, &interval) != 0 && errno == EINTR) {
    }
}

static bool wait_child(pid_t child, unsigned int timeout_seconds)
{
    unsigned int tick;

    for (tick = 0U; tick < timeout_seconds * 10U; ++tick) {
        int status;
        pid_t result = waitpid(child, &status, WNOHANG);

        if (result == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
        sleep_milliseconds(100L);
    }
    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
    return false;
}

static bool run_program(const char *path, char *const argv[], unsigned int timeout_seconds)
{
    pid_t child = fork();

    if (child < 0) {
        return false;
    }
    if (child == 0) {
        int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);

        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) {
                close(null_fd);
            }
        }
        execv(path, argv);
        _exit(127);
    }
    return wait_child(child, timeout_seconds);
}

static bool run_wpa_command_timeout(const char *control_dir,
                                    const char *command,
                                    char *output,
                                    size_t capacity,
                                    int timeout_ms)
{
    static unsigned int sequence;
    struct sockaddr_un local;
    struct sockaddr_un remote;
    struct pollfd readable;
    int descriptor;
    ssize_t count;
    bool success = false;

    if (control_dir == NULL || command == NULL || strchr(command, '\n') != NULL ||
        strlen(control_dir) + sizeof("/wlan0") > sizeof(remote.sun_path) ||
        capacity < 2U || timeout_ms < 0) {
        return false;
    }
    descriptor = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (descriptor < 0) {
        return false;
    }
    fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    snprintf(local.sun_path,
             sizeof(local.sun_path),
             C1_WIFI_RUN_DIR "/wpa-client-%ld-%u",
             (long)getpid(),
             sequence++);
    unlink(local.sun_path);
    if (bind(descriptor, (struct sockaddr *)&local, sizeof(local)) != 0) {
        close(descriptor);
        return false;
    }
    chmod(local.sun_path, 0600);
    memset(&remote, 0, sizeof(remote));
    remote.sun_family = AF_UNIX;
    snprintf(remote.sun_path, sizeof(remote.sun_path), "%s/wlan0", control_dir);
    if (connect(descriptor, (struct sockaddr *)&remote, sizeof(remote)) != 0 ||
        send(descriptor, command, strlen(command), 0) != (ssize_t)strlen(command)) {
        goto finished;
    }
    readable.fd = descriptor;
    readable.events = POLLIN;
    readable.revents = 0;
    if (poll(&readable, 1U, timeout_ms) <= 0 || (readable.revents & POLLIN) == 0) {
        goto finished;
    }
    count = recv(descriptor, output, capacity - 1U, 0);
    if (count <= 0) {
        goto finished;
    }
    output[count] = '\0';
    success = true;

finished:
    close(descriptor);
    unlink(local.sun_path);
    return success;
}

static bool run_wpa_command(const char *command, char *output, size_t capacity)
{
    return run_wpa_command_timeout(C1_WIFI_CTRL_DIR, command, output, capacity, 5000);
}

static bool interface_ipv4(char *value, size_t capacity)
{
    struct ifreq request;
    int descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    bool found = false;

    if (descriptor < 0) {
        return false;
    }
    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", "wlan0");
    if (ioctl(descriptor, SIOCGIFADDR, &request) == 0) {
        struct sockaddr_in *address = (struct sockaddr_in *)&request.ifr_addr;
        found = inet_ntop(AF_INET, &address->sin_addr, value, (socklen_t)capacity) != NULL;
    }
    close(descriptor);
    return found;
}

static bool ensure_directory(const char *path, mode_t mode)
{
    return mkdir(path, mode) == 0 || errno == EEXIST;
}

static bool write_runtime_config(void)
{
    static const char content[] =
        "ctrl_interface=" C1_WIFI_CTRL_DIR "\n"
        "update_config=1\n"
        "ap_scan=1\n";
    int descriptor;
    ssize_t count;

    if (!ensure_directory(C1_WIFI_RUN_DIR, 0700) || !ensure_directory(C1_WIFI_CTRL_DIR, 0700) ||
        !ensure_directory(C1_WIFI_DATA_DIR, 0700)) {
        return false;
    }
    if (access(C1_WIFI_CONFIG, F_OK) == 0) {
        return chmod(C1_WIFI_CONFIG, 0600) == 0;
    }
    descriptor = open(C1_WIFI_CONFIG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }
    count = write(descriptor, content, sizeof(content) - 1U);
    if (count != (ssize_t)(sizeof(content) - 1U) || fsync(descriptor) != 0 || close(descriptor) != 0) {
        if (count == (ssize_t)(sizeof(content) - 1U)) {
            close(descriptor);
        }
        return false;
    }
    return true;
}

static bool control_ready(void)
{
    char output[256];

    return run_wpa_command_timeout(C1_WIFI_CTRL_DIR,
                                   "PING",
                                   output,
                                   sizeof(output),
                                   500) &&
           strstr(output, "PONG") != NULL;
}

static bool factory_control_ready(void)
{
    char output[256];

    return run_wpa_command_timeout(C1_WIFI_FACTORY_CTRL_DIR,
                                   "PING",
                                   output,
                                   sizeof(output),
                                   500) &&
           strstr(output, "PONG") != NULL;
}

static void terminate_processes_named(const char *name)
{
    DIR *processes = opendir("/proc");
    struct dirent *entry;

    if (processes == NULL) {
        return;
    }
    while ((entry = readdir(processes)) != NULL) {
        char *end = NULL;
        long value = strtol(entry->d_name, &end, 10);

        if (entry->d_name[0] == '\0' || end == entry->d_name || *end != '\0' || value <= 1L) {
            continue;
        }
        {
            char path[64];
            char comm[64];
            FILE *stream;

            snprintf(path, sizeof(path), "/proc/%ld/comm", value);
            stream = fopen(path, "r");
            if (stream != NULL && fgets(comm, sizeof(comm), stream) != NULL) {
                size_t length = strlen(comm);
                while (length > 0U && (comm[length - 1U] == '\n' || comm[length - 1U] == '\r')) {
                    comm[--length] = '\0';
                }
                if (strcmp(comm, name) == 0) {
                    kill((pid_t)value, SIGTERM);
                }
            }
            if (stream != NULL) {
                fclose(stream);
            }
        }
    }
    closedir(processes);
    sleep_milliseconds(300L);
}

static bool ensure_wifi_ready(void)
{
    unsigned int tick;

    if (control_ready()) {
        current_state = C1_WIFI_READY;
        return true;
    }
    if (access("/sys/class/net/wlan0", F_OK) != 0) {
        char *const argv[] = {"wifi_up.sh", NULL};

        if (!run_program("/bin/wifi_up.sh", argv, 15U)) {
            set_error("WI-FI HARDWARE FAILED");
            return false;
        }
    }
    terminate_processes_named("udhcpc");
    terminate_processes_named("wpa_supplicant");
    if (!write_runtime_config()) {
        set_error("RUNTIME CONFIG FAILED");
        return false;
    }
    {
        char *const argv[] = {
            "wpa_supplicant", "-B", "-D", "nl80211", "-i", "wlan0", "-c", C1_WIFI_CONFIG,
            "-P", C1_WIFI_PID, NULL
        };

        if (!run_program("/usr/sbin/wpa_supplicant", argv, 5U) &&
            !run_program("/sbin/wpa_supplicant", argv, 5U)) {
            set_error("SUPPLICANT FAILED");
            return false;
        }
    }
    for (tick = 0U; tick < 50U; ++tick) {
        if (control_ready()) {
            current_state = C1_WIFI_READY;
            last_error[0] = '\0';
            return true;
        }
        sleep_milliseconds(100L);
    }
    set_error("CONTROL SOCKET TIMEOUT");
    return false;
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

bool c1_wifi_decode_scan_ssid(const char *encoded, char *decoded, size_t capacity)
{
    size_t input_index = 0U;
    size_t output_index = 0U;

    if (encoded == NULL || decoded == NULL || capacity == 0U) {
        return false;
    }
    while (encoded[input_index] != '\0') {
        unsigned char value = (unsigned char)encoded[input_index++];

        if (value == '\\') {
            char escape = encoded[input_index++];

            if (escape == '\0') {
                return false;
            }
            if (escape == 'x') {
                int high;
                int low;

                if (encoded[input_index] == '\0' || encoded[input_index + 1U] == '\0') {
                    return false;
                }
                high = hex_value(encoded[input_index]);
                low = hex_value(encoded[input_index + 1U]);
                if (high < 0 || low < 0) {
                    return false;
                }
                value = (unsigned char)((high << 4) | low);
                input_index += 2U;
            } else {
                switch (escape) {
                case '\\':
                case '"':
                    value = (unsigned char)escape;
                    break;
                case 'n':
                    value = '\n';
                    break;
                case 'r':
                    value = '\r';
                    break;
                case 't':
                    value = '\t';
                    break;
                case 'e':
                    value = 27U;
                    break;
                default:
                    return false;
                }
            }
        }
        if (value == 0U || output_index + 1U >= capacity) {
            return false;
        }
        decoded[output_index++] = (char)value;
    }
    decoded[output_index] = '\0';
    return output_index > 0U;
}

bool c1_wifi_encode_control_ssid(const char *ssid, char *encoded, size_t capacity)
{
    static const char digits[] = "0123456789abcdef";
    size_t index;
    size_t length;

    if (ssid == NULL || encoded == NULL) {
        return false;
    }
    length = strlen(ssid);
    if (length == 0U || length > 32U || capacity < length * 2U + 1U) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)ssid[index];

        encoded[index * 2U] = digits[value >> 4U];
        encoded[index * 2U + 1U] = digits[value & 0x0fU];
    }
    encoded[length * 2U] = '\0';
    return true;
}

static int network_compare(const void *left, const void *right)
{
    const c1_wifi_network *a = left;
    const c1_wifi_network *b = right;

    return b->signal_dbm - a->signal_dbm;
}

static void parse_scan_results(char *output)
{
    char *save = NULL;
    char *line = strtok_r(output, "\r\n", &save);

    cached_network_count = 0U;
    while (line != NULL) {
        char bssid[24];
        char flags[128];
        char encoded_ssid[C1_WIFI_SSID_CAPACITY * 4U];
        char ssid[C1_WIFI_SSID_CAPACITY];
        int frequency;
        int signal;

        if (sscanf(line, "%23s\t%d\t%d\t%127s\t%131[^\r\n]", bssid, &frequency, &signal, flags, encoded_ssid) == 5 &&
            strchr(bssid, ':') != NULL &&
            c1_wifi_decode_scan_ssid(encoded_ssid, ssid, sizeof(ssid))) {
            size_t index;
            bool duplicate = false;

            (void)frequency;
            for (index = 0U; index < cached_network_count; ++index) {
                if (strcmp(cached_networks[index].ssid, ssid) == 0) {
                    if (signal > cached_networks[index].signal_dbm) {
                        cached_networks[index].signal_dbm = signal;
                    }
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate && cached_network_count < C1_WIFI_MAX_NETWORKS) {
                c1_wifi_network *network = &cached_networks[cached_network_count++];

                snprintf(network->ssid, sizeof(network->ssid), "%s", ssid);
                network->signal_dbm = signal;
                network->secured = strstr(flags, "WPA") != NULL || strstr(flags, "WEP") != NULL;
            }
        }
        line = strtok_r(NULL, "\r\n", &save);
    }
    qsort(cached_networks, cached_network_count, sizeof(cached_networks[0]), network_compare);
}

static bool quote_wpa_value(const char *value, char *quoted, size_t capacity)
{
    size_t input_index;
    size_t output_index = 0U;
    size_t length = strlen(value);

    if (length > 63U || capacity < 3U) {
        return false;
    }
    quoted[output_index++] = '"';
    for (input_index = 0U; input_index < length; ++input_index) {
        unsigned char character = (unsigned char)value[input_index];

        if (character < 32U || character > 126U) {
            return false;
        }
        if (character == '"' || character == '\\') {
            if (output_index + 2U >= capacity) {
                return false;
            }
            quoted[output_index++] = '\\';
        } else if (output_index + 1U >= capacity) {
            return false;
        }
        quoted[output_index++] = (char)character;
    }
    quoted[output_index++] = '"';
    quoted[output_index] = '\0';
    return true;
}

bool c1_wifi_read_snapshot(c1_wifi_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = current_state;
    snapshot->network_count = cached_network_count;
    memcpy(snapshot->networks, cached_networks, sizeof(cached_networks));
    snprintf(snapshot->connected_ssid,
             sizeof(snapshot->connected_ssid),
             "%s",
             cached_connected_ssid);
    snprintf(snapshot->error, sizeof(snapshot->error), "%s", last_error);
    if (interface_ipv4(snapshot->ipv4, sizeof(snapshot->ipv4))) {
        snapshot->state = C1_WIFI_CONNECTED;
    }
    return true;
}

void c1_wifi_adopt_snapshot(const c1_wifi_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    current_state = snapshot->state;
    cached_network_count = snapshot->network_count <= C1_WIFI_MAX_NETWORKS
                               ? snapshot->network_count
                               : C1_WIFI_MAX_NETWORKS;
    memcpy(cached_networks, snapshot->networks, sizeof(cached_networks));
    snprintf(cached_connected_ssid,
             sizeof(cached_connected_ssid),
             "%s",
             snapshot->connected_ssid);
    snprintf(last_error, sizeof(last_error), "%s", snapshot->error);
}

c1_status c1_wifi_scan(c1_wifi_snapshot *snapshot)
{
    char output[C1_WIFI_OUTPUT_CAPACITY];
    unsigned int tick;

    current_state = C1_WIFI_SCANNING;
    last_error[0] = '\0';
    if (!ensure_wifi_ready()) {
        c1_wifi_read_snapshot(snapshot);
        return C1_STATUS_UNAVAILABLE;
    }
    current_state = C1_WIFI_SCANNING;
    if (!run_wpa_command("SCAN", output, sizeof(output)) || strstr(output, "OK") == NULL) {
        set_error("SCAN START FAILED");
        c1_wifi_read_snapshot(snapshot);
        return C1_STATUS_IO_ERROR;
    }
    for (tick = 0U; tick < 30U; ++tick) {
        sleep_milliseconds(200L);
        if (run_wpa_command("SCAN_RESULTS", output, sizeof(output))) {
            parse_scan_results(output);
            if (cached_network_count > 0U) {
                current_state = C1_WIFI_READY;
                c1_wifi_read_snapshot(snapshot);
                return C1_STATUS_OK;
            }
        }
    }
    set_error("NO NETWORKS FOUND");
    c1_wifi_read_snapshot(snapshot);
    return C1_STATUS_UNAVAILABLE;
}

c1_status c1_wifi_connect(const char *ssid, const char *password, c1_wifi_snapshot *snapshot)
{
    char output[C1_WIFI_OUTPUT_CAPACITY];
    char encoded_ssid[C1_WIFI_SSID_CAPACITY * 2U];
    char quoted_password[132];
    char command[224];
    char network_id[16];
    char *number;
    size_t password_length;
    unsigned int tick;
    c1_status result = C1_STATUS_IO_ERROR;

    if (ssid == NULL || password == NULL ||
        !c1_wifi_encode_control_ssid(ssid, encoded_ssid, sizeof(encoded_ssid))) {
        set_error("INVALID NETWORK");
        c1_wifi_read_snapshot(snapshot);
        return C1_STATUS_INVALID_ARGUMENT;
    }
    password_length = strlen(password);
    if (password_length != 0U && (password_length < 8U || password_length > 63U)) {
        set_error("PASSWORD MUST BE 8-63");
        c1_wifi_read_snapshot(snapshot);
        return C1_STATUS_INVALID_ARGUMENT;
    }
    current_state = C1_WIFI_CONNECTING;
    if (!ensure_wifi_ready() || !run_wpa_command("REMOVE_NETWORK all", output, sizeof(output)) ||
        !run_wpa_command("ADD_NETWORK", output, sizeof(output))) {
        set_error("NETWORK SETUP FAILED");
        goto finished;
    }
    number = output;
    while (*number != '\0' && (*number < '0' || *number > '9')) {
        ++number;
    }
    if (*number == '\0' || sscanf(number, "%15[0-9]", network_id) != 1) {
        set_error("NETWORK ID FAILED");
        goto finished;
    }
    snprintf(command, sizeof(command), "SET_NETWORK %s ssid %s", network_id, encoded_ssid);
    if (!run_wpa_command(command, output, sizeof(output)) || strstr(output, "OK") == NULL) {
        set_error("SSID REJECTED");
        goto finished;
    }
    if (password_length == 0U) {
        snprintf(command, sizeof(command), "SET_NETWORK %s key_mgmt NONE", network_id);
    } else {
        if (!quote_wpa_value(password, quoted_password, sizeof(quoted_password))) {
            set_error("INVALID PASSWORD");
            goto finished;
        }
        snprintf(command, sizeof(command), "SET_NETWORK %s psk %s", network_id, quoted_password);
    }
    if (!run_wpa_command(command, output, sizeof(output)) || strstr(output, "OK") == NULL) {
        set_error("CREDENTIAL REJECTED");
        goto finished;
    }
    snprintf(command, sizeof(command), "ENABLE_NETWORK %s", network_id);
    if (!run_wpa_command(command, output, sizeof(output)) || strstr(output, "OK") == NULL ||
        !run_wpa_command("SAVE_CONFIG", output, sizeof(output)) || strstr(output, "OK") == NULL ||
        chmod(C1_WIFI_CONFIG, 0600) != 0) {
        set_error("CONNECT START FAILED");
        goto finished;
    }
    for (tick = 0U; tick < 75U; ++tick) {
        sleep_milliseconds(200L);
        if (run_wpa_command("STATUS", output, sizeof(output)) && strstr(output, "wpa_state=COMPLETED") != NULL) {
            char *const argv[] = {
                "udhcpc", "-R", "-S", "-b", "-t", "10", "-T", "2", "-i", "wlan0",
                "-p", C1_WIFI_DHCP_PID, "-x", "hostname:C1-Slim", NULL
            };

            terminate_processes_named("udhcpc");
            sleep_milliseconds(300L);
            if (!run_program("/sbin/udhcpc", argv, 5U)) {
                run_program("/bin/udhcpc", argv, 5U);
            }
            break;
        }
    }
    for (tick = 0U; tick < 100U; ++tick) {
        char ip[C1_WIFI_IP_CAPACITY];

        if (interface_ipv4(ip, sizeof(ip))) {
            current_state = C1_WIFI_CONNECTED;
            snprintf(cached_connected_ssid, sizeof(cached_connected_ssid), "%s", ssid);
            snprintf(last_error, sizeof(last_error), "CONNECTED TO %.32s", ssid);
            result = C1_STATUS_OK;
            goto finished;
        }
        sleep_milliseconds(200L);
    }
    set_error("CONNECTION TIMED OUT");

finished:
    secure_clear(quoted_password, sizeof(quoted_password));
    secure_clear(command, sizeof(command));
    secure_clear(output, sizeof(output));
    c1_wifi_read_snapshot(snapshot);
    return result;
}

c1_status c1_wifi_pause(bool *was_enabled, bool *was_connected, bool *was_managed)
{
    char saved_ssid[C1_WIFI_SSID_CAPACITY];
    char output[C1_WIFI_OUTPUT_CAPACITY];
    char ip[C1_WIFI_IP_CAPACITY];
    bool managed_control;
    bool factory_control;
    c1_status status;

    if (was_enabled == NULL || was_connected == NULL || was_managed == NULL) {
        return C1_STATUS_INVALID_ARGUMENT;
    }
    managed_control = control_ready();
    factory_control = !managed_control && factory_control_ready();
    *was_managed = managed_control || access(C1_WIFI_PID, F_OK) == 0;
    *was_enabled = current_state != C1_WIFI_DISABLED || managed_control || factory_control ||
                   access("/sys/class/net/wlan0", F_OK) == 0;
    *was_connected = current_state == C1_WIFI_CONNECTED ||
                     cached_connected_ssid[0] != '\0' ||
                     interface_ipv4(ip, sizeof(ip));
    if ((managed_control || factory_control) &&
        run_wpa_command_timeout(managed_control ? C1_WIFI_CTRL_DIR : C1_WIFI_FACTORY_CTRL_DIR,
                                "STATUS",
                                output,
                                sizeof(output),
                                1000) &&
        strstr(output, "wpa_state=COMPLETED") != NULL) {
        char *ssid = strstr(output, "ssid=");

        while (ssid != NULL && ssid != output && ssid[-1] != '\n') {
            ssid = strstr(ssid + 5U, "ssid=");
        }
        *was_connected = true;
        if (ssid != NULL) {
            size_t length;

            ssid += 5U;
            length = strcspn(ssid, "\r\n");
            if (length >= sizeof(cached_connected_ssid)) {
                length = sizeof(cached_connected_ssid) - 1U;
            }
            memcpy(cached_connected_ssid, ssid, length);
            cached_connected_ssid[length] = '\0';
        }
    }
    if (!*was_enabled) {
        return C1_STATUS_OK;
    }
    snprintf(saved_ssid, sizeof(saved_ssid), "%s", cached_connected_ssid);
    status = c1_wifi_disable(NULL);
    if (status == C1_STATUS_OK) {
        snprintf(cached_connected_ssid, sizeof(cached_connected_ssid), "%s", saved_ssid);
    }
    return status;
}

c1_status c1_wifi_resume(bool was_enabled, bool was_connected, bool was_managed)
{
    char output[C1_WIFI_OUTPUT_CAPACITY];
    unsigned int tick;

    if (!was_enabled) {
        return C1_STATUS_OK;
    }
    if (!was_managed) {
        char *const argv[] = {"wifi_up.sh", NULL};

        if (!run_program("/bin/wifi_up.sh", argv, 20U)) {
            set_error("FACTORY WI-FI RESUME FAILED");
            return C1_STATUS_UNAVAILABLE;
        }
        for (tick = 0U; tick < 20U && !factory_control_ready(); ++tick) {
            sleep_milliseconds(100L);
        }
        if (!factory_control_ready()) {
            set_error("FACTORY WI-FI CONTROL TIMEOUT");
            return C1_STATUS_UNAVAILABLE;
        }
        if (!was_connected) {
            (void)run_wpa_command_timeout(C1_WIFI_FACTORY_CTRL_DIR,
                                          "DISCONNECT",
                                          output,
                                          sizeof(output),
                                          1000);
            terminate_processes_named("udhcpc");
            current_state = C1_WIFI_READY;
            last_error[0] = '\0';
            return C1_STATUS_OK;
        }
        for (tick = 0U; tick < 100U; ++tick) {
            char ip[C1_WIFI_IP_CAPACITY];

            if (interface_ipv4(ip, sizeof(ip))) {
                current_state = C1_WIFI_CONNECTED;
                last_error[0] = '\0';
                return C1_STATUS_OK;
            }
            sleep_milliseconds(200L);
        }
        set_error("FACTORY WI-FI RESUME TIMED OUT");
        return C1_STATUS_UNAVAILABLE;
    }
    if (!ensure_wifi_ready()) {
        return C1_STATUS_UNAVAILABLE;
    }
    if (!was_connected) {
        current_state = C1_WIFI_READY;
        return C1_STATUS_OK;
    }
    for (tick = 0U; tick < 30U; ++tick) {
        if (run_wpa_command_timeout(C1_WIFI_CTRL_DIR,
                                    "STATUS",
                                    output,
                                    sizeof(output),
                                    500) &&
            strstr(output, "wpa_state=COMPLETED") != NULL) {
            char *const argv[] = {
                "udhcpc", "-R", "-S", "-b", "-t", "10", "-T", "2", "-i", "wlan0",
                "-p", C1_WIFI_DHCP_PID, "-x", "hostname:C1-Slim", NULL
            };

            terminate_processes_named("udhcpc");
            if (!run_program("/sbin/udhcpc", argv, 5U)) {
                (void)run_program("/bin/udhcpc", argv, 5U);
            }
            current_state = C1_WIFI_CONNECTED;
            last_error[0] = '\0';
            return C1_STATUS_OK;
        }
        sleep_milliseconds(100L);
    }
    set_error("WI-FI RESUME TIMED OUT");
    return C1_STATUS_UNAVAILABLE;
}

c1_status c1_wifi_disable(c1_wifi_snapshot *snapshot)
{
    char output[256];
    char *const argv[] = {"wifi_down.sh", NULL};

    run_wpa_command("TERMINATE", output, sizeof(output));
    if (!run_program("/bin/wifi_down.sh", argv, 10U)) {
        set_error("WI-FI STOP FAILED");
        c1_wifi_read_snapshot(snapshot);
        return C1_STATUS_IO_ERROR;
    }
    current_state = C1_WIFI_DISABLED;
    cached_network_count = 0U;
    memset(cached_networks, 0, sizeof(cached_networks));
    cached_connected_ssid[0] = '\0';
    last_error[0] = '\0';
    unlink(C1_WIFI_PID);
    unlink(C1_WIFI_DHCP_PID);
    c1_wifi_read_snapshot(snapshot);
    return C1_STATUS_OK;
}