/*
 * c1pkg is dependency-free at link time. Ed25519 index verification is built
 * in; repository operations fail closed unless curl, sha256sum, and tar are
 * executable. A trusted raw 32-byte Ed25519 public key must be provisioned at
 * C1PKG_KEY_DEFAULT (or supplied with --key/C1PKG_KEY).
 */
#include "pkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream)
{
    (void)fprintf(stream,
        "usage: c1pkg [--repo URL] [--key RAW_ED25519_KEY] COMMAND [ARGS]\n"
        "commands:\n"
        "  tui                    interactive 49x19 package browser\n"
        "  refresh                fetch and verify repository index\n"
        "  list                   list installed packages\n"
        "  available              list verified repository packages\n"
        "  install ID             install/update ID from verified index\n"
        "  update ID              alias for install\n"
        "  remove ID              atomically uninstall ID\n"
        "  rollback ID            swap current and previous versions\n"
        "  launch ID [ARGS...]    exec installed entry point directly\n"
        "runtime helpers: curl, sha256sum, tar; signatures are verified in-process\n");
}

static int load_repository(const struct c1pkg_config *config, struct c1pkg_index *index,
                           char *error, size_t error_size)
{
    if (c1pkg_repo_refresh(config, index, error, error_size) == 0) {
        return 0;
    }
    return -1;
}

static int command_list(void)
{
    struct c1pkg_installed_list list;
    char error[C1PKG_ERROR_MAX] = "";
    size_t i;

    if (c1pkg_store_list(&list, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "c1pkg: %s\n", error);
        return 1;
    }
    for (i = 0U; i < list.count; ++i) {
        (void)printf("%s\t%s\n", list.items[i].id, list.items[i].version);
    }
    return 0;
}

static int command_available(const struct c1pkg_config *config)
{
    struct c1pkg_index index;
    char error[C1PKG_ERROR_MAX] = "";
    size_t i;

    if (load_repository(config, &index, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "c1pkg: %s\n", error);
        return 1;
    }
    for (i = 0U; i < index.count; ++i) {
        const struct c1pkg_package *package = &index.packages[i];
        (void)printf("%s\t%s\t%s\t%llu\n", package->id, package->version,
                     package->name, (unsigned long long)package->size);
    }
    return 0;
}

static int command_install(const struct c1pkg_config *config, const char *id)
{
    struct c1pkg_index index;
    const struct c1pkg_package *package;
    char error[C1PKG_ERROR_MAX] = "";

    if (!c1pkg_safe_id(id)) {
        (void)fprintf(stderr, "c1pkg: unsafe application ID\n");
        return 1;
    }
    if (load_repository(config, &index, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "c1pkg: %s\n", error);
        return 1;
    }
    package = c1pkg_repo_find(&index, id);
    if (package == NULL) {
        (void)fprintf(stderr, "c1pkg: package not found in verified index: %s\n", id);
        return 1;
    }
    if (c1pkg_store_install(config, package, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "c1pkg: %s\n", error);
        return 1;
    }
    (void)printf("installed %s %s\n", package->id, package->version);
    return 0;
}

int main(int argc, char **argv)
{
    struct c1pkg_config config;
    const char *environment;
    const char *command;
    char error[C1PKG_ERROR_MAX] = "";
    int argument = 1;

    environment = getenv("C1PKG_REPO");
    config.repo_base = environment != NULL && environment[0] != '\0' ?
                       environment : C1PKG_REPO_DEFAULT;
    environment = getenv("C1PKG_KEY");
    config.public_key = environment != NULL && environment[0] != '\0' ?
                        environment : C1PKG_KEY_DEFAULT;

    while (argument < argc && strncmp(argv[argument], "--", 2U) == 0) {
        if (strcmp(argv[argument], "--repo") == 0 && argument + 1 < argc) {
            config.repo_base = argv[argument + 1];
            argument += 2;
        } else if (strcmp(argv[argument], "--key") == 0 && argument + 1 < argc) {
            config.public_key = argv[argument + 1];
            argument += 2;
        } else if (strcmp(argv[argument], "--help") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (argument >= argc) {
        usage(stderr);
        return 2;
    }
    command = argv[argument++];
    if (strcmp(command, "tui") == 0 && argument == argc) {
        return c1pkg_tui(&config);
    }
    if (strcmp(command, "refresh") == 0 && argument == argc) {
        struct c1pkg_index index;
        if (c1pkg_repo_refresh(&config, &index, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "c1pkg: %s\n", error);
            return 1;
        }
        (void)printf("verified %lu package(s)\n", (unsigned long)index.count);
        return 0;
    }
    if (strcmp(command, "list") == 0 && argument == argc) {
        return command_list();
    }
    if (strcmp(command, "available") == 0 && argument == argc) {
        return command_available(&config);
    }
    if ((strcmp(command, "install") == 0 || strcmp(command, "update") == 0) &&
        argument + 1 == argc) {
        return command_install(&config, argv[argument]);
    }
    if (strcmp(command, "remove") == 0 && argument + 1 == argc) {
        if (c1pkg_store_remove(argv[argument], error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "c1pkg: %s\n", error);
            return 1;
        }
        return 0;
    }
    if (strcmp(command, "rollback") == 0 && argument + 1 == argc) {
        if (c1pkg_store_rollback(argv[argument], error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "c1pkg: %s\n", error);
            return 1;
        }
        return 0;
    }
    if (strcmp(command, "launch") == 0 && argument < argc) {
        return c1pkg_store_launch(argv[argument], &argv[argument + 1],
                                  error, sizeof(error)) == 0 ? 0 :
               ((void)fprintf(stderr, "c1pkg: %s\n", error), 1);
    }
    usage(stderr);
    return 2;
}