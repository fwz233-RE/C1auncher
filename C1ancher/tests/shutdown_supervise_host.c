/* Host-only fixture: real supervise.c, launcher and IPC; only authenticated
 * update storage is replaced. No signing, rootfs, device or power commands. */
#define _DEFAULT_SOURCE 1
#include "update/supervise.h"
#include "update/boot.h"
#include "update/slot.h"
#include "update/io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static enum c1_update_phase fixture_phase;
static const char *events;

static void event(const char *text)
{
    FILE *stream = fopen(events, "a");
    if (stream == NULL) abort();
    fprintf(stream, "%s\n", text);
    fclose(stream);
}

int c1_update_recover(const char *state, const char *core, const char *key,
                       char *error, size_t size)
{
    (void)state; (void)core; (void)key; (void)error; (void)size;
    return 0;
}
int c1_update_activate(const char *state, const char *core, const char *key,
                        char *error, size_t size)
{
    (void)state; (void)core; (void)key; (void)error; (void)size;
    abort();
}
int c1_update_state_load(const char *root, struct c1_update_state *state,
                         char *error, size_t size)
{
    (void)root; (void)error; (void)size;
    memset(state, 0, sizeof(*state));
    state->phase = fixture_phase;
    strcpy(state->digest, "test-only-digest");
    return 0;
}
int c1_update_validate_current(const char *core, const char *key,
                               const struct c1_update_state *state,
                               char *error, size_t size)
{
    (void)core; (void)key; (void)state; (void)error; (void)size;
    return 0;
}
int c1_update_release_requires_slot_switch(const char *release, const char *key,
                                           int *required, char *error, size_t size)
{
    (void)release; (void)key; (void)error; (void)size;
    *required = 0;
    return 0;
}
int c1_update_ready_matches(const char *ready, const char *digest,
                            char *error, size_t size)
{
    (void)digest; (void)error; (void)size;
    return access(ready, F_OK);
}
int c1_update_confirm(const char *state, const char *core, const char *key,
                       char *error, size_t size)
{
    (void)state; (void)core; (void)key; (void)error; (void)size;
    fixture_phase = C1_UPDATE_CONFIRMED;
    event("confirmed");
    return 0;
}
int c1_update_rollback(const char *state, const char *core, const char *key,
                        char *error, size_t size)
{
    (void)state; (void)core; (void)key; (void)error; (void)size;
    event("rollback");
    /* End the isolated fixture at the rollback boundary. */
    return -1;
}
int c1_update_join_path(char *output, size_t size, const char *root, const char *suffix)
{
    int count = snprintf(output, size, "%s/%s", root, suffix);
    return count >= 0 && (size_t)count < size ? 0 : -1;
}

int main(int argc, char **argv)
{
    char launcher[C1_UPDATE_PATH_MAX], error[C1_UPDATE_ERROR_MAX] = {0};
    int result;
    if (argc != 5) return 99;
    fixture_phase = strcmp(argv[4], "pending") == 0 ? C1_UPDATE_PENDING_BOOT : C1_UPDATE_CONFIRMED;
    events = argv[3];
    (void)snprintf(launcher, sizeof(launcher), "%s/current/artifacts/C1ancher-launcher", argv[1]);
    result = c1_update_supervise(argv[1], argv[1], "unused-fixture-key", argv[2], launcher,
                                 error, sizeof(error));
    if (error[0]) fprintf(stderr, "%s\n", error);
    return result;
}
