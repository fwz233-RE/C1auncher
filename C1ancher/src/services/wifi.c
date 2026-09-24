#define _DEFAULT_SOURCE 1

#include "services/wifi.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
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
#define C1_WIFI_CONFIG C1_WIFI_DATA_DIR "/wpa_supplicant.conf"
#define C1_WIFI_FACTORY_CONFIG "/usr/resource/wpa_supplicant.conf"
#define C1_WIFI_DISABLED_MARKER C1_WIFI_RUN_DIR "/wifi.disabled"
#define C1_WIFI_PID C1_WIFI_RUN_DIR "/wpa_supplicant.pid"
#define C1_WIFI_DHCP_PID C1_WIFI_RUN_DIR "/udhcpc.pid"
#define C1_WIFI_OUTPUT_CAPACITY 4096U
#define C1_WIFI_SCAN_CAPACITY 65536U
#define C1_WIFI_SAVED_NETWORKS 128U
#define C1_WIFI_CONNECT_MS 45000
#define C1_WIFI_SCAN_MS 20000
#define C1_WIFI_ROLLBACK_MS 10000
#define C1_WIFI_MODULE "/etc/firmware/atbm603x_wifi_sdio.ko"
#define C1_WIFI_MODULE_SYSFS "/sys/module/atbm603x_wifi_sdio"
#define C1_WIFI_INTERFACE_SYSFS "/sys/class/net/wlan0"
#define C1_WIFI_PHY_SYSFS C1_WIFI_INTERFACE_SYSFS "/phy80211"
#define C1_WIFI_HARDWARE_MS 6000
#define C1_WIFI_RECOVERY_MS 6000
#define C1_WIFI_SDIO_DRIVER "/sys/bus/sdio/drivers/atbm_wlan"
#define C1_WIFI_OBSERVE_MS 40
#define C1_WIFI_OBSERVE_CACHE_MS 1000
/* Standard wpa_supplicant control replies have a 4096-byte buffer. Reserve
 * more than the largest possible LIST_NETWORKS row (32 escaped SSID bytes,
 * ID, BSSID, separators and all standard flags) to prove a short page ended
 * because there were no further entries, not because the next row was full. */
#define C1_WIFI_LIST_ROW_BOUND 512U

typedef struct {
    char *bytes;
    size_t size;
} config_backup;

static c1_wifi_state current_state = C1_WIFI_DISABLED;
static c1_wifi_phase current_phase;
static c1_wifi_network cached_networks[C1_WIFI_MAX_NETWORKS];
static size_t cached_network_count;
static char cached_connected_ssid[C1_WIFI_SSID_CAPACITY];
static char last_error[C1_WIFI_ERROR_CAPACITY];
static const c1_wifi_operation_options *active_options;
static int64_t operation_deadline;
static bool operation_cancelled;
static bool restoring;
/* Initial DISABLED means unobserved, not an explicit request to disconnect. */
static bool explicitly_disabled;
static bool scan_profiles_valid;
static bool using_factory_control;
static const char *control_directory(void)
{
    return using_factory_control ? C1_WIFI_FACTORY_CTRL_DIR : C1_WIFI_CTRL_DIR;
}
static bool ensure_directory(const char *path);
static const char *config_path(void)
{
    return using_factory_control ? C1_WIFI_FACTORY_CONFIG : C1_WIFI_CONFIG;
}
static const char *config_directory(void)
{
    return using_factory_control ? "/usr/resource" : C1_WIFI_DATA_DIR;
}
static bool disabled_marker_present(void)
{
    struct stat info;
    return stat(C1_WIFI_DISABLED_MARKER, &info) == 0 && S_ISREG(info.st_mode);
}
static bool set_disabled_marker(bool disabled)
{
    int descriptor;
    if (!ensure_directory(C1_WIFI_RUN_DIR)) return false;
    if (!disabled) return unlink(C1_WIFI_DISABLED_MARKER) == 0 || errno == ENOENT;
    descriptor = open(C1_WIFI_DISABLED_MARKER, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) return false;
    return close(descriptor) == 0;
}
static int scan_profile_saved(const char *ssid, c1_wifi_security security);
static struct {
    bool valid;
    bool connected;
    bool control_available;
    bool interface_disabled;
    c1_wifi_state context_state;
    char target[C1_WIFI_SSID_CAPACITY];
    char ssid[C1_WIFI_SSID_CAPACITY];
    char ipv4[C1_WIFI_IP_CAPACITY];
    int64_t checked_at;
} observation;

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

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return INT64_MAX / 2;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void sleep_milliseconds(long milliseconds)
{
    struct timespec interval = {milliseconds / 1000L, (milliseconds % 1000L) * 1000000L};
    /* An interrupt returns control to the operation's deadline/cancel check. */
    (void)nanosleep(&interval, NULL);
}

static bool continue_operation(void);
static int remaining_ms(int maximum);

static bool run_program(const char *path, char *const argv[], int timeout_ms)
{
    pid_t child;
    int64_t deadline = monotonic_ms() + timeout_ms;
    if (timeout_ms <= 0) {
        return false;
    }
    child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) {
                close(null_fd);
            }
        }
        (void)umask(0077);
        execv(path, argv);
        _exit(127);
    }
    while (monotonic_ms() < deadline && continue_operation()) {
        int status;
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
        sleep_milliseconds(50L);
    }
    /* Only the child we created, never all processes with a matching name. */
    (void)kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
    return false;
}

static int open_control_at(const char *directory, bool observer)
{
    static unsigned int sequence;
    struct sockaddr_un local;
    struct sockaddr_un remote;
    int descriptor;
    if (strlen(directory) + sizeof("/wlan0") > sizeof(remote.sun_path)) {
        return -1;
    }
    descriptor = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (descriptor < 0) {
        return -1;
    }
    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    if (observer) {
        /* Linux abstract client sockets allow read-only startup discovery
         * before /run/c1 exists, without creating directories or files. */
        snprintf(local.sun_path + 1U, sizeof(local.sun_path) - 1U, "c1-wifi-observe-%ld-%u",
                 (long)getpid(), sequence++);
    } else {
        snprintf(local.sun_path, sizeof(local.sun_path), C1_WIFI_RUN_DIR "/wpa-client-%ld-%u",
                 (long)getpid(), sequence++);
    }
    if (bind(descriptor, (struct sockaddr *)&local, sizeof(local)) != 0) {
        close(descriptor);
        return -1;
    }
    if (!observer) (void)chmod(local.sun_path, 0600);
    memset(&remote, 0, sizeof(remote));
    remote.sun_family = AF_UNIX;
    snprintf(remote.sun_path, sizeof(remote.sun_path), "%s/wlan0", directory);
    if (connect(descriptor, (struct sockaddr *)&remote, sizeof(remote)) != 0) {
        close(descriptor);
        if (!observer) unlink(local.sun_path);
        return -1;
    }
    return descriptor;
}

static int open_control(const char *directory)
{
    return open_control_at(directory, false);
}

static void close_control(int descriptor)
{
    struct sockaddr_un local;
    socklen_t size = sizeof(local);
    memset(&local, 0, sizeof(local));
    if (getsockname(descriptor, (struct sockaddr *)&local, &size) == 0 && local.sun_path[0] != '\0') {
        unlink(local.sun_path);
    }
    close(descriptor);
}

/* Reject truncation, rather than parsing a partial last row as a real SSID. */
static bool receive_control(int descriptor, char *output, size_t capacity, int timeout_ms)
{
    struct pollfd readable = {descriptor, POLLIN, 0};
    int64_t deadline = monotonic_ms() + timeout_ms;
    ssize_t count;
    if (capacity < 2U || timeout_ms <= 0) return false;
    for (;;) {
        int64_t left = deadline - monotonic_ms();
        int result;
        if (left <= 0) return false;
        readable.revents = 0;
        result = poll(&readable, 1U, (int)left);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || !(readable.revents & POLLIN)) return false;
        count = recv(descriptor, output, capacity - 1U, MSG_TRUNC);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count < 0 || (size_t)count >= capacity) return false;
        output[count] = '\0';
        return true;
    }
}

static bool command_at(const char *directory, const char *command,
                       char *output, size_t capacity, int timeout_ms)
{
    int descriptor;
    bool success;
    if (strchr(command, '\n') != NULL || strchr(command, '\r') != NULL || timeout_ms <= 0) {
        return false;
    }
    descriptor = open_control(directory);
    if (descriptor < 0) {
        return false;
    }
    success = send(descriptor, command, strlen(command), 0) == (ssize_t)strlen(command) &&
              receive_control(descriptor, output, capacity, timeout_ms);
    close_control(descriptor);
    return success;
}

static bool reply_is(const char *output, const char *expected)
{
    size_t length = strlen(expected);
    return strncmp(output, expected, length) == 0 &&
           (output[length] == '\0' || strcmp(output + length, "\n") == 0 ||
            strcmp(output + length, "\r\n") == 0);
}

static bool real_command(const char *command, char *output, size_t capacity, int timeout_ms)
{
    return command_at(control_directory(), command, output, capacity, timeout_ms);
}

static bool query_observed_status(const char *directory, char *output, size_t capacity, int timeout_ms)
{
    int descriptor;
    bool success;
    if (timeout_ms <= 0) return false;
    descriptor = open_control_at(directory, true);
    if (descriptor < 0) return false;
    success = send(descriptor, "STATUS", 6U, 0) == 6 &&
              receive_control(descriptor, output, capacity, timeout_ms);
    close_control(descriptor);
    return success;
}

static bool interface_ipv4(char *value, size_t capacity)
{
    struct ifreq request;
    int descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    bool found = false;
    value[0] = '\0';
    if (descriptor < 0) {
        return false;
    }
    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, sizeof(request.ifr_name), "wlan0");
    if (ioctl(descriptor, SIOCGIFADDR, &request) == 0) {
        struct sockaddr_in *address = (struct sockaddr_in *)&request.ifr_addr;
        uint32_t ip = ntohl(address->sin_addr.s_addr);
        /* Exclude zero, loopback, multicast, and link-local fallback addresses. */
        if (ip != 0U && (ip >> 24U) != 127U && (ip >> 28U) < 14U &&
            (ip >> 16U) != 0xa9feU) {
            found = inet_ntop(AF_INET, &address->sin_addr, value, (socklen_t)capacity) != NULL;
        }
    }
    close(descriptor);
    return found;
}

