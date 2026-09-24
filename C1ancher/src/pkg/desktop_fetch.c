#define _DEFAULT_SOURCE 1
#include "pkg/desktop.h"
#include "services/desktop_data.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t cancelled;
static int64_t fetch_deadline;
static void cancel_fetch(int signal_number) { (void)signal_number; cancelled = 1; }
static int progress(const char *message, void *context)
{
    (void)message; (void)context;
    struct timespec now;
    return cancelled || clock_gettime(CLOCK_MONOTONIC, &now) ||
        (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 >= fetch_deadline;
}

int c1pkg_desktop_summary(const struct c1pkg_config *config)
{
    c1_desktop_data data = {0}, old;
    char source[1025], url[1100], directory[] = "/tmp/c1-desktop-XXXXXX", path[128];
    char error[C1PKG_ERROR_MAX] = "", body[C1_DESKTOP_BODY_MAX + 1];
    struct sigaction action, old_term, old_int;
    struct timespec now;
    if (!config || c1pkg_desktop_source(config->repo_base, source, sizeof(source)) ||
        snprintf(url, sizeof(url), "%s/desktop.v1", source) >= (int)sizeof(url) || !mkdtemp(directory)) return 1;
    memset(&action, 0, sizeof(action)); action.sa_handler = cancel_fetch; sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, &old_term); (void)sigaction(SIGINT, &action, &old_int);
    clock_gettime(CLOCK_MONOTONIC, &now);
    fetch_deadline = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + 35000;
    cancelled = 0; c1pkg_set_progress(progress, NULL);
    snprintf(path, sizeof(path), "%s/body", directory);
    int result = 1;
    if (c1pkg_fetch(url, path, C1_DESKTOP_BODY_MAX, error, sizeof(error)) == 0) {
        FILE *file = fopen(path, "rb");
        if (file) {
            size_t n = fread(body, 1, sizeof(body), file);
            bool ok = !ferror(file) && c1_desktop_parse(body, n, &data);
            fclose(file);
            if (ok) {
                strcpy(data.source, source);
                if (c1_desktop_load(C1_DESKTOP_CACHE, &old) && !strcmp(old.source, source) && old.sequence > data.sequence)
                    goto done;
                if (c1_desktop_save(C1_DESKTOP_CACHE, &data)) result = 0;
            }
        }
    }
done:
    c1pkg_set_progress(NULL, NULL);
    (void)sigaction(SIGTERM, &old_term, NULL); (void)sigaction(SIGINT, &old_int, NULL);
    unlink(path); rmdir(directory);
    return result;
}
