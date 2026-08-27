#define _DEFAULT_SOURCE 1

#include "services/ssh.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define C1_SSH_RUN_ROOT "/run/c1"
#define C1_SSH_RUN_DIR "/run/c1/ssh"
#define C1_SSH_SHADOW "/run/c1/ssh/shadow"
#define C1_SSH_CONFIG "/run/c1/ssh/sshd_config"
#define C1_SSH_PID_FILE "/run/c1/ssh/sshd.pid"
#define C1_SSH_HOST_KEY "/usr/data/c1/ssh/ssh_host_ed25519_key"

static char last_error[C1_SSH_ERROR_CAPACITY];
static bool shadow_mounted;

static void set_error(const char *message)
{
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

static bool read_pid(pid_t *pid)
{
    FILE *stream = fopen(C1_SSH_PID_FILE, "r");
    long value;
    bool valid;

    if (stream == NULL) {
        return false;
    }
    valid = fscanf(stream, "%ld", &value) == 1 && value > 1L;
    if (fclose(stream) != 0) {
        valid = false;
    }
    if (!valid) {
        return false;
    }
    *pid = (pid_t)value;
    return true;
}

static bool service_running(void)
{
    pid_t pid;

    return read_pid(&pid) && (kill(pid, 0) == 0 || errno == EPERM);
}

static bool port_is_open(void)
{
    int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    int flags;
    bool open = false;

    if (descriptor < 0) {
        return false;
    }
    flags = fcntl(descriptor, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(22U);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) == 0 || errno == EINPROGRESS) {
        struct timespec interval = {0, 100000000L};
        fd_set writable;
        struct timeval timeout = {1, 0};
        int socket_error = 1;
        socklen_t length = sizeof(socket_error);

        nanosleep(&interval, NULL);
        FD_ZERO(&writable);
        FD_SET(descriptor, &writable);
        if (select(descriptor + 1, NULL, &writable, NULL, &timeout) > 0 &&
            getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 && socket_error == 0) {
            open = true;
        }
    }
    close(descriptor);
    return open;
}

static bool write_temporary_shadow(void)
{
    FILE *source = fopen("/etc/shadow", "r");
    int descriptor;
    FILE *destination;
    char line[1024];
    bool root_written = false;
    bool success = true;

    if (source == NULL) {
        return false;
    }
    descriptor = open(C1_SSH_SHADOW, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0 || (destination = fdopen(descriptor, "w")) == NULL) {
        if (descriptor >= 0) {
            close(descriptor);
        }
        fclose(source);
        return false;
    }
    while (fgets(line, sizeof(line), source) != NULL) {
        if (strncmp(line, "root:", 5U) == 0) {
            char *fields = strchr(line + 5, ':');

            if (fields == NULL || fprintf(destination, "root:%s", fields) < 0) {
                success = false;
                break;
            }
            root_written = true;
        } else if (fputs(line, destination) == EOF) {
            success = false;
            break;
        }
    }
    if (ferror(source) != 0 || fclose(source) != 0 || fflush(destination) != 0 ||
        fsync(fileno(destination)) != 0 || fclose(destination) != 0) {
        success = false;
    }
    return success && root_written;
}

static bool write_sshd_config(void)
{
    static const char content[] =
        "Port 22\n"
        "ListenAddress 0.0.0.0\n"
        "HostKey " C1_SSH_HOST_KEY "\n"
        "PermitRootLogin yes\n"
        "PasswordAuthentication yes\n"
        "PermitEmptyPasswords yes\n"
        "KbdInteractiveAuthentication no\n"
        "PubkeyAuthentication no\n"
        "UsePAM no\n"
        "AllowAgentForwarding no\n"
        "AllowTcpForwarding no\n"
        "X11Forwarding no\n"
        "PermitTunnel no\n"
        "UseDNS no\n"
        "PidFile " C1_SSH_PID_FILE "\n"
        "Subsystem sftp internal-sftp\n";
    int descriptor = open(C1_SSH_CONFIG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    ssize_t count;

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

static bool start_sshd(void)
{
    pid_t child = fork();
    int status;
    unsigned int tick;

    if (child < 0) {
        return false;
    }
    if (child == 0) {
        int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        char *const argv[] = {"/usr/sbin/sshd", "-f", C1_SSH_CONFIG, NULL};

        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) {
                close(null_fd);
            }
        }
        execv("/usr/sbin/sshd", argv);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return false;
    }
    for (tick = 0U; tick < 30U; ++tick) {
        if (service_running() && port_is_open()) {
            return true;
        }
        sleep_milliseconds(100L);
    }
    return false;
}

