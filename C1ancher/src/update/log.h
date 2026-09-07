#ifndef C1_UPDATE_LOG_H
#define C1_UPDATE_LOG_H

/* argv is a NULL-terminated command vector. Logging is always best effort. */
int c1_update_run_logged(const char *path, const char *limit,
                         const char *rotations, char *const argv[]);

#endif
