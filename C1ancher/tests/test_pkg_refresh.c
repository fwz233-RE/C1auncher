/* Integrate the actual signed repository/cache implementation with the GUI
 * worker. Loopback HTTP only; inherited repo fixture virtualizes cooldown time,
 * not child processes, network transfer, advisory locks or cancellation. */
#define main repository_fixture_main
#include "test_pkg_repo.c"
#undef main
#include "../src/pkg/gui_refresh.h"

static int finish_job(struct gui_refresh_job *job)
{
    uint64_t deadline = c1pkg_input_now_ms() + 10000U;
    int result = 0;
    while (job->pid > 0 && c1pkg_input_now_ms() < deadline) {
        result = gui_refresh_poll(job, NULL, 0U);
        if (result == 1 || result == -1) break;
        struct pollfd event = {job->result_fd, POLLIN, 0};
        (void)poll(&event, 1U, 5);
    }
    expect(job->pid == 0, "real repository worker completes/reaps within test deadline");
    return result;
}
static pid_t worker_helper(pid_t worker)
{
    char path[128]; long pid = 0;
    snprintf(path, sizeof(path), "/proc/%ld/task/%ld/children", (long)worker, (long)worker);
    FILE *file = fopen(path, "r");
    if (file) { if (fscanf(file, "%ld", &pid) != 1) pid = 0; fclose(file); }
    return (pid_t)pid;
}
static void no_temporary_files(void)
{
    DIR *directory = opendir(C1PKG_STATE_ROOT "/cache"); struct dirent *entry;
    expect(directory != NULL, "cache directory exists");
    if (!directory) return;
    while ((entry = readdir(directory)) != NULL)
        expect(strstr(entry->d_name, ".tmp") == NULL && strstr(entry->d_name, ".headers.") == NULL,
               "cancelled worker removes only its owned repository/HTTP temporary files");
    closedir(directory);
}
int main(void)
{
    unsigned char seed[32] = {7};
    char text[2048], base[256], error[256] = "";
    struct c1pkg_config config = {base, C1PKG_STATE_ROOT "/key"};
    struct c1pkg_index cache = {0};
    struct gui_refresh_job job;
    pid_t server;
    if (mkdir(C1PKG_STATE_ROOT, 0700)) { perror("isolated refresh directory"); return 2; }
    write_text(C1PKG_STATE_ROOT "/curl-shim", "#!/bin/sh\nexec /usr/bin/curl \"$@\"\n");
    expect(chmod(C1PKG_STATE_ROOT "/curl-shim", 0700) == 0, "configure loopback-only curl helper");
    ed25519_create_keypair(public_key, private_key, seed);
    expect(c1pkg_write_file(config.public_key, public_key, sizeof(public_key), 0600, NULL, 0U) == 0, "write trusted fixture key");
    gui_refresh_init(&job);
    index_text(text, sizeof(text), 2, 10U, "Signed Author");
    server = server_start(base, sizeof(base), 0, text);
    expect(gui_refresh_start(&job, &config) == 1 && finish_job(&job) == 1 &&
           job.result->result == 0 && job.result->index.sequence == 10U,
           "full pipe result originates from actual Ed25519 verification and atomic cache commit");
    gui_refresh_dispose(&job); server_stop(server);
    expect(c1pkg_repo_load_cached(&config, &cache, error, sizeof(error)) == 0 && cache.sequence == 10U,
           "signed cache remains independently verifiable after worker exits");
    /* A writer holds the real repo.lock while a real curl helper is stalled.
     * Competing callers fail before touching shared temporary files. */
    for (int iteration = 0; iteration < 4; ++iteration) {
        index_text(text, sizeof(text), 2, 11U, "Next Author");
        server = server_start(base, sizeof(base), 5, text);
        expect(gui_refresh_start(&job, &config) == 1, "start stalled refresh");
        uint64_t deadline = c1pkg_input_now_ms() + 3000U;
        pid_t helper = 0, worker = job.pid;
        while (!helper && c1pkg_input_now_ms() < deadline) {
            (void)gui_refresh_poll(&job, NULL, 0U);
            helper = worker_helper(worker);
            (void)poll(NULL, 0U, 5);
        }
        expect(helper > 0, "actual curl helper is running under refresh worker");
        expect(c1pkg_repo_refresh(&config, &cache, error, sizeof(error)) != 0 && strstr(error, "another repository") != NULL,
               "real advisory lock rejects a competing cache writer before temp deletion");
        expect(c1pkg_repo_load_cached(&config, &cache, error, sizeof(error)) == 0 && cache.sequence == 10U,
               "stalled refresh preserves previously signed cache");
        uint64_t cancelled = c1pkg_input_now_ms();
        gui_refresh_dispose(&job);
        expect(c1pkg_input_now_ms() - cancelled < 750U, "cooperative cancellation stops stalled real curl promptly");
        expect(kill(helper, 0) == -1 && errno == ESRCH, "repository helper is killed and reaped, not orphaned");
        expect(waitpid(worker, NULL, WNOHANG) == -1 && errno == ECHILD, "GUI worker is reaped exactly once");
        no_temporary_files();
        expect(c1pkg_repo_load_cached(&config, &cache, error, sizeof(error)) == 0 && cache.sequence == 10U,
               "cancellation cannot damage the retained signed catalog");
        server_stop(server);
    }
    /* A successor can commit after cancellation: no parent-side cleanup may
     * race it and unlink index.tmp/signature.tmp belonging to another writer. */
    server = server_start(base, sizeof(base), 0, text);
    expect(gui_refresh_start(&job, &config) == 1 && finish_job(&job) == 1 && job.result->result == 0 && job.result->index.sequence == 11U,
           "successor safely obtains lock and commits newer verified cache");
    gui_refresh_dispose(&job); server_stop(server);
    index_text(text, sizeof(text), 2, 9U, "Old Author");
    server = server_start(base, sizeof(base), 0, text);
    expect(gui_refresh_start(&job, &config) == 1 && finish_job(&job) == 1 && job.result->result != 0 && !job.result->index.count,
           "rollback failure sends no installable records over result pipe");
    gui_refresh_dispose(&job); server_stop(server);
    index_text(text, sizeof(text), 2, 12U, "Invalid Author");
    server = server_start(base, sizeof(base), 2, text);
    expect(gui_refresh_start(&job, &config) == 1 && finish_job(&job) == 1 && job.result->result != 0 && !job.result->index.count,
           "bad signature sends no installable records over result pipe");
    gui_refresh_dispose(&job); server_stop(server);
    expect(c1pkg_repo_load_cached(&config, &cache, error, sizeof(error)) == 0 && cache.sequence == 11U,
           "rollback and bad signatures preserve newer trusted snapshot");
    expect(c1pkg_remove_tree(C1PKG_STATE_ROOT, NULL, 0U) == 0, "remove isolated test cache");
    if (failures) return 1;
    puts("PASS: real signed async cache, curl cleanup, repo.lock contention, successor and rollback/signature rejection");
    return 0;
}
