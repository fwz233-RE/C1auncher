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
#define C1_WIFI_DATA_DIR "/usr/data/c1/wifi"
#define C1_WIFI_CONFIG "/usr/data/c1/wifi/wpa_supplicant.conf"
#define C1_WIFI_PID "/run/c1/wpa_supplicant.pid"
#define C1_WIFI_DHCP_PID "/run/c1/udhcpc.pid"
#define C1_WIFI_OUTPUT_CAPACITY 4096U

static c1_wifi_state current_state = C1_WIFI_DISABLED;
static c1_wifi_network cached_networks[C1_WIFI_MAX_NETWORKS];
static size_t cached_network_count;
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

static bool run_wpa_command(const char *command, char *output, size_t capacity)
{
    static unsigned int sequence;
    struct sockaddr_un local;
    struct sockaddr_un remote;
    struct pollfd readable;
    int descriptor;
    ssize_t count;
    bool success = false;

    if (command == NULL || strchr(command, '\n') != NULL || capacity < 2U) {
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
    snprintf(remote.sun_path, sizeof(remote.sun_path), C1_WIFI_CTRL_DIR "/wlan0");
    if (connect(descriptor, (struct sockaddr *)&remote, sizeof(remote)) != 0 ||
        send(descriptor, command, strlen(command), 0) != (ssize_t)strlen(command)) {
        goto finished;
    }
    readable.fd = descriptor;
    readable.events = POLLIN;
    readable.revents = 0;
    if (poll(&readable, 1U, 5000) <= 0 || (readable.revents & POLLIN) == 0) {
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

    return run_wpa_command("PING", output, sizeof(output)) && strstr(output, "PONG") != NULL;
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
        char ssid[C1_WIFI_SSID_CAPACITY];
        int frequency;
        int signal;

        if (sscanf(line, "%23s\t%d\t%d\t%127s\t%32[^\r\n]", bssid, &frequency, &signal, flags, ssid) == 5 &&
            strchr(bssid, ':') != NULL && ssid[0] != '\0') {
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
    char output[C1_WIFI_OUTPUT_CAPACITY];
    char *ssid_line;

    if (snapshot == NULL) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = current_state;
    snapshot->network_count = cached_network_count;
    memcpy(snapshot->networks, cached_networks, sizeof(cached_networks));
    snprintf(snapshot->error, sizeof(snapshot->error), "%s", last_error);
    if (run_wpa_command("STATUS", output, sizeof(output))) {
        ssid_line = strstr(output, "ssid=");
        while (ssid_line != NULL && ssid_line != output && ssid_line[-1] != '\n') {
            ssid_line = strstr(ssid_line + 5U, "ssid=");
        }
        if (ssid_line != NULL) {
            size_t length;

            ssid_line += 5U;
            length = strcspn(ssid_line, "\r\n");
            if (length >= sizeof(snapshot->connected_ssid)) {
                length = sizeof(snapshot->connected_ssid) - 1U;
            }
            memcpy(snapshot->connected_ssid, ssid_line, length);
            snapshot->connected_ssid[length] = '\0';
        }
    }
    if (interface_ipv4(snapshot->ipv4, sizeof(snapshot->ipv4))) {
        snapshot->state = C1_WIFI_CONNECTED;
    }
    return true;
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
    char quoted_ssid[68];
    char quoted_password[132];
    char command[224];
    char network_id[16];
    char *number;
    size_t password_length;
    unsigned int tick;
    c1_status result = C1_STATUS_IO_ERROR;

    if (ssid == NULL || password == NULL || ssid[0] == '\0' || strlen(ssid) > 32U ||
        !quote_wpa_value(ssid, quoted_ssid, sizeof(quoted_ssid))) {
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
    snprintf(command, sizeof(command), "SET_NETWORK %s ssid %s", network_id, quoted_ssid);
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
    last_error[0] = '\0';
    unlink(C1_WIFI_PID);
    unlink(C1_WIFI_DHCP_PID);
    c1_wifi_read_snapshot(snapshot);
    return C1_STATUS_OK;
}