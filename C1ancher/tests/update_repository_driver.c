#define _POSIX_C_SOURCE 200809L
#include "update/repository.h"
#include "update/transaction.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

/* Linked only into this test executable. Production never consults these
 * environment variables and always executes its fixed curl path. */
static struct rlimit transfer_limit;
int __real_setrlimit(int resource, const struct rlimit *limit);
int __wrap_setrlimit(int resource, const struct rlimit *limit)
{
    if (resource != RLIMIT_FSIZE) return __real_setrlimit(resource, limit);
    /* Defer the identical production bound until after test-only logging. */
    transfer_limit = *limit;
    return 0;
}

int __real_execv(const char *path, char *const argv[]);
int __wrap_execv(const char *path, char *const argv[])
{
    const char *port = getenv("C1_TEST_HTTP_PORT");
    const char *mode = getenv("C1_TEST_PRIMARY_FAILURE");
    const char *log = getenv("C1_TEST_CURL_LOG");
    char resolve[128], primary[128], fallback[128];
    char *arguments[96];
    size_t count = 0U, i;
    int is_primary = 0;
    for (i = 0U; argv[i] != NULL; ++i) {
        if (strncmp(argv[i], "http://www.fwz233.com/", 22U) == 0) is_primary = 1;
    }
    if (log != NULL) {
        FILE *stream = fopen(log, "a");
        if (stream == NULL) _exit(126);
        for (i = 0U; argv[i] != NULL; ++i) fprintf(stream, "%s\t", argv[i]);
        fputc('\n', stream);
        fclose(stream);
    }
    if (is_primary && mode != NULL) {
        if (strcmp(mode, "dns") == 0) _exit(6);
        if (strcmp(mode, "connect") == 0) _exit(7);
        if (strcmp(mode, "cancel") == 0) { raise(SIGTERM); _exit(126); }
    }
    if (__real_setrlimit(RLIMIT_FSIZE, &transfer_limit) != 0) _exit(126);
    if (port == NULL) return __real_execv(path, argv);
    (void)snprintf(resolve, sizeof(resolve), "www.fwz233.com:%s:127.0.0.1", port);
    (void)snprintf(primary, sizeof(primary), "www.fwz233.com:80:www.fwz233.com:%s", port);
    (void)snprintf(fallback, sizeof(fallback), "123.56.214.77:80:127.0.0.1:%s", port);
    /* --disable stays first. All test origins resolve/connect only to loopback. */
    arguments[count++] = argv[0];
    arguments[count++] = argv[1];
    arguments[count++] = "--noproxy";
    arguments[count++] = "*";
    arguments[count++] = "--resolve";
    arguments[count++] = resolve;
    arguments[count++] = "--connect-to";
    arguments[count++] = primary;
    arguments[count++] = "--connect-to";
    arguments[count++] = fallback;
    for (i = 2U; argv[i] != NULL && count + 1U < sizeof(arguments) / sizeof(arguments[0]); ++i)
        arguments[count++] = argv[i];
    if (argv[i] != NULL) _exit(126);
    arguments[count] = NULL;
    return __real_execv(path, arguments);
}

int main(int argc, char **argv)
{
    char error[C1_UPDATE_ERROR_MAX] = "";
    uint64_t size;
    int status;
    if (argc == 5 && strcmp(argv[1], "release") == 0) {
        struct c1_update_release release = {0};
        status = c1_update_repository_fetch_release(argv[2], argv[3], argv[4], &release,
                                                    error, sizeof(error));
        c1_update_repository_release_free(&release);
    } else if (argc == 7 && strcmp(argv[1], "prepare") == 0) {
        struct c1_update_transaction_config config = {0};
        struct c1_update_transaction_result result;
        config.staging_root = argv[3];
        config.core_root = argv[4];
        config.state_root = argv[5];
        config.key_path = argv[6];
        status = c1_update_prepare(&config, argv[2], &result, error, sizeof(error));
    } else {
        if (argc != 4) return 2;
        size = strtoull(argv[3], NULL, 10);
        status = c1_update_repository_download(argv[1], "artifacts/C1ancher", argv[2],
                                                size, size, error, sizeof(error));
    }
    if (status != 0) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    return 0;
}
