#ifndef C1_LAUNCHER_CLEANUP_H
#define C1_LAUNCHER_CLEANUP_H

/* Legacy standalone release maintenance; the deployed partition is untouched
 * by ordinary startup. NULL resolves the running executable. */
int c1_launcher_cleanup_once(const char *executable_path);

/* Automatic post-update maintenance, run in a child so UI/heartbeats continue.
 * Waits for confirmed state matching the running release. Keeps desktop
 * runtime/configuration, current and previous core; deletes other /usr/data
 * entries without following links or crossing mounts. A persisted release
 * identity makes successful cleanup once-per-update, not once-per-boot.
 * Failed/incomplete passes can be retried. Explicit paths are for host tests. */
int c1_launcher_cleanup_after_update(const char *executable_path);

#endif
