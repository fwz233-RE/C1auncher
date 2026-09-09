/* SPDX-License-Identifier: GPL-3.0-only */
#include "session.h"
#include "platform/app_lease.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* Private compile-time paths; never touches actual launcher locks. Tests use
 * the launcher's exact lease implementation as the competing display guard. */
static void check_guard(bool available) {
    int guard = c1_app_lease_guard_acquire();
    assert((guard >= 0) == available);
    if (guard >= 0) close(guard);
}
static void wait_ok(pid_t pid) {
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--inherited")) {
        assert(iw_session_open() == 0);
        check_guard(false);
        iw_session_close();
        check_guard(false); /* c1pkg's inherited original is still held */
        return 0;
    }
    const char *directory = "/tmp/inkwars-session-tests";
    assert(mkdir(directory, 0700) == 0 || errno == EEXIST);
    struct stat info;
    assert(lstat(directory, &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == geteuid());
    assert(unlink(C1_APP_LEASE_PATH) == 0 || errno == ENOENT);
    check_guard(true);
    int master, slave;
    assert(openpty(&master, &slave, NULL, NULL, NULL) == 0);
    int saved_stdin = dup(STDIN_FILENO);
    assert(saved_stdin >= 0 && dup2(slave, STDIN_FILENO) == STDIN_FILENO);
    struct termios original, quiet, restored;
    assert(tcgetattr(STDIN_FILENO, &original) == 0);
    original.c_lflag |= ECHO | ECHONL | ICANON;
    assert(tcsetattr(STDIN_FILENO, TCSANOW, &original) == 0);
    assert(iw_session_open() == 0);
    assert(tcgetattr(STDIN_FILENO, &quiet) == 0);
    assert(!(quiet.c_lflag & (ECHO | ECHONL | ICANON)));
    check_guard(false);
    assert(iw_session_open() < 0 && errno == EALREADY);
    iw_session_close();
    assert(tcgetattr(STDIN_FILENO, &restored) == 0);
    assert(restored.c_lflag == original.c_lflag);
    check_guard(true);
    assert(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO);
    close(saved_stdin); close(master); close(slave);

    /* Real exec inheritance: c1pkg direct launch must not deadlock itself. */
    int inherited = c1_app_lease_acquire();
    assert(inherited >= 0);
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { execl(argv[0], argv[0], "--inherited", (char *)NULL); _exit(99); }
    wait_ok(pid);
    check_guard(false);
    close(inherited);
    check_guard(true);

    /* Another process owns the hardware. Fail closed, no lock stealing. */
    int ready[2], finish[2]; assert(pipe(ready) == 0 && pipe(finish) == 0);
    pid = fork(); assert(pid >= 0);
    if (!pid) {
        close(ready[0]); close(finish[1]);
        assert(iw_session_open() == 0);
        assert(write(ready[1], "1", 1) == 1);
        char c; assert(read(finish[0], &c, 1) == 1);
        iw_session_close(); _exit(0);
    }
    close(ready[1]); close(finish[0]);
    char c; assert(read(ready[0], &c, 1) == 1);
    check_guard(false);
    assert(iw_session_open() < 0);
    assert(kill(pid, SIGSTOP) == 0);
    int stopped; assert(waitpid(pid, &stopped, WUNTRACED) == pid && WIFSTOPPED(stopped));
    check_guard(false); /* sleeping game still owns screen */
    assert(kill(pid, SIGCONT) == 0);
    assert(write(finish[1], "1", 1) == 1);
    wait_ok(pid); close(ready[0]); close(finish[1]);
    check_guard(true);
    /* Unknown or unsafe lease file must not enable uncoordinated writes. */
    assert(chmod(C1_APP_LEASE_PATH, 0644) == 0);
    assert(iw_session_open() < 0);
    assert(unlink(C1_APP_LEASE_PATH) == 0);
    assert(rmdir(directory) == 0);
    puts("PASS session: launcher guard exclusion, exec inheritance, PTY echo off/restore, competing owner refusal, suspend lock retention");
    return 0;
}