static bool shadow_is_mounted(void)
{
    FILE *mounts = fopen("/proc/self/mounts", "r");
    char source[256];
    char mountpoint[256];
    char filesystem[64];
    char options[256];
    bool found = false;

    if (mounts == NULL) {
        return shadow_mounted;
    }
    while (fscanf(mounts, "%255s %255s %63s %255s %*d %*d", source, mountpoint, filesystem, options) == 4) {
        if (strcmp(mountpoint, "/etc/shadow") == 0) {
            found = true;
            break;
        }
    }
    fclose(mounts);
    return found;
}

static void cleanup_runtime(void)
{
    if (shadow_is_mounted()) {
        if (umount2("/etc/shadow", MNT_DETACH) == 0 || errno == EINVAL) {
            shadow_mounted = false;
        } else {
            shadow_mounted = true;
        }
    } else {
        shadow_mounted = false;
    }
    unlink(C1_SSH_SHADOW);
    unlink(C1_SSH_CONFIG);
    unlink(C1_SSH_PID_FILE);
}

bool c1_ssh_read_snapshot(c1_ssh_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->enabled = service_running() && port_is_open();
    interface_ipv4(snapshot->ipv4, sizeof(snapshot->ipv4));
    snprintf(snapshot->error, sizeof(snapshot->error), "%s", last_error);
    return true;
}

c1_status c1_ssh_enable(c1_ssh_snapshot *snapshot)
{
    c1_status result = C1_STATUS_IO_ERROR;

    if (service_running()) {
        last_error[0] = '\0';
        c1_ssh_read_snapshot(snapshot);
        return C1_STATUS_OK;
    }
    if (access(C1_SSH_HOST_KEY, R_OK) != 0) {
        set_error("SSH HOST KEY IS MISSING");
        c1_ssh_read_snapshot(snapshot);
        return C1_STATUS_UNAVAILABLE;
    }
    if ((mkdir(C1_SSH_RUN_ROOT, 0700) != 0 && errno != EEXIST) ||
        (mkdir(C1_SSH_RUN_DIR, 0700) != 0 && errno != EEXIST)) {
        set_error("SSH RUNTIME DIR FAILED");
        goto finished;
    }
    if (!write_temporary_shadow()) {
        set_error("TEMP SHADOW FAILED");
        goto finished;
    }
    if (mount(C1_SSH_SHADOW, "/etc/shadow", NULL, MS_BIND, NULL) != 0) {
        set_error("SHADOW BIND FAILED");
        goto finished;
    }
    shadow_mounted = true;
    if (!write_sshd_config() || !start_sshd()) {
        set_error("SSHD START FAILED");
        goto finished;
    }
    last_error[0] = '\0';
    result = C1_STATUS_OK;

finished:
    if (result != C1_STATUS_OK) {
        char failure[C1_SSH_ERROR_CAPACITY];

        snprintf(failure, sizeof(failure), "%s", last_error);
        c1_ssh_disable(NULL);
        snprintf(last_error, sizeof(last_error), "%s", failure);
    }
    c1_ssh_read_snapshot(snapshot);
    return result;
}

c1_status c1_ssh_disable(c1_ssh_snapshot *snapshot)
{
    pid_t pid;
    unsigned int tick;
    c1_status result = C1_STATUS_OK;

    if (read_pid(&pid) && (kill(pid, SIGTERM) == 0 || errno == ESRCH)) {
        for (tick = 0U; tick < 30U; ++tick) {
            if (kill(pid, 0) != 0 && errno == ESRCH) {
                break;
            }
            sleep_milliseconds(100L);
        }
        if (kill(pid, 0) == 0) {
            kill(pid, SIGKILL);
        }
    }
    cleanup_runtime();
    if (shadow_mounted) {
        set_error("SHADOW RESTORE FAILED");
        result = C1_STATUS_IO_ERROR;
    } else {
        last_error[0] = '\0';
    }
    c1_ssh_read_snapshot(snapshot);
    return result;
}