#define _DEFAULT_SOURCE 1
#include "platform/child_processes.h"
#include "platform/liveness.h"
#include <assert.h>
#include <stdio.h>
#include <sys/prctl.h>

static pid_t sleeper(void)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        for (;;) pause();
    }
    return child;
}

static void run_cases(void)
{
    pid_t primary, doomed, middle, adopted;
    int status, channel[2], remaining, attempts;
    assert(c1_descendants_adopt() == 0);
    assert(c1_descendants_cleanup(1000));

    primary = sleeper();
    doomed = sleeper();
    remaining = 1;
    for (attempts = 0; attempts < 200 && remaining != 0; ++attempts) {
        remaining = c1_child_processes_signal(primary, SIGKILL);
        assert(remaining >= 0);
        if (remaining) usleep(10000);
    }
    assert(remaining == 0);
    assert(kill(primary, 0) == 0);
    assert(waitpid(doomed, NULL, WNOHANG) < 0 && errno == ECHILD);
    assert(kill(primary, SIGKILL) == 0);
    assert(waitpid(primary, &status, 0) == primary);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

    assert(pipe(channel) == 0);
    middle = fork();
    assert(middle >= 0);
    if (middle == 0) {
        pid_t descendant = fork();
        if (descendant < 0) _exit(90);
        if (descendant == 0) {
            pid_t own = getpid();
            close(channel[0]);
            assert(setsid() >= 0);
            assert(signal(SIGTERM, SIG_IGN) != SIG_ERR);
            assert(write(channel[1], &own, sizeof(own)) == (ssize_t)sizeof(own));
            close(channel[1]);
            for (;;) pause();
        }
        _exit(0);
    }
    close(channel[1]);
    assert(read(channel[0], &adopted, sizeof(adopted)) == (ssize_t)sizeof(adopted));
    close(channel[0]);
    assert(waitpid(middle, &status, 0) == middle && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(c1_descendants_cleanup(2000));
    assert(kill(adopted, 0) < 0 && errno == ESRCH);
    assert(c1_descendants_cleanup(1000));
}

int main(void)
{
    pid_t unrelated = sleeper();
    pid_t worker = fork();
    int status;
    assert(worker >= 0);
    if (worker == 0) {
        run_cases();
        _exit(0);
    }
    assert(waitpid(worker, &status, 0) == worker);
    assert(kill(unrelated, 0) == 0);
    assert(kill(unrelated, SIGKILL) == 0);
    assert(waitpid(unrelated, NULL, 0) == unrelated);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("child process cleanup passed: empty, direct, excluded primary, adopted setsid, unrelated process preserved");
    return 0;
}