static bool clear_ipv4(void)
{
    struct ifreq request;
    struct sockaddr_in *address;
    int descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    bool success;
    if (descriptor < 0) {
        return false;
    }
    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, sizeof(request.ifr_name), "wlan0");
    address = (struct sockaddr_in *)&request.ifr_addr;
    address->sin_family = AF_INET;
    success = ioctl(descriptor, SIOCSIFADDR, &request) == 0;
    close(descriptor);
    return success;
}

static bool set_interface_enabled(bool enabled)
{
    struct ifreq request;
    int descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    bool success;
    if (descriptor < 0) return false;
    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, sizeof(request.ifr_name), "wlan0");
    success = ioctl(descriptor, SIOCGIFFLAGS, &request) == 0;
    if (success) {
        request.ifr_flags = (short)(enabled ? request.ifr_flags | IFF_UP : request.ifr_flags & ~IFF_UP);
        success = ioctl(descriptor, SIOCSIFFLAGS, &request) == 0;
    }
    close(descriptor);
    return success;
}

static bool ensure_directory(const char *path)
{
    struct stat info;
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    return lstat(path, &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
           chmod(path, 0700) == 0;
}

static bool secure_config(void)
{
    struct stat info;
    int descriptor = open(config_path(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    bool success;
    if (descriptor < 0) {
        return false;
    }
    success = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) &&
              info.st_uid == geteuid() && info.st_nlink == 1 && fchmod(descriptor, 0600) == 0;
    close(descriptor);
    return success;
}

static bool read_config_backup(config_backup *backup)
{
    int descriptor;
    struct stat info;
    size_t offset = 0U;
    if (!secure_config()) return false;
    descriptor = open(config_path(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) return false;
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        info.st_size > (off_t)C1_WIFI_SCAN_CAPACITY) {
        close(descriptor);
        return false;
    }
    backup->size = (size_t)info.st_size;
    backup->bytes = malloc(backup->size + 1U);
    if (backup->bytes == NULL) {
        close(descriptor);
        return false;
    }
    while (offset < backup->size) {
        ssize_t count = read(descriptor, backup->bytes + offset, backup->size - offset);
        if (count <= 0) break;
        offset += (size_t)count;
    }
    close(descriptor);
    return offset == backup->size;
}

static bool restore_config_backup(const config_backup *backup)
{
    char temporary[160];
    int descriptor;
    int directory;
    size_t offset = 0U;
    bool success;
    snprintf(temporary, sizeof(temporary), "%s.restore-%ld", config_path(), (long)getpid());
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) return false;
    while (offset < backup->size) {
        ssize_t count = write(descriptor, backup->bytes + offset, backup->size - offset);
        if (count <= 0) break;
        offset += (size_t)count;
    }
    success = offset == backup->size && fsync(descriptor) == 0;
    if (close(descriptor) != 0) success = false;
    if (success) success = rename(temporary, config_path()) == 0;
    if (!success) {
        unlink(temporary);
        return false;
    }
    directory = open(config_directory(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return false;
    success = fsync(directory) == 0;
    close(directory);
    return success;
}

static bool write_runtime_config(void)
{
    static const char content[] = "ctrl_interface=" C1_WIFI_CTRL_DIR "\nupdate_config=1\nap_scan=1\n";
    int descriptor;
    bool success;
    if (!ensure_directory(C1_WIFI_RUN_DIR) || !ensure_directory(C1_WIFI_CTRL_DIR) ||
        !ensure_directory(C1_WIFI_DATA_DIR)) {
        return false;
    }
    descriptor = open(config_path(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        return errno == EEXIST && secure_config();
    }
    success = write(descriptor, content, sizeof(content) - 1U) == (ssize_t)(sizeof(content) - 1U) &&
              fsync(descriptor) == 0;
    if (close(descriptor) != 0) {
        success = false;
    }
    if (!success) {
        unlink(config_path());
    }
    return success;
}

/* /proc cmdline consists of NUL-delimited arguments. Require exact interface
 * and project pidfile arguments before signalling an existing DHCP client. */
static bool argument_is(const char *arguments, size_t size, const char *option, const char *value)
{
    size_t offset = 0U;
    size_t option_length = strlen(option);
    while (offset < size) {
        size_t length = strnlen(arguments + offset, size - offset);
        if (length == size - offset) {
            return false;
        }
        if (strcmp(arguments + offset, option) == 0 && offset + length + 1U < size &&
            strcmp(arguments + offset + length + 1U, value) == 0) {
            return true;
        }
        if (strncmp(arguments + offset, option, option_length) == 0 &&
            strcmp(arguments + offset + option_length, value) == 0) {
            return true;
        }
        offset += length + 1U;
    }
    return false;
}

static bool inspect_processes(const char *name, bool stop_owned, bool *any)
{
    DIR *processes = opendir("/proc");
    struct dirent *entry;
    bool safe = processes != NULL;
    *any = false;
    if (processes == NULL) {
        return false;
    }
    while ((entry = readdir(processes)) != NULL) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        char path[64];
        char arguments[4096];
        const char *base;
        ssize_t count;
        int descriptor;
        if (*end != '\0' || pid <= 1L || pid > INT_MAX) {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        descriptor = open(path, O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) {
            continue;
        }
        count = read(descriptor, arguments, sizeof(arguments));
        close(descriptor);
        if (count <= 0 || (size_t)count == sizeof(arguments) || arguments[count - 1] != '\0') {
            continue;
        }
        base = strrchr(arguments, '/');
        base = base != NULL ? base + 1 : arguments;
        if (strcmp(base, "busybox") == 0 && strlen(arguments) + 1U < (size_t)count) {
            base = arguments + strlen(arguments) + 1U;
        }
        if (strcmp(base, name) != 0 || !argument_is(arguments, (size_t)count, "-i", "wlan0")) {
            continue;
        }
        *any = true;
        if (strcmp(name, "udhcpc") != 0 ||
            !argument_is(arguments, (size_t)count, "-p", C1_WIFI_DHCP_PID)) {
            safe = false;
            continue;
        }
        if (stop_owned && kill((pid_t)pid, SIGTERM) != 0 && errno != ESRCH) {
            safe = false;
        }
    }
    closedir(processes);
    return safe;
}

static bool dhcp_available(void)
{
    bool any;
    return inspect_processes("udhcpc", false, &any);
}

static bool stop_owned_dhcp(void)
{
    bool any;
    int64_t deadline = monotonic_ms() + remaining_ms(1000);
    if (!inspect_processes("udhcpc", true, &any)) {
        return false;
    }
    while (any && monotonic_ms() < deadline && continue_operation()) {
        sleep_milliseconds(50L);
        if (!inspect_processes("udhcpc", false, &any)) {
            return false;
        }
    }
    return !any;
}

static bool acquire_dhcp(int timeout_ms)
{
    char *const argv[] = {"udhcpc", "-n", "-t", "3", "-T", "2", "-i", "wlan0",
                          "-p", C1_WIFI_DHCP_PID, "-x", "hostname:C1-Slim", NULL};
    const char *binary = access("/sbin/udhcpc", X_OK) == 0 ? "/sbin/udhcpc" : "/bin/udhcpc";
    /* Default udhcpc mode backgrounds only AFTER a lease; -n exits on lease
     * failure (unlike -b). Retain the owned daemon for lease renewal. Clear
     * the previous address before invoking the ordinary DHCP script. */
    return stop_owned_dhcp() && clear_ipv4() &&
           run_program(binary, argv, remaining_ms(timeout_ms));
}

static bool control_ready_at(const char *directory)
{
    char output[64];
    return command_at(directory, "PING", output, sizeof(output), remaining_ms(500)) &&
           reply_is(output, "PONG");
}

static bool hardware_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static bool load_wifi_module(int timeout_ms)
{
    char *const argv[] = {"insmod", C1_WIFI_MODULE, NULL};
    const char *binary = access("/sbin/insmod", X_OK) == 0 ? "/sbin/insmod" : "/bin/insmod";
    return run_program(binary, argv, timeout_ms);
}

static bool rfkill_entry_name(const char *name)
{
    const char *number;
    if (strncmp(name, "rfkill", 6U) != 0) return false;
    number = name + 6U;
    if (*number == '\0') return false;
    while (*number >= '0' && *number <= '9') ++number;
    return *number == '\0';
}

static bool read_sysfs_at(int directory, const char *name, char *value, size_t capacity)
{
    int descriptor = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    ssize_t count;
    if (descriptor < 0) return false;
    count = read(descriptor, value, capacity - 1U);
    close(descriptor);
    if (count <= 0 || (size_t)count >= capacity - 1U) return false;
    value[count] = '\0';
    value[strcspn(value, "\r\n")] = '\0';
    return true;
}

/* A failed SDIO probe can leave a live module with no wlan0. The vendor
 * driver's insmod_stat is 0 in that state and 1 once the netdev exists. Only
 * that positively identified failed, unused module may be retried. Unknown
 * sysfs state, module dependants and bound SDIO devices all refuse recovery. */
static bool module_probe_failed_at(const char *module_path, const char *driver_path)
{
    char value[32];
    int module = open(module_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    bool safe = module >= 0;
    if (module < 0) return false;
    safe = read_sysfs_at(module, "initstate", value, sizeof(value)) && strcmp(value, "live") == 0 &&
           read_sysfs_at(module, "refcnt", value, sizeof(value)) && strcmp(value, "0") == 0 &&
           read_sysfs_at(module, "parameters/insmod_stat", value, sizeof(value)) && strcmp(value, "0") == 0;
    if (safe) {
        int holders = openat(module, "holders", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        DIR *directory = holders >= 0 ? fdopendir(holders) : NULL;
        struct dirent *entry;
        safe = directory != NULL;
        if (directory != NULL) {
            errno = 0;
            while ((entry = readdir(directory)) != NULL) {
                if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) safe = false;
            }
            if (errno != 0) safe = false;
            closedir(directory);
        } else if (holders >= 0) close(holders);
    }
    close(module);
    if (safe) {
        int driver = open(driver_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        DIR *directory = driver >= 0 ? fdopendir(driver) : NULL;
        struct dirent *entry;
        safe = directory != NULL;
        if (directory != NULL) {
            errno = 0;
            while ((entry = readdir(directory)) != NULL) {
                if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") &&
                    strcmp(entry->d_name, "bind") && strcmp(entry->d_name, "unbind") &&
                    strcmp(entry->d_name, "uevent")) safe = false;
            }
            if (errno != 0) safe = false;
            closedir(directory);
        } else if (driver >= 0) close(driver);
    }
    return safe;
}

static bool wifi_module_recovery_safe(void)
{
    struct stat info;
    bool any;
    /* lstat also refuses a dangling wlan0 symlink. Never equate access errors
     * with an absent interface or stop somebody else's networking process. */
    if (lstat(C1_WIFI_INTERFACE_SYSFS, &info) == 0 || errno != ENOENT) return false;
    if (!module_probe_failed_at(C1_WIFI_MODULE_SYSFS, C1_WIFI_SDIO_DRIVER)) return false;
    if (!inspect_processes("wpa_supplicant", false, &any) || any) return false;
    return inspect_processes("udhcpc", false, &any) && !any;
}

static bool unload_failed_wifi_module(int timeout_ms)
{
    char *const argv[] = {"rmmod", "atbm603x_wifi_sdio", NULL};
    const char *binary = access("/sbin/rmmod", X_OK) == 0 ? "/sbin/rmmod" : "/bin/rmmod";
    /* Recheck immediately before normal (never forced) removal. */
    return timeout_ms > 0 && continue_operation() && wifi_module_recovery_safe() &&
           run_program(binary, argv, timeout_ms);
}

static bool clear_radio_soft_block(int radio)
{
    int descriptor = openat(radio, "soft", O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    bool success = descriptor >= 0 && write(descriptor, "0\n", 2U) == 2;
    if (descriptor >= 0) close(descriptor);
    return success;
}

static struct {
    bool (*read)(int, const char *, char *, size_t);
    bool (*clear_soft)(int);
} radio_io = {read_sysfs_at, clear_radio_soft_block};

static bool unblock_radio_entry(int radio)
{
    char type[16], hard_value[8], soft_value[8];
    if (!radio_io.read(radio, "type", type, sizeof(type)) || strcmp(type, "wlan") != 0 ||
        !radio_io.read(radio, "hard", hard_value, sizeof(hard_value)) || strcmp(hard_value, "0") != 0 ||
        !radio_io.read(radio, "soft", soft_value, sizeof(soft_value))) return false;
    if (strcmp(soft_value, "0") == 0) return true;
    if (strcmp(soft_value, "1") != 0 || !radio_io.clear_soft(radio)) return false;
    return radio_io.read(radio, "soft", soft_value, sizeof(soft_value)) && strcmp(soft_value, "0") == 0;
}

/* Only children of wlan0's own physical radio are eligible. Never write
 * /dev/rfkill (whose ALL operation affects Bluetooth/other WiFi adapters),
 * and never guess ownership from global rfkill names or enumeration order. */
static bool unblock_wifi_radio(void)
{
    DIR *phy = opendir(C1_WIFI_PHY_SYSFS);
    struct dirent *entry;
    bool success = true;
    if (phy == NULL) {
        /* Some older drivers expose no per-PHY rfkill at all. IFF_UP below
         * still fails safely if the kernel reports the radio is blocked. */
        return errno == ENOENT;
    }
    while ((entry = readdir(phy)) != NULL) {
        int radio;
        if (!rfkill_entry_name(entry->d_name)) continue;
        if (!continue_operation()) {
            success = false;
            break;
        }
        radio = openat(dirfd(phy), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (radio < 0) {
            success = false;
            break;
        }
        success = unblock_radio_entry(radio);
        close(radio);
        if (!success) break;
    }
    closedir(phy);
    return success;
}

/* Separate private hardware I/O boundary keeps cold-start tests entirely
 * offline. Neither this table nor any setters are exported in production. */
static struct {
    bool (*exists)(const char *);
    bool (*load_module)(int);
    bool (*unblock)(void);
    bool (*enable)(bool);
    int64_t (*now)(void);
    void (*sleep)(long);
    bool (*recovery_safe)(void);
    bool (*unload_failed_module)(int);
} hardware_io = {hardware_exists, load_wifi_module, unblock_wifi_radio,
                 set_interface_enabled, monotonic_ms, sleep_milliseconds,
                 wifi_module_recovery_safe, unload_failed_wifi_module};

static void wait_for_wifi_interface(int64_t deadline)
{
    while (!hardware_io.exists(C1_WIFI_INTERFACE_SYSFS) &&
           hardware_io.now() < deadline && continue_operation()) {
        int64_t left = deadline - hardware_io.now();
        if (left <= 0) break;
        hardware_io.sleep(left < 100 ? (long)left : 100L);
    }
}

static bool prepare_wifi_hardware(void)
{
    int budget = remaining_ms(C1_WIFI_HARDWARE_MS);
    int64_t deadline = hardware_io.now() + budget;
    if (budget <= 0) return false;
    if (!hardware_io.exists(C1_WIFI_INTERFACE_SYSFS)) {
        if (!hardware_io.exists(C1_WIFI_MODULE_SYSFS)) {
            bool loaded = hardware_io.load_module(remaining_ms(budget < 4000 ? budget : 4000));
            /* Concurrent loading may report EEXIST. Accept it only when the
             * exact module or target interface is now actually present. */
            if (!loaded && !hardware_io.exists(C1_WIFI_MODULE_SYSFS) &&
                !hardware_io.exists(C1_WIFI_INTERFACE_SYSFS)) {
                set_error("WI-FI MODULE LOAD FAILED");
                return false;
            }
        }
        wait_for_wifi_interface(deadline);
        if (!hardware_io.exists(C1_WIFI_INTERFACE_SYSFS) && continue_operation() &&
            remaining_ms(C1_WIFI_RECOVERY_MS) > 0 && hardware_io.recovery_safe()) {
            /* One recovery attempt inside the scan/connect total deadline.
             * Keep normal cold start's 6 s grace: a slow probe is not failure.
             * Do not clear credentials, use vendor scripts or force removal. */
            budget = remaining_ms(C1_WIFI_RECOVERY_MS);
            deadline = hardware_io.now() + budget;
            if (!hardware_io.unload_failed_module(budget < 1000 ? budget : 1000)) {
                set_error("WI-FI FAILED DRIVER RECOVERY REFUSED");
                return false;
            }
            int64_t left = deadline - hardware_io.now();
            if (left <= 0 || !continue_operation()) return false;
            int load_budget = remaining_ms(left < 4000 ? (int)left : 4000);
            if (load_budget <= 0 || !continue_operation()) return false;
            bool loaded = hardware_io.load_module(load_budget);
            if (!loaded && !hardware_io.exists(C1_WIFI_MODULE_SYSFS) &&
                !hardware_io.exists(C1_WIFI_INTERFACE_SYSFS)) {
                set_error("WI-FI MODULE LOAD FAILED");
                return false;
            }
            wait_for_wifi_interface(deadline);
        }
        if (!hardware_io.exists(C1_WIFI_INTERFACE_SYSFS)) {
            set_error("WLAN0 INITIALIZATION TIMED OUT");
            return false;
        }
    }
    if (!continue_operation() || hardware_io.now() >= deadline) return false;
    if (!hardware_io.unblock()) {
        set_error("WLAN0 RADIO BLOCKED OR UNAVAILABLE");
        return false;
    }
    if (!continue_operation() || hardware_io.now() >= deadline || !hardware_io.enable(true)) {
        set_error("WLAN0 ENABLE FAILED");
        return false;
    }
    return true;
}

static bool ensure_wifi_ready(void)
{
    bool any;
    int64_t deadline;
    if (!ensure_directory(C1_WIFI_RUN_DIR)) {
        set_error("CONTROL DIRECTORY FAILED");
        return false;
    }
    if (control_ready_at(C1_WIFI_CTRL_DIR)) {
        using_factory_control = false;
        return prepare_wifi_hardware();
    }
    if (control_ready_at(C1_WIFI_FACTORY_CTRL_DIR)) {
        /* The firmware's S40network/wifi_up.sh already owns wlan0. Adopt its
         * control socket instead of creating a second supplicant that can race
         * it; all commands remain scoped to this interface. */
        using_factory_control = true;
        return prepare_wifi_hardware();
    }
    /* Never take over a factory supplicant or invoke vendor scripts which may
     * use global killall. Existing foreign wlan0 ownership is explicit. */
    if (!inspect_processes("wpa_supplicant", false, &any) || any) {
        set_error("WLAN0 OWNED BY ANOTHER SERVICE");
        return false;
    }
    if (!prepare_wifi_hardware()) return false;
    if (!write_runtime_config()) {
        set_error("RUNTIME CONFIG FAILED");
        return false;
    }
    using_factory_control = false;
    {
        char config_file[PATH_MAX];
        char *const argv[] = {"wpa_supplicant", "-B", "-D", "nl80211", "-i", "wlan0",
                              "-c", config_file, "-P", C1_WIFI_PID, NULL};
        snprintf(config_file, sizeof(config_file), "%s", config_path());
        const char *binary = access("/usr/sbin/wpa_supplicant", X_OK) == 0
                                 ? "/usr/sbin/wpa_supplicant" : "/sbin/wpa_supplicant";
        if (!run_program(binary, argv, remaining_ms(4000))) {
            set_error("SUPPLICANT FAILED");
            return false;
        }
    }
    deadline = monotonic_ms() + remaining_ms(5000);
    while (monotonic_ms() < deadline && continue_operation()) {
        if (control_ready_at(C1_WIFI_CTRL_DIR)) {
            return true;
        }
        sleep_milliseconds(100L);
    }
    set_error("CONTROL SOCKET TIMEOUT");
    return false;
}

static int scan_events_open(void)
{
    char output[256];
    int descriptor = open_control(control_directory());
    if (descriptor >= 0 && send(descriptor, "ATTACH", 6U, 0) == 6 &&
        receive_control(descriptor, output, sizeof(output), remaining_ms(750)) && reply_is(output, "OK")) {
        return descriptor;
    }
    if (descriptor >= 0) {
        close_control(descriptor);
    }
    return -1;
}

static int scan_events_wait(int descriptor, int timeout_ms)
{
    char output[1024];
    const char *event;
    if (!receive_control(descriptor, output, sizeof(output), timeout_ms)) {
        return 0;
    }
    event = output;
    if (*event == '<') {
        event = strchr(event, '>');
        if (event == NULL) {
            return 0;
        }
        ++event;
    }
    if (strncmp(event, "CTRL-EVENT-SCAN-RESULTS", 23U) == 0) {
        return 1;
    }
    if (strncmp(event, "CTRL-EVENT-SCAN-FAILED", 22U) == 0) {
        return -1;
    }
    return 0;
}

/* Private I/O boundary. Host tests include this translation unit and replace
 * these functions; no exported setter, environment switch or production
 * validation bypass exists. All policy below executes identically in tests. */
static struct {
    int64_t (*now)(void);
    void (*sleep)(long);
    bool (*ready)(void);
    bool (*command)(const char *, char *, size_t, int);
    bool (*observe_status)(const char *, char *, size_t, int);
    bool (*ipv4)(char *, size_t);
    bool (*dhcp_available)(void);
    bool (*dhcp)(int);
    bool (*stop_dhcp)(void);
    bool (*clear_address)(void);
    bool (*secure_config)(void);
    bool (*backup_config)(config_backup *);
    bool (*restore_config)(const config_backup *);
    int (*events_open)(void);
    int (*events_wait)(int, int);
    void (*events_close)(int);
} wifi_io = {monotonic_ms, sleep_milliseconds, ensure_wifi_ready, real_command, query_observed_status,
             interface_ipv4, dhcp_available, acquire_dhcp, stop_owned_dhcp, clear_ipv4, secure_config,
             read_config_backup, restore_config_backup,
             scan_events_open, scan_events_wait, close_control};

static bool continue_operation(void)
{
    if (active_options != NULL && active_options->progress != NULL &&
        !active_options->progress(current_phase, active_options->context) && !restoring) {
        operation_cancelled = true;
    }
    return (!operation_cancelled || restoring) &&
           (operation_deadline == 0 || wifi_io.now() < operation_deadline);
}

static int remaining_ms(int maximum)
{
    int64_t remaining;
    if (!continue_operation()) {
        return 0;
    }
    if (operation_deadline == 0) {
        return maximum;
    }
    remaining = operation_deadline - wifi_io.now();
    return remaining <= 0 ? 0 : remaining < maximum ? (int)remaining : maximum;
}

static void begin_operation(const c1_wifi_operation_options *options, int timeout_ms)
{
    active_options = options;
    scan_profiles_valid = false;
    observation.valid = false;
    explicitly_disabled = disabled_marker_present();
    operation_cancelled = false;
    restoring = false;
    operation_deadline = wifi_io.now() + timeout_ms;
    current_phase = C1_WIFI_PHASE_PREPARING;
    last_error[0] = '\0';
}

static bool command(const char *text, char *output, size_t capacity)
{
    int timeout = remaining_ms(750);
    return timeout > 0 && wifi_io.command(text, output, capacity, timeout);
}

static bool command_ok(const char *text)
{
    char output[128];
    return command(text, output, sizeof(output)) && reply_is(output, "OK");
}

static bool network_command(const char *verb, int id)
{
    char text[64];
    snprintf(text, sizeof(text), "%s %d", verb, id);
    return command_ok(text);
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

bool c1_wifi_decode_scan_ssid(const char *encoded, char *decoded, size_t capacity)
{
    size_t input_index = 0U;
    size_t output_index = 0U;
    if (encoded == NULL || decoded == NULL || capacity == 0U) return false;
    decoded[0] = '\0';
    while (encoded[input_index] != '\0') {
        unsigned char value = (unsigned char)encoded[input_index++];
        if (value == '\\') {
            char escape = encoded[input_index++];
            if (escape == '\0') return false;
            if (escape == 'x') {
                int high;
                int low;
                if (encoded[input_index] == '\0' || encoded[input_index + 1U] == '\0') return false;
                high = hex_value(encoded[input_index]);
                low = hex_value(encoded[input_index + 1U]);
                if (high < 0 || low < 0) return false;
                value = (unsigned char)((high << 4) | low);
                input_index += 2U;
            } else {
                switch (escape) {
                case '\\': case '"': value = (unsigned char)escape; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                case 'e': value = 27U; break;
                default: return false;
                }
            }
        }
        if (value == 0U || output_index + 1U >= capacity || output_index >= 32U) return false;
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
    if (ssid == NULL || encoded == NULL) return false;
    length = strlen(ssid);
    if (length == 0U || length > 32U || capacity < length * 2U + 1U) return false;
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)ssid[index];
        encoded[index * 2U] = digits[value >> 4U];
        encoded[index * 2U + 1U] = digits[value & 0x0fU];
    }
    encoded[length * 2U] = '\0';
    return true;
}

bool c1_wifi_security_supported(c1_wifi_security security)
{
    return security == C1_WIFI_SECURITY_OPEN || security == C1_WIFI_SECURITY_WPA_PSK;
}

static c1_wifi_security parse_security(const char *flags)
{
    if (flags[0] == '\0') return C1_WIFI_SECURITY_UNKNOWN;
    if (strstr(flags, "WEP") != NULL) return C1_WIFI_SECURITY_WEP;
    if (strstr(flags, "PSK") != NULL) return C1_WIFI_SECURITY_WPA_PSK;
    if (strstr(flags, "EAP") != NULL || strstr(flags, "IEEE8021X") != NULL) return C1_WIFI_SECURITY_ENTERPRISE;
    if (strstr(flags, "SAE") != NULL) return C1_WIFI_SECURITY_SAE;
    if (strstr(flags, "OWE") != NULL) return C1_WIFI_SECURITY_OWE;
    /* Treat unknown flags conservatively, never silently downgrade to open. */
    while (*flags != '\0') {
        if (strncmp(flags, "[ESS]", 5U) == 0 || strncmp(flags, "[WPS]", 5U) == 0) {
            flags += 5U;
        } else {
            return C1_WIFI_SECURITY_UNKNOWN;
        }
    }
    return C1_WIFI_SECURITY_OPEN;
}

static int network_compare(const void *left, const void *right)
{
    const c1_wifi_network *a = left;
    const c1_wifi_network *b = right;
    int name;
    if (a->saved != b->saved) return a->saved ? -1 : 1;
    if (a->signal_dbm != b->signal_dbm) return a->signal_dbm > b->signal_dbm ? -1 : 1;
    name = strcmp(a->ssid, b->ssid);
    return name != 0 ? name : (int)a->security - (int)b->security;
}

static bool parse_integer(const char *text, int minimum, int maximum, int *value)
{
    char *end;
    long number;
    errno = 0;
    number = strtol(text, &end, 10);
    if (text == end || *end != '\0' || errno != 0 || number < minimum || number > maximum) return false;
    *value = (int)number;
    return true;
}

static bool parse_scan_results(char *output, bool reset)
{
    char *save = NULL;
    char *line = strtok_r(output, "\n", &save);
    if (line == NULL || strncmp(line, "bssid / frequency / signal level / flags / ssid", 45U) != 0) {
        return false;
    }
    if (reset) {
        cached_network_count = 0U;
        memset(cached_networks, 0, sizeof(cached_networks));
    }
    while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
        char *fields[5];
        size_t index;
        int frequency;
        c1_wifi_network network;
        memset(&network, 0, sizeof(network));
        fields[0] = line;
        for (index = 1U; index < 5U; ++index) {
            char *tab = strchr(fields[index - 1U], '\t');
            if (tab == NULL) break;
            *tab = '\0';
            fields[index] = tab + 1;
        }
        if (index != 5U || strlen(fields[0]) != 17U) continue;
        for (index = 0U; index < 17U; ++index) {
            if (index % 3U == 2U ? fields[0][index] != ':' : hex_value(fields[0][index]) < 0) break;
        }
        if (index != 17U || !parse_integer(fields[1], 1, 100000, &frequency) ||
            !parse_integer(fields[2], -127, 0, &network.signal_dbm)) continue;
        fields[4][strcspn(fields[4], "\r")] = '\0';
        if (!c1_wifi_decode_scan_ssid(fields[4], network.ssid, sizeof(network.ssid))) continue;
        network.security = parse_security(fields[3]);
        network.secured = network.security != C1_WIFI_SECURITY_OPEN;
        network.supported = c1_wifi_security_supported(network.security);
        if (scan_profiles_valid && network.supported) {
            int saved = scan_profile_saved(network.ssid, network.security);
            if (saved < 0) return false;
            network.saved = saved > 0;
        }
        for (index = 0U; index < cached_network_count; ++index) {
            if (strcmp(cached_networks[index].ssid, network.ssid) == 0 &&
                cached_networks[index].security == network.security) break;
        }
        if (index < cached_network_count) {
            if (network.signal_dbm > cached_networks[index].signal_dbm) cached_networks[index] = network;
        } else if (cached_network_count < C1_WIFI_MAX_NETWORKS) {
            cached_networks[cached_network_count++] = network;
        } else if (network_compare(&network, &cached_networks[cached_network_count - 1U]) < 0) {
            cached_networks[cached_network_count - 1U] = network;
        }
        /* Keep saved profiles first, then strongest signal, across ALL rows. */
        qsort(cached_networks, cached_network_count, sizeof(cached_networks[0]), network_compare);
    }
    return true;
}

static bool status_value(const char *output, const char *key, char *value, size_t capacity)
{
    size_t key_length = strlen(key);
    while (*output != '\0') {
        size_t length = strcspn(output, "\n");
        if (length > key_length && strncmp(output, key, key_length) == 0 && output[key_length] == '=') {
            size_t size = length - key_length - 1U;
            if (size > 0U && output[length - 1U] == '\r') --size;
            if (size >= capacity) return false;
            memcpy(value, output + key_length + 1U, size);
            value[size] = '\0';
            return true;
        }
        output += length;
        if (*output == '\n') ++output;
    }
    return false;
}

static bool read_scan_networks(void)
{
    char output[C1_WIFI_OUTPUT_CAPACITY];
    char request[64] = "BSS FIRST";
    char id_text[16];
    int previous_id = -1;
    cached_network_count = 0U;
    memset(cached_networks, 0, sizeof(cached_networks));
    /* SCAN_RESULTS is commonly limited to 4 KiB INSIDE wpa_supplicant even
     * with a larger receive buffer. Walk every BSS instead of accepting that
     * silently truncated table. No legacy-cache fallback on unsupported BSS. */
    while (continue_operation()) {
        char bssid[24], frequency[16], level[16], flags[512];
        char ssid[C1_WIFI_SSID_CAPACITY * 4U];
        char row[1024];
        int id;
        if (!command(request, output, sizeof(output))) return false;
        if (reply_is(output, "FAIL") || output[0] == '\0') return true;
        if (!status_value(output, "id", id_text, sizeof(id_text)) ||
            !parse_integer(id_text, 0, INT_MAX, &id) || id <= previous_id ||
            !status_value(output, "bssid", bssid, sizeof(bssid)) ||
            !status_value(output, "freq", frequency, sizeof(frequency)) ||
            !status_value(output, "level", level, sizeof(level)) ||
            !status_value(output, "flags", flags, sizeof(flags)) ||
            !status_value(output, "ssid", ssid, sizeof(ssid))) return false;
        snprintf(row, sizeof(row), "bssid / frequency / signal level / flags / ssid\n%s\t%s\t%s\t%s\t%s\n",
                 bssid, frequency, level, flags, ssid);
        if (!parse_scan_results(row, false)) return false;
        previous_id = id;
        snprintf(request, sizeof(request), "BSS NEXT-%d", id);
    }
    return false;
}

static bool completed_status(const char *output, const char *target, int network_id,
                             char *ssid, size_t capacity)
{
    char state[32];
    char encoded[C1_WIFI_SSID_CAPACITY * 4U];
    char id_text[16];
    int id;
    if (!status_value(output, "wpa_state", state, sizeof(state)) || strcmp(state, "COMPLETED") != 0 ||
        !status_value(output, "ssid", encoded, sizeof(encoded)) ||
        !c1_wifi_decode_scan_ssid(encoded, ssid, capacity)) return false;
    if (target != NULL && strcmp(ssid, target) != 0) return false;
    return network_id < 0 || (status_value(output, "id", id_text, sizeof(id_text)) &&
                             parse_integer(id_text, 0, INT_MAX, &id) && id == network_id);
}

static void remember_verified_connection(const char *ssid, const char *ipv4)
{
    /* Connection/resume have already checked authentication and acquired a
     * lease. Do not make their final result depend on an extra short UI probe. */
    memset(&observation, 0, sizeof(observation));
    observation.valid = true;
    observation.connected = true;
    observation.control_available = true;
    observation.context_state = C1_WIFI_CONNECTED;
    snprintf(observation.target, sizeof(observation.target), "%s", ssid);
    snprintf(observation.ssid, sizeof(observation.ssid), "%s", ssid);
    snprintf(observation.ipv4, sizeof(observation.ipv4), "%s", ipv4);
    observation.checked_at = wifi_io.now();
}

bool c1_wifi_read_snapshot(c1_wifi_snapshot *snapshot)
{
    char output[C1_WIFI_OUTPUT_CAPACITY];
    const char *target;
    int64_t now;
    if (snapshot == NULL) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = current_state;
    snapshot->phase = current_phase;
    snapshot->network_count = cached_network_count;
    memcpy(snapshot->networks, cached_networks, sizeof(cached_networks));
    snprintf(snapshot->error, sizeof(snapshot->error), "%s", last_error);
    /* Busy/error/explicit-off state has precedence over cached observation.
     * Only initial unobserved DISABLED is eligible for passive discovery. */
    if (explicitly_disabled || disabled_marker_present() || current_state == C1_WIFI_ERROR || current_state == C1_WIFI_CONNECTING ||
        current_state == C1_WIFI_SCANNING) {
        observation.valid = false;
        return true;
    }
    target = current_state == C1_WIFI_CONNECTED ? cached_connected_ssid : "";
    now = wifi_io.now();
    if (!observation.valid || observation.context_state != current_state ||
        strcmp(observation.target, target) != 0 || now < observation.checked_at ||
        now - observation.checked_at >= C1_WIFI_OBSERVE_CACHE_MS) {
        int budget = operation_deadline != 0 ? remaining_ms(C1_WIFI_OBSERVE_MS) : C1_WIFI_OBSERVE_MS;
        int64_t deadline = now + budget;
        bool received = false;
        /* Both successful and failed observations are rate-limited. Clear a
         * formerly positive result BEFORE refreshing: no stale-on-error. */
        memset(&observation, 0, sizeof(observation));
        observation.context_state = current_state;
        snprintf(observation.target, sizeof(observation.target), "%s", target);
        if (budget > 0) {
            received = wifi_io.observe_status(C1_WIFI_CTRL_DIR, output, sizeof(output),
                                               (budget + 1) / 2);
            if (!received && wifi_io.now() < deadline) {
                received = wifi_io.observe_status(C1_WIFI_FACTORY_CTRL_DIR, output, sizeof(output),
                                                   (int)(deadline - wifi_io.now()));
                if (received) using_factory_control = true;
            }
        }
        observation.control_available = received;
        if (received) {
            char state[32];
            observation.interface_disabled = status_value(output, "wpa_state", state, sizeof(state)) &&
                                             strcmp(state, "INTERFACE_DISABLED") == 0;
        }
        observation.connected = received &&
            completed_status(output, target[0] != '\0' ? target : NULL, -1,
                             observation.ssid, sizeof(observation.ssid)) &&
            wifi_io.ipv4(observation.ipv4, sizeof(observation.ipv4));
        if (!observation.connected) {
            observation.ssid[0] = '\0';
            observation.ipv4[0] = '\0';
        }
        observation.checked_at = wifi_io.now();
        observation.valid = true;
    }
    if (observation.connected) {
        snapshot->state = C1_WIFI_CONNECTED;
        snprintf(snapshot->connected_ssid, sizeof(snapshot->connected_ssid), "%s", observation.ssid);
        snprintf(snapshot->ipv4, sizeof(snapshot->ipv4), "%s", observation.ipv4);
    } else if (observation.interface_disabled) {
        /* Off keeps the daemon/socket alive. After a UI restart, a reachable
         * control socket alone must not turn the switch back into READY. */
        snapshot->state = C1_WIFI_DISABLED;
    } else if (current_state != C1_WIFI_DISABLED || observation.control_available) {
        snapshot->state = C1_WIFI_READY;
    }
    return true;
}

void c1_wifi_adopt_snapshot(const c1_wifi_snapshot *snapshot)
{
    if (snapshot == NULL) return;
    observation.valid = false;
    /* Worker completion snapshots carry DONE; an initial zero/IDLE snapshot
     * remains unobserved and must not masquerade as an explicit disable. */
    explicitly_disabled = snapshot->state == C1_WIFI_DISABLED && snapshot->phase == C1_WIFI_PHASE_DONE;
    current_state = snapshot->state;
    current_phase = snapshot->phase;
    cached_network_count = snapshot->network_count <= C1_WIFI_MAX_NETWORKS
                               ? snapshot->network_count : C1_WIFI_MAX_NETWORKS;
    memcpy(cached_networks, snapshot->networks, sizeof(cached_networks));
    snprintf(cached_connected_ssid, sizeof(cached_connected_ssid), "%.32s", snapshot->connected_ssid);
    snprintf(last_error, sizeof(last_error), "%.47s", snapshot->error);
}

static c1_status end_operation(c1_status result, c1_wifi_snapshot *snapshot)
{
    if (result != C1_STATUS_OK) {
        if (operation_cancelled) {
            result = C1_STATUS_INTERRUPTED;
            set_error(strstr(last_error, "RESTORE INCOMPLETE") != NULL
                          ? "CANCELLED; RESTORE INCOMPLETE" : "WI-FI OPERATION CANCELLED");
            current_phase = C1_WIFI_PHASE_CANCELLED;
        } else {
            current_phase = C1_WIFI_PHASE_FAILED;
            if (current_state != C1_WIFI_ERROR) set_error("WI-FI OPERATION FAILED");
        }
    } else {
        current_phase = C1_WIFI_PHASE_DONE;
    }
    /* Preserve the deadline for final snapshot validation, then release it. */
    (void)c1_wifi_read_snapshot(snapshot);
    active_options = NULL;
    scan_profiles_valid = false;
    operation_deadline = 0;
    restoring = false;
    return result;
}

static bool prepare_scan_profiles(void);

/* Bringing IFF_UP back up is asynchronous to the supplicant's netlink event
 * loop. A PONG only proves the control socket is alive, including while the
 * interface is still INTERFACE_DISABLED after our own disable/pause. */
static bool wait_interface_enabled(void)
{
    char output[C1_WIFI_OUTPUT_CAPACITY], state[32];
    int64_t deadline = wifi_io.now() + remaining_ms(C1_WIFI_HARDWARE_MS);
    while (continue_operation() && wifi_io.now() < deadline) {
        if (command("STATUS", output, sizeof(output)) &&
            status_value(output, "wpa_state", state, sizeof(state)) &&
            strcmp(state, "INTERFACE_DISABLED") != 0) return true;
        int delay = remaining_ms(100);
        if (delay > 0) wifi_io.sleep(delay);
    }
    set_error("WLAN0 ENABLE FAILED");
    return false;
}

c1_status c1_wifi_scan_ex(const c1_wifi_operation_options *options, c1_wifi_snapshot *snapshot)
{
    int events = -1;
    c1_status result = C1_STATUS_IO_ERROR;
    begin_operation(options, C1_WIFI_SCAN_MS);
    current_state = C1_WIFI_SCANNING;
    if (!continue_operation() || !wifi_io.ready() || !wait_interface_enabled())
        return end_operation(C1_STATUS_UNAVAILABLE, snapshot);
    (void)set_disabled_marker(false);
    explicitly_disabled = false;
    current_phase = C1_WIFI_PHASE_SCANNING;
    while (continue_operation()) {
        char output[128];
        /* A saved profile can already be doing its automatic startup scan.
         * FAIL-BUSY is retryable, not a permanent SCAN START FAILED. Each retry
         * discards that scan's event subscription and BSS cache, so its results
         * cannot be mistaken for completion of our newly accepted request. */
        if (!command_ok("BSS_FLUSH 0")) {
            set_error("FRESH SCAN UNSUPPORTED");
            result = C1_STATUS_UNSUPPORTED;
            goto finished;
        }
        events = wifi_io.events_open();
        if (events < 0 || !command("SCAN", output, sizeof(output))) {
            set_error("SCAN START FAILED");
            goto finished;
        }
        if (reply_is(output, "OK")) break;
        if (!reply_is(output, "FAIL-BUSY")) {
            set_error("SCAN START FAILED");
            goto finished;
        }
        int wait = remaining_ms(250);
        if (wait > 0) (void)wifi_io.events_wait(events, wait);
        wifi_io.events_close(events);
        events = -1;
        int delay = remaining_ms(100);
        if (delay > 0) wifi_io.sleep(delay);
    }
    while (events >= 0 && continue_operation()) {
        int event = wifi_io.events_wait(events, remaining_ms(250));
        if (event < 0) {
            set_error("SCAN FAILED");
            goto finished;
        }
        if (event > 0) {
            if (!prepare_scan_profiles()) {
                set_error("SAVED NETWORK LOOKUP FAILED; RETRY SCAN");
                goto finished;
            }
            if (!read_scan_networks()) {
                set_error("SCAN RESULTS INVALID OR TOO LARGE");
                goto finished;
            }
            current_state = C1_WIFI_READY;
            result = C1_STATUS_OK; /* A completed, empty scan is valid. */
            goto finished;
        }
    }
    set_error("SCAN TIMED OUT");
finished:
    if (events >= 0) wifi_io.events_close(events);
    if (result != C1_STATUS_OK) {
        cached_network_count = 0U;
        memset(cached_networks, 0, sizeof(cached_networks));
    }
    return end_operation(result, snapshot);
}

c1_status c1_wifi_scan(c1_wifi_snapshot *snapshot)
{
    return c1_wifi_scan_ex(NULL, snapshot);
}

/* WPA/WPA2 specifies PBKDF2-HMAC-SHA1, 4096 rounds, a 256-bit result.
 * Derive locally and send the 64 hex digits: wpa_config_parse_psk does NOT
 * unescape quoted strings, while its config-file comment parser can truncate
 * a literal quote followed by '#'. Hex PSKs survive both control and file
 * parsing unchanged. SHA-1 is used only for this mandated legacy KDF, never
 * as a signature or collision-resistant digest. No new runtime dependency. */
typedef struct {
    uint32_t state[5];
    uint64_t bytes;
    unsigned char block[64];
    size_t used;
} wifi_sha1;

static uint32_t rotate_left(uint32_t value, unsigned int bits)
{
    return (value << bits) | (value >> (32U - bits));
}

static void wifi_sha1_transform(wifi_sha1 *context)
{
    uint32_t words[80];
    uint32_t a = context->state[0], b = context->state[1], c = context->state[2];
    uint32_t d = context->state[3], e = context->state[4];
    for (size_t i = 0U; i < 16U; ++i) {
        const unsigned char *p = context->block + i * 4U;
        words[i] = (uint32_t)p[0] << 24U | (uint32_t)p[1] << 16U |
                   (uint32_t)p[2] << 8U | (uint32_t)p[3];
    }
    for (size_t i = 16U; i < 80U; ++i)
        words[i] = rotate_left(words[i - 3U] ^ words[i - 8U] ^ words[i - 14U] ^ words[i - 16U], 1U);
    for (size_t i = 0U; i < 80U; ++i) {
        uint32_t f, k;
        if (i < 20U) { f = (b & c) | (~b & d); k = UINT32_C(0x5a827999); }
        else if (i < 40U) { f = b ^ c ^ d; k = UINT32_C(0x6ed9eba1); }
        else if (i < 60U) { f = (b & c) | (b & d) | (c & d); k = UINT32_C(0x8f1bbcdc); }
        else { f = b ^ c ^ d; k = UINT32_C(0xca62c1d6); }
        uint32_t next = rotate_left(a, 5U) + f + e + k + words[i];
        e = d; d = c; c = rotate_left(b, 30U); b = a; a = next;
    }
    context->state[0] += a; context->state[1] += b; context->state[2] += c;
    context->state[3] += d; context->state[4] += e;
    secure_clear(words, sizeof(words));
}

static void wifi_sha1_init(wifi_sha1 *context)
{
    memset(context, 0, sizeof(*context));
    context->state[0] = UINT32_C(0x67452301);
    context->state[1] = UINT32_C(0xefcdab89);
    context->state[2] = UINT32_C(0x98badcfe);
    context->state[3] = UINT32_C(0x10325476);
    context->state[4] = UINT32_C(0xc3d2e1f0);
}

static void wifi_sha1_update(wifi_sha1 *context, const unsigned char *bytes, size_t size)
{
    context->bytes += size;
    while (size > 0U) {
        size_t take = sizeof(context->block) - context->used;
        if (take > size) take = size;
        memcpy(context->block + context->used, bytes, take);
        context->used += take;
        bytes += take;
        size -= take;
        if (context->used == sizeof(context->block)) {
            wifi_sha1_transform(context);
            context->used = 0U;
        }
    }
}

static void wifi_sha1_finish(wifi_sha1 *context, unsigned char digest[20])
{
    unsigned char padding[64] = {0x80U};
    unsigned char length[8];
    uint64_t bits = context->bytes * 8U;
    size_t count = context->used < 56U ? 56U - context->used : 120U - context->used;
    for (size_t i = 0U; i < 8U; ++i) length[7U - i] = (unsigned char)(bits >> (i * 8U));
    wifi_sha1_update(context, padding, count);
    wifi_sha1_update(context, length, sizeof(length));
    for (size_t i = 0U; i < 20U; ++i)
        digest[i] = (unsigned char)(context->state[i / 4U] >> ((3U - i % 4U) * 8U));
    secure_clear(context, sizeof(*context));
}

static void wifi_hmac_sha1(const wifi_sha1 *inner, const wifi_sha1 *outer,
                           const unsigned char *bytes, size_t size, unsigned char digest[20])
{
    wifi_sha1 context = *inner;
    unsigned char temporary[20];
    wifi_sha1_update(&context, bytes, size);
    wifi_sha1_finish(&context, temporary);
    context = *outer;
    wifi_sha1_update(&context, temporary, sizeof(temporary));
    wifi_sha1_finish(&context, digest);
    secure_clear(temporary, sizeof(temporary));
}

static bool valid_wpa_passphrase(const char *password)
{
    size_t length = strlen(password);
    if (length < 8U || length > 63U) return false;
    for (size_t i = 0U; i < length; ++i) {
        if ((unsigned char)password[i] < 32U || (unsigned char)password[i] > 126U) return false;
    }
    return true;
}

static bool derive_wpa_psk(const char *ssid, const char *password, char output[65])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char pad[64] = {0}, salt[36] = {0}, u[20] = {0}, block[20] = {0}, key[32] = {0};
    wifi_sha1 inner, outer;
    size_t ssid_size = strlen(ssid), password_size = strlen(password);
    bool success = false;
    output[0] = '\0';
    if (ssid_size == 0U || ssid_size > 32U || !valid_wpa_passphrase(password)) return false;
    for (size_t i = 0U; i < sizeof(pad); ++i)
        pad[i] = (unsigned char)((i < password_size ? (unsigned char)password[i] : 0U) ^ 0x36U);
    wifi_sha1_init(&inner);
    wifi_sha1_update(&inner, pad, sizeof(pad));
    for (size_t i = 0U; i < sizeof(pad); ++i) pad[i] ^= 0x36U ^ 0x5cU;
    wifi_sha1_init(&outer);
    wifi_sha1_update(&outer, pad, sizeof(pad));
    memcpy(salt, ssid, ssid_size);
    for (unsigned int index = 1U; index <= 2U; ++index) {
        salt[ssid_size + 3U] = (unsigned char)index;
        wifi_hmac_sha1(&inner, &outer, salt, ssid_size + 4U, u);
        memcpy(block, u, sizeof(block));
        for (unsigned int round = 1U; round < 4096U; ++round) {
            /* Keep cancellation/heartbeat and the enclosing deadline live
             * even on the low-power target; no subprocess sees the secret. */
            if (round % 64U == 1U && !continue_operation()) goto finished;
            wifi_hmac_sha1(&inner, &outer, u, sizeof(u), u);
            for (size_t i = 0U; i < sizeof(block); ++i) block[i] ^= u[i];
        }
        memcpy(key + (index - 1U) * 20U, block, index == 1U ? 20U : 12U);
    }
    if (!continue_operation()) goto finished;
    for (size_t i = 0U; i < sizeof(key); ++i) {
        output[i * 2U] = hex[key[i] >> 4U];
        output[i * 2U + 1U] = hex[key[i] & 15U];
    }
    output[64] = '\0';
    success = true;
finished:
    secure_clear(&inner, sizeof(inner)); secure_clear(&outer, sizeof(outer));
    secure_clear(pad, sizeof(pad)); secure_clear(salt, sizeof(salt));
    secure_clear(u, sizeof(u)); secure_clear(block, sizeof(block)); secure_clear(key, sizeof(key));
    return success;
}

typedef struct {
    int id;
    bool disabled;
    char ssid[C1_WIFI_SSID_CAPACITY];
} saved_network;

typedef struct {
    saved_network networks[C1_WIFI_SAVED_NETWORKS];
    size_t count;
    int previous_id;
    char previous_ssid[C1_WIFI_SSID_CAPACITY];
    bool previous_completed;
    bool previous_disconnected;
} connection_backup;

static bool capture_networks(connection_backup *backup)
{
    char output[C1_WIFI_OUTPUT_CAPACITY];
    char request[64] = "LIST_NETWORKS";
    char *line;
    char *save;
    char id[16], state[32];
    int last_id = -1;
    memset(backup, 0, sizeof(*backup));
    backup->previous_id = -1;
    if (!command("STATUS", output, sizeof(output))) return false;
    backup->previous_completed = completed_status(output, NULL, -1,
                                                  backup->previous_ssid, sizeof(backup->previous_ssid));
    backup->previous_disconnected = status_value(output, "wpa_state", state, sizeof(state)) &&
                                    strcmp(state, "DISCONNECTED") == 0;
    if (status_value(output, "id", id, sizeof(id)) &&
        !parse_integer(id, 0, INT_MAX, &backup->previous_id)) return false;
    /* A short, newline-terminated reply has room for the maximum next row,
     * so a standard daemon could not have omitted one for buffer space. This
     * safely supports old LIST_NETWORKS implementations with no LAST_ID.
     * Near-limit pages still demand working pagination; never infer that a
     * full-looking page or an ignored LAST_ID means the list is complete. */
    while (continue_operation()) {
        size_t before = backup->count;
        size_t reply_length;
        bool complete_page;
        if (!command(request, output, sizeof(output))) return false;
        reply_length = strlen(output);
        if (reply_length == 0U || output[reply_length - 1U] != '\n') return false;
        complete_page = reply_length + C1_WIFI_LIST_ROW_BOUND < sizeof(output);
        line = strtok_r(output, "\n", &save);
        if (line == NULL || strncmp(line, "network id / ssid / bssid / flags", 32U) != 0) return false;
        while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
            char *tab = strchr(line, '\t');
            char *flags;
            saved_network *network;
            if (tab == NULL || strlen(line) >= C1_WIFI_LIST_ROW_BOUND ||
                backup->count == C1_WIFI_SAVED_NETWORKS) return false;
            *tab = '\0';
            flags = strrchr(tab + 1, '\t');
            if (flags == NULL) return false;
            network = &backup->networks[backup->count];
            char *ssid_end = strchr(tab + 1, '\t');
            if (ssid_end == NULL || ssid_end == flags) return false;
            *ssid_end = '\0';
            /* Hidden/invalid names cannot match a scanned UTF-8 SSID, but
             * retain their IDs and enabled bits for safe restoration. */
            if (!c1_wifi_decode_scan_ssid(tab + 1, network->ssid, sizeof(network->ssid)))
                network->ssid[0] = '\0';
            if (!parse_integer(line, 0, INT_MAX, &network->id) || network->id <= last_id) return false;
            last_id = network->id;
            network->disabled = strstr(flags, "[DISABLED]") != NULL;
            if (strstr(flags, "[CURRENT]") != NULL && backup->previous_id < 0) backup->previous_id = network->id;
            ++backup->count;
        }
        if (backup->count == before || complete_page) return true;
        snprintf(request, sizeof(request), "LIST_NETWORKS LAST_ID=%d", last_id);
    }
    return false;
}

/* Match SSID plus authentication, never SSID alone. GET_NETWORK psk may
 * return a masked marker; only test presence and immediately wipe the reply. */
static int saved_profile_matches(int id, c1_wifi_security security)
{
    char request[64], output[256];
    snprintf(request, sizeof(request), "GET_NETWORK %d key_mgmt", id);
    if (!command(request, output, sizeof(output))) return -1;
    output[strcspn(output, "\r\n")] = '\0';
    bool matches = false;
    if (security == C1_WIFI_SECURITY_WPA_PSK) {
        char *save = NULL;
        for (char *token = strtok_r(output, " ", &save); token != NULL;
             token = strtok_r(NULL, " ", &save)) {
            if (strcmp(token, "WPA-PSK") == 0) matches = true;
        }
    } else if (security == C1_WIFI_SECURITY_OPEN) {
        matches = strcmp(output, "NONE") == 0;
    }
    if (!matches) return 0;
    for (unsigned int key = 0U; key < (security == C1_WIFI_SECURITY_OPEN ? 4U : 1U); ++key) {
        if (security == C1_WIFI_SECURITY_OPEN)
            snprintf(request, sizeof(request), "GET_NETWORK %d wep_key%u", id, key);
        else
            snprintf(request, sizeof(request), "GET_NETWORK %d psk", id);
        if (!command(request, output, sizeof(output))) {
            secure_clear(output, sizeof(output));
            return -1;
        }
        output[strcspn(output, "\r\n")] = '\0';
        bool present = output[0] != '\0' && !reply_is(output, "FAIL");
        secure_clear(output, sizeof(output));
        if (security == C1_WIFI_SECURITY_WPA_PSK) return present;
        if (present) return 0;
    }
    return 1;
}

static int find_saved_profile(const connection_backup *backup, const char *ssid,
                               c1_wifi_security security)
{
    /* Newer successfully saved credentials win over older duplicate entries. */
    for (size_t i = backup->count; i > 0U; --i) {
        const saved_network *network = &backup->networks[i - 1U];
        if (strcmp(network->ssid, ssid) != 0) continue;
        int match = saved_profile_matches(network->id, security);
        if (match < 0) return -2;
        if (match > 0) return network->id;
    }
    return -1;
}

static connection_backup scan_profiles;
/* Match results are cached per profile/security for repeated BSS entries. */
static signed char scan_profile_matches[C1_WIFI_SAVED_NETWORKS][2];

static bool prepare_scan_profiles(void)
{
    memset(scan_profile_matches, -1, sizeof(scan_profile_matches));
    scan_profiles_valid = capture_networks(&scan_profiles);
    return scan_profiles_valid;
}

static int scan_profile_saved(const char *ssid, c1_wifi_security security)
{
    size_t kind = security == C1_WIFI_SECURITY_OPEN ? 0U : 1U;
    for (size_t i = scan_profiles.count; i > 0U; --i) {
        if (strcmp(scan_profiles.networks[i - 1U].ssid, ssid) != 0) continue;
        signed char *match = &scan_profile_matches[i - 1U][kind];
        if (*match < 0) {
            int result = saved_profile_matches(scan_profiles.networks[i - 1U].id, security);
            if (result < 0) return -1;
            *match = (signed char)result;
        }
        if (*match > 0) return 1;
    }
    return 0;
}

static bool restore_enabled(const connection_backup *backup, int keep_enabled)
{
    size_t index;
    bool success = true;
    for (index = 0U; index < backup->count; ++index) {
        if (backup->networks[index].id == keep_enabled) continue;
        if (!network_command(backup->networks[index].disabled ? "DISABLE_NETWORK" : "ENABLE_NETWORK",
                             backup->networks[index].id)) success = false;
    }
    return success;
}

static bool wait_authenticated(const char *ssid, int id, int timeout_ms)
{
    char output[C1_WIFI_OUTPUT_CAPACITY];
    char actual[C1_WIFI_SSID_CAPACITY];
    int64_t deadline = wifi_io.now() + remaining_ms(timeout_ms);
    while (continue_operation() && wifi_io.now() < deadline) {
        if (command("STATUS", output, sizeof(output)) &&
            completed_status(output, ssid, id, actual, sizeof(actual))) return true;
        wifi_io.sleep(100L);
    }
    return false;
}

static bool rollback_connection(const connection_backup *backup, int new_id, bool selected, bool dhcp_started)
{
    bool success;
    restoring = true;
    current_phase = C1_WIFI_PHASE_RESTORING;
    operation_deadline = wifi_io.now() + C1_WIFI_ROLLBACK_MS;
    success = true;
    if (dhcp_started) {
        /* Never leave a new-network renewal daemon/address attached to the
         * restored (or disconnected) network, even if old auth later fails. */
        if (!wifi_io.stop_dhcp()) success = false;
        if (!wifi_io.clear_address()) success = false;
    }
    if (new_id >= 0 && !network_command("REMOVE_NETWORK", new_id)) success = false;
    if (selected && backup->previous_id >= 0) {
        if (!network_command("SELECT_NETWORK", backup->previous_id)) success = false;
    } else if (selected && !command_ok("DISCONNECT")) {
        success = false;
    }
    if (!restore_enabled(backup, -1)) success = false;
    /* ENABLE_NETWORK can itself start association. Restore explicit
     * disconnected state AFTER enabled bits, otherwise that state is undone
     * by the last enabled profile (including a failed saved reconnect). */
    if (backup->previous_disconnected && !command_ok("DISCONNECT")) success = false;
    if (selected && backup->previous_completed) {
        if (!wait_authenticated(backup->previous_ssid, backup->previous_id, 4000)) {
            success = false;
        } else if (dhcp_started && !wifi_io.dhcp(remaining_ms(4000))) {
            success = false;
        }
    }
    /* No SAVE_CONFIG in rollback: the previous on-disk credentials were never
     * removed or overwritten before the new connection was validated. */
    return success;
}

static c1_status connect_profile(const char *ssid, const char *password, c1_wifi_security security,
                                 bool use_saved, const c1_wifi_operation_options *options,
                                 c1_wifi_snapshot *snapshot)
{
    char output[C1_WIFI_OUTPUT_CAPACITY];
    char encoded_ssid[C1_WIFI_SSID_CAPACITY * 2U];
    char encoded_psk[65] = {0};
    char text[256] = {0};
    char actual[C1_WIFI_SSID_CAPACITY];
    char ip[C1_WIFI_IP_CAPACITY];
    connection_backup backup;
    config_backup config = {NULL, 0U};
    int new_id = -1;
    int target_id = -1;
    bool save_attempted = false;
    bool selected = false;
    bool dhcp_started = false;
    size_t password_length;
    c1_status result = C1_STATUS_IO_ERROR;
    begin_operation(options, C1_WIFI_CONNECT_MS);
    current_state = C1_WIFI_CONNECTING;
    if (ssid == NULL || (!use_saved && password == NULL) ||
        !c1_wifi_encode_control_ssid(ssid, encoded_ssid, sizeof(encoded_ssid))) {
        set_error("INVALID NETWORK");
        result = C1_STATUS_INVALID_ARGUMENT;
        goto finished;
    }
    if (!c1_wifi_security_supported(security)) {
        set_error("NETWORK SECURITY UNSUPPORTED");
        result = C1_STATUS_UNSUPPORTED;
        goto finished;
    }
    password_length = password != NULL ? strlen(password) : 0U;
    if (!use_saved && ((security == C1_WIFI_SECURITY_OPEN && password_length != 0U) ||
        (security == C1_WIFI_SECURITY_WPA_PSK && !valid_wpa_passphrase(password)))) {
        set_error("INVALID PASSWORD (PSK: 8-63 ASCII)");
        result = C1_STATUS_INVALID_ARGUMENT;
        goto finished;
    }
    if (!use_saved && security == C1_WIFI_SECURITY_WPA_PSK && !derive_wpa_psk(ssid, password, encoded_psk)) {
        set_error("CREDENTIAL PREPARATION FAILED");
        goto finished;
    }
    if (!continue_operation() || !wifi_io.ready()) {
        result = C1_STATUS_UNAVAILABLE;
        goto finished;
    }
    if (!wifi_io.dhcp_available()) {
        set_error("WLAN0 DHCP OWNED BY ANOTHER SERVICE");
        result = C1_STATUS_UNAVAILABLE;
        goto finished;
    }
    if (!wait_interface_enabled()) {
        result = C1_STATUS_UNAVAILABLE;
        goto finished;
    }
    (void)set_disabled_marker(false);
    explicitly_disabled = false;
    if ((!use_saved && !wifi_io.backup_config(&config)) || !capture_networks(&backup)) {
        set_error("CANNOT PRESERVE NETWORK CONFIG");
        goto finished;
    }
    if (use_saved) {
        target_id = find_saved_profile(&backup, ssid, security);
        if (target_id < 0) {
            set_error(target_id == -2 ? "SAVED NETWORK LOOKUP FAILED" : "SAVED NETWORK MISSING; REFRESH LIST");
            result = C1_STATUS_UNAVAILABLE;
            goto finished;
        }
        goto authenticate;
    }
    if (!command("ADD_NETWORK", output, sizeof(output))) {
        set_error("ADD NETWORK FAILED");
        goto finished;
    }
    output[strcspn(output, "\r\n")] = '\0';
    if (!parse_integer(output, 0, INT_MAX, &new_id)) {
        set_error("INVALID NETWORK ID");
        goto finished;
    }
    snprintf(text, sizeof(text), "SET_NETWORK %d ssid %s", new_id, encoded_ssid);
    if (!command_ok(text)) {
        set_error("SSID REJECTED");
        goto finished;
    }
    snprintf(text, sizeof(text), "SET_NETWORK %d key_mgmt %s", new_id,
             security == C1_WIFI_SECURITY_OPEN ? "NONE" : "WPA-PSK");
    if (!command_ok(text)) {
        set_error("SECURITY REJECTED");
        goto finished;
    }
    if (security == C1_WIFI_SECURITY_WPA_PSK) {
        snprintf(text, sizeof(text), "SET_NETWORK %d psk %s", new_id, encoded_psk);
        if (!command_ok(text)) {
            set_error("CREDENTIAL REJECTED");
            goto finished;
        }
    }
    target_id = new_id;
authenticate:
    current_phase = C1_WIFI_PHASE_AUTHENTICATING;
    /* A command can take effect even if its reply is lost: restore selection
     * on EVERY SELECT attempt, not just acknowledged SELECTs. */
    selected = true;
    if (!network_command("SELECT_NETWORK", target_id)) {
        set_error("CONNECT START FAILED");
        goto finished;
    }
    if (!wait_authenticated(ssid, target_id, 20000)) {
        set_error("TARGET AUTHENTICATION TIMED OUT");
        goto finished;
    }
    current_phase = C1_WIFI_PHASE_ACQUIRING_ADDRESS;
    dhcp_started = true;
    if (!continue_operation() || !wifi_io.dhcp(remaining_ms(10000)) ||
        !command("STATUS", output, sizeof(output)) ||
        !completed_status(output, ssid, target_id, actual, sizeof(actual)) ||
        !wifi_io.ipv4(ip, sizeof(ip))) {
        set_error("TARGET ADDRESS ACQUISITION FAILED");
        goto finished;
    }
    /* SELECT disables all other entries. Restore their original enabled bits
     * before committing so a new network never destroys previous preferences. */
    if (!restore_enabled(&backup, target_id) || !command("STATUS", output, sizeof(output)) ||
        !completed_status(output, ssid, target_id, actual, sizeof(actual))) {
        set_error("NETWORK CHANGED BEFORE SAVE");
        goto finished;
    }
    if (!use_saved) {
        current_phase = C1_WIFI_PHASE_SAVING;
        if (!continue_operation()) goto finished;
        /* Once SAVE is sent, finish the commit rather than cancelling it. */
        active_options = NULL;
        save_attempted = true;
        if (!command_ok("SAVE_CONFIG") || !wifi_io.secure_config()) {
            set_error("SAVE CONFIG FAILED");
            goto finished;
        }
    }
    for (size_t i = 0; i < cached_network_count; ++i) {
        if (strcmp(cached_networks[i].ssid, ssid) == 0 && cached_networks[i].security == security)
            cached_networks[i].saved = true;
    }
    current_state = C1_WIFI_CONNECTED;
    snprintf(cached_connected_ssid, sizeof(cached_connected_ssid), "%s", ssid);
    remember_verified_connection(ssid, ip);
    last_error[0] = '\0';
    result = C1_STATUS_OK;
finished:
    if (result != C1_STATUS_OK && (new_id >= 0 || selected)) {
        bool restored;
        active_options = options;
        restored = rollback_connection(&backup, new_id, selected, dhcp_started);
        /* A SAVE reply can be lost after the daemon wrote the file. Restore
         * the original bytes atomically, even when the reply was ambiguous. */
        if (save_attempted && !wifi_io.restore_config(&config)) restored = false;
        if (!restored) set_error("CONNECT FAILED; RESTORE INCOMPLETE");
    }
    if (config.bytes != NULL) {
        secure_clear(config.bytes, config.size);
        free(config.bytes);
    }
    secure_clear(encoded_psk, sizeof(encoded_psk));
    secure_clear(text, sizeof(text));
    secure_clear(output, sizeof(output));
    return end_operation(result, snapshot);
}

c1_status c1_wifi_connect_ex(const char *ssid, const char *password, c1_wifi_security security,
                              const c1_wifi_operation_options *options, c1_wifi_snapshot *snapshot)
{
    return connect_profile(ssid, password, security, false, options, snapshot);
}

c1_status c1_wifi_connect_saved_ex(const char *ssid, c1_wifi_security security,
                                    const c1_wifi_operation_options *options, c1_wifi_snapshot *snapshot)
{
    return connect_profile(ssid, NULL, security, true, options, snapshot);
}

c1_status c1_wifi_connect(const char *ssid, const char *password, c1_wifi_snapshot *snapshot)
{
    c1_wifi_security security = password != NULL && password[0] != '\0'
                                    ? C1_WIFI_SECURITY_WPA_PSK : C1_WIFI_SECURITY_OPEN;
    bool matched = false;
    size_t index;
    for (index = 0U; ssid != NULL && index < cached_network_count; ++index) {
        if (strcmp(ssid, cached_networks[index].ssid) == 0) {
            if (matched && security != cached_networks[index].security) {
                begin_operation(NULL, C1_WIFI_CONNECT_MS);
                set_error("AMBIGUOUS SECURITY; SELECT TYPE");
                return end_operation(C1_STATUS_UNSUPPORTED, snapshot);
            }
            security = cached_networks[index].security;
            matched = true;
        }
    }
    return c1_wifi_connect_ex(ssid, password, security, NULL, snapshot);
}

c1_status c1_wifi_disable(c1_wifi_snapshot *snapshot)
{
    char output[64];
    c1_status result = C1_STATUS_OK;
    begin_operation(NULL, 5000);
    /* Worker snapshots do not carry process-local control ownership. Re-select
     * the live interface before issuing DISCONNECT, so a fresh UI can stop the
     * firmware's /var/run/wpa_supplicant instance instead of probing only its
     * private socket directory. */
    if (!control_ready_at(C1_WIFI_CTRL_DIR)) {
        using_factory_control = control_ready_at(C1_WIFI_FACTORY_CTRL_DIR);
    } else {
        using_factory_control = false;
    }
    /* Keep the managed supplicant/config alive for pause/resume. No vendor
     * down script and no name-based kill of unrelated interfaces. */
    if (!wifi_io.dhcp_available()) {
        set_error("WLAN0 DHCP OWNED BY ANOTHER SERVICE");
        return end_operation(C1_STATUS_UNAVAILABLE, snapshot);
    }
    if (!command("PING", output, sizeof(output)) || !reply_is(output, "PONG") || !command_ok("DISCONNECT") ||
        !wifi_io.stop_dhcp() || !wifi_io.clear_address() || !hardware_io.enable(false)) {
        set_error("WI-FI DISCONNECT FAILED OR UNMANAGED");
        result = C1_STATUS_UNAVAILABLE;
    } else {
        current_state = C1_WIFI_DISABLED;
        explicitly_disabled = true;
        (void)set_disabled_marker(true);
        cached_network_count = 0U;
        memset(cached_networks, 0, sizeof(cached_networks));
        cached_connected_ssid[0] = '\0';
    }
    return end_operation(result, snapshot);
}

c1_status c1_wifi_pause(bool *was_enabled, bool *was_connected, bool *was_managed)
{
    c1_wifi_snapshot snapshot;
    char saved_ssid[C1_WIFI_SSID_CAPACITY];
    c1_status result;
    if (was_enabled == NULL || was_connected == NULL || was_managed == NULL) return C1_STATUS_INVALID_ARGUMENT;
    *was_managed = control_ready_at(C1_WIFI_CTRL_DIR);
    using_factory_control = false;
    if (!*was_managed && control_ready_at(C1_WIFI_FACTORY_CTRL_DIR)) {
        using_factory_control = true;
        *was_managed = true;
    }
    if (disabled_marker_present()) explicitly_disabled = true;
    *was_enabled = !explicitly_disabled && (current_state != C1_WIFI_DISABLED || *was_managed ||
                                           control_ready_at(C1_WIFI_FACTORY_CTRL_DIR));
    *was_connected = false;
    if (!*was_enabled) return C1_STATUS_OK;
    if (!*was_managed) return C1_STATUS_UNAVAILABLE;
    (void)c1_wifi_read_snapshot(&snapshot);
    *was_connected = snapshot.state == C1_WIFI_CONNECTED;
    snprintf(saved_ssid, sizeof(saved_ssid), "%s", snapshot.connected_ssid);
    result = c1_wifi_disable(NULL);
    if (result == C1_STATUS_OK) snprintf(cached_connected_ssid, sizeof(cached_connected_ssid), "%s", saved_ssid);
    return result;
}

c1_status c1_wifi_resume(bool was_enabled, bool was_connected, bool was_managed)
{
    return c1_wifi_resume_ex(was_enabled, was_connected, was_managed, NULL);
}

c1_status c1_wifi_resume_ex(bool was_enabled, bool was_connected, bool was_managed,
                            const c1_wifi_operation_options *options)
{
    char output[C1_WIFI_OUTPUT_CAPACITY];
    char actual[C1_WIFI_SSID_CAPACITY];
    char ip[C1_WIFI_IP_CAPACITY];
    c1_status result = C1_STATUS_UNAVAILABLE;
    if (!was_enabled) return C1_STATUS_OK;
    begin_operation(options, 30000);
    current_state = C1_WIFI_CONNECTING;
    if (!was_managed) {
        set_error("UNMANAGED WI-FI RESUME UNSUPPORTED");
        goto finished;
    }
    if (!wifi_io.ready() || !wait_interface_enabled()) goto finished;
    (void)set_disabled_marker(false);
    explicitly_disabled = false;
    if (!was_connected) {
        current_state = C1_WIFI_READY;
        result = C1_STATUS_OK;
        goto finished;
    }
    current_phase = C1_WIFI_PHASE_AUTHENTICATING;
    if (!command_ok("RECONNECT") ||
        !wait_authenticated(cached_connected_ssid[0] != '\0' ? cached_connected_ssid : NULL, -1, 15000)) {
        set_error("WI-FI RESUME AUTHENTICATION FAILED");
        goto finished;
    }
    current_phase = C1_WIFI_PHASE_ACQUIRING_ADDRESS;
    if (!wifi_io.dhcp_available() || !wifi_io.dhcp(remaining_ms(10000)) ||
        !command("STATUS", output, sizeof(output)) ||
        !completed_status(output, cached_connected_ssid[0] != '\0' ? cached_connected_ssid : NULL,
                          -1, actual, sizeof(actual)) || !wifi_io.ipv4(ip, sizeof(ip))) {
        set_error("WI-FI RESUME ADDRESS FAILED");
        goto finished;
    }
    snprintf(cached_connected_ssid, sizeof(cached_connected_ssid), "%s", actual);
    current_state = C1_WIFI_CONNECTED;
    remember_verified_connection(actual, ip);
    result = C1_STATUS_OK;
finished:
    return end_operation(result, NULL);
}
