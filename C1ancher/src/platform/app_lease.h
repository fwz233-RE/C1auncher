#ifndef C1_PLATFORM_APP_LEASE_H
#define C1_PLATFORM_APP_LEASE_H

#include <stdbool.h>

#ifndef C1_APP_RUN_PATH
#define C1_APP_RUN_PATH "/dev/shm/c1ancher-external-app.runlock"
#endif
#ifndef C1_APP_MODE_GUARD_PATH
#define C1_APP_MODE_GUARD_PATH "/dev/shm/c1ancher-external-app.mode-guard"
#endif
#ifndef C1_APP_LEASE_PATH
#define C1_APP_LEASE_PATH "/dev/shm/c1ancher-external-app.lock"
#endif
#ifndef C1_APP_MODE_PATH
#define C1_APP_MODE_PATH "/dev/shm/c1ancher-external-app.mode"
#endif

/* Run lock is inherited by all apps; lease only excludes hardware owners. */
int c1_app_run_acquire(void);
bool c1_app_run_active(void);
int c1_app_run_acquire_at(const char *path);
bool c1_app_run_active_at(const char *path);

int c1_app_lease_acquire(void);
int c1_app_lease_guard_acquire(void);
bool c1_app_lease_active(void);
void c1_app_lease_release(int descriptor);

int c1_app_lease_acquire_at(const char *path);
int c1_app_lease_guard_acquire_at(const char *path);
bool c1_app_lease_active_at(const char *path);

bool c1_app_lease_write_mode(const char *mode);
void c1_app_lease_clear_mode(void);
bool c1_app_lease_terminal_mode(void);

#endif
