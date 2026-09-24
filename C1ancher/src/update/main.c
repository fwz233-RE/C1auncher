#include "update/update.h"
#include "update/log.h"
#include "update/boot.h"
#include "update/slot.h"
#include "update/supervise.h"
#include "update/repository.h"
#include "update/check.h"
#include "update/transaction.h"
#include "security/secure_file.h"
#include "security/trusted_ed25519.h"
#include "platform/update_request.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream)
{
    fputs("Usage:\n"
          "  c1updater --help | --self-test\n"
          "  c1updater --recovery-version | --log-version\n"
          "  c1updater run-logged LOGFILE LIMIT ROTATIONS command args...\n"
          "  c1updater verify-current CORE_ROOT KEY\n"
          "  c1updater recovery-supervise STATE_ROOT CORE_ROOT KEY current|previous READY_FILE LAUNCHER_PATH\n"
          "  c1updater tui\n"
          "  c1updater tui-prepared (show prepared update only; never checks network)\n"
          "  c1updater verify-signature PAYLOAD SIGNATURE KEY\n"
          "  c1updater verify-manifest MANIFEST SIGNATURE KEY\n"
          "  c1updater state ROOT\n"
          "  c1updater check-configured (signed metadata only; never prepares/downloads components)\n"
          "  c1updater prepare-local RELEASE_DIR STAGING_ROOT CORE_ROOT STATE_ROOT KEY\n"
          "  c1updater prepare URL STAGING_ROOT CORE_ROOT STATE_ROOT KEY\n"
          "  c1updater prepare-configured STAGING_ROOT CORE_ROOT STATE_ROOT KEY\n"
          "  c1updater activate STATE_ROOT CORE_ROOT KEY\n"
          "  c1updater bootstrap-activate STATE_ROOT CORE_ROOT KEY\n"
          "  c1updater recover STATE_ROOT CORE_ROOT KEY\n"
          "  c1updater confirm STATE_ROOT CORE_ROOT KEY\n"
          "  c1updater rollback STATE_ROOT CORE_ROOT KEY\n"
          "  c1updater mark-ready STATE_ROOT READY_FILE\n"
          "  c1updater supervise STATE_ROOT CORE_ROOT KEY READY_FILE LAUNCHER_PATH\n"
          "  c1updater activate-updater-slot UPDATE_ROOT ACTIVE_SLOT\n"
          "  c1updater install-prepared-slot SLOT_ROOT ACTIVE_SLOT CORE_ROOT STATE_ROOT KEY\n"
          "  c1updater install-inactive-slot SLOT_ROOT ACTIVE_SLOT PREPARED_RELEASE KEY\n",
          stream);
}

static int verify_signature(const char *payload_path, const char *signature_path,
                            const char *key_path)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    char error[C1_UPDATE_ERROR_MAX] = "";
    int result = EXIT_FAILURE;
    if (c1_secure_read_file(payload_path, &data, &size, C1_ED25519_PAYLOAD_MAX,
                            C1_SECURE_FILE_ANY_SIZE, error, sizeof(error)) != 0 ||
        c1_trusted_ed25519_verify_files(key_path, signature_path, data, size,
                                       error, sizeof(error)) != 0) {
        fprintf(stderr, "verification failed: %s\n", error[0] != '\0' ? error : "rejected");
        goto done;
    }
    puts("signature verified");
    result = EXIT_SUCCESS;
done:
    free(data);
    return result;
}

static int verify_manifest(const char *manifest_path, const char *signature_path,
                           const char *key_path)
{
    struct c1_update_manifest manifest;
    unsigned char *data = NULL;
    size_t size = 0U;
    char error[C1_UPDATE_ERROR_MAX] = "";
    int result = EXIT_FAILURE;
    if (c1_secure_read_file(manifest_path, &data, &size, C1_UPDATE_MANIFEST_MAX,
                            C1_SECURE_FILE_ANY_SIZE, error, sizeof(error)) != 0 ||
        c1_trusted_ed25519_verify_files(key_path, signature_path, data, size,
                                       error, sizeof(error)) != 0 ||
        c1_update_parse_manifest(data, size, &manifest, error, sizeof(error)) != 0) {
        fprintf(stderr, "verification failed: %s\n", error[0] != '\0' ? error : "rejected");
        goto done;
    }
    printf("manifest verified: sequence=%llu version=%s security_epoch=%llu\n",
           (unsigned long long)manifest.sequence, manifest.version,
           (unsigned long long)manifest.security_epoch);
    result = EXIT_SUCCESS;
done:
    free(data);
    return result;
}

static int show_state(const char *root)
{
    struct c1_update_state state;
    char error[C1_UPDATE_ERROR_MAX] = "";
    if (c1_update_state_load(root, &state, error, sizeof(error)) != 0) {
        fprintf(stderr, "state load failed: %s\n", error[0] != '\0' ? error : "rejected");
        return EXIT_FAILURE;
    }
    if (state.generation == 0U) puts("state: idle (no committed generation)");
    else printf("state: generation=%llu phase=%s sequence=%llu security_epoch=%llu release=%s\n",
                (unsigned long long)state.generation, c1_update_phase_name(state.phase),
                (unsigned long long)state.sequence, (unsigned long long)state.security_epoch,
                state.release);
    return EXIT_SUCCESS;
}

static int check_configured(void)
{
    struct c1_update_check_result result;
    char repository[C1_UPDATE_URL_MAX + 1U];
    char error[C1_UPDATE_ERROR_MAX] = "";
    if (c1_update_repository_read_url(C1_UPDATE_REPOSITORY_CONFIG, repository,
                                      sizeof(repository), error, sizeof(error)) != 0 ||
        c1_update_check(repository, C1_UPDATE_DEFAULT_STAGING_ROOT,
                         C1_UPDATE_DEFAULT_STATE_ROOT, C1_UPDATE_DEFAULT_KEY,
                         &result, error, sizeof(error)) != 0) {
        fprintf(stderr, "check failed: %s\n", error[0] != '\0' ? error : "rejected");
        return EXIT_FAILURE;
    }
    /* Emit only a complete successful, verified result after staging cleanup.
     * Consumers must require exit status 0 and exactly this versioned protocol. */
    if (printf("C1UPDATE-CHECK 1\nS\t%llu\nV\t%s\nA\t%d\n",
               (unsigned long long)result.sequence, result.version, result.available) < 0 ||
        fflush(stdout) != 0) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

static int prepare_release(const char *source, int local, const char *staging_root,
                           const char *core_root, const char *state_root,
                           const char *key_path)
{
    struct c1_update_transaction_config config;
    struct c1_update_transaction_result result;
    char error[C1_UPDATE_ERROR_MAX] = "";
    int status;
    (void)memset(&config, 0, sizeof(config));
    config.staging_root = staging_root;
    config.core_root = core_root;
    config.state_root = state_root;
    config.key_path = key_path;
    status = local != 0 ? c1_update_prepare_local(&config, source, &result, error, sizeof(error))
                        : c1_update_prepare(&config, source, &result, error, sizeof(error));
    if (status != 0) {
        fprintf(stderr, "prepare failed: %s\n", error[0] != '\0' ? error : "rejected");
        return status == C1_UPDATE_BUSY ? C1_UPDATER_TRANSIENT_EXIT : EXIT_FAILURE;
    }
    if (result.phase == C1_UPDATE_CONFIRMED) puts("Core is up-to-date.");
    printf("phase=%s release=%s sequence=%llu\n", c1_update_phase_name(result.phase),
           result.release, (unsigned long long)result.sequence);
    return EXIT_SUCCESS;
}

static int boot_command(const char *command, const char *state_root,
                        const char *core_root, const char *key)
{
    char error[C1_UPDATE_ERROR_MAX] = "";
    int result;
    if (strcmp(command, "activate") == 0) result = c1_update_activate(state_root, core_root, key, error, sizeof(error));
    else if (strcmp(command, "bootstrap-activate") == 0) result = c1_update_bootstrap_activate(state_root, core_root, key, error, sizeof(error));
    else if (strcmp(command, "recover") == 0) result = c1_update_recover(state_root, core_root, key, error, sizeof(error));
    else if (strcmp(command, "confirm") == 0) result = c1_update_confirm(state_root, core_root, key, error, sizeof(error));
    else result = c1_update_rollback(state_root, core_root, key, error, sizeof(error));
    if (result != 0) fprintf(stderr, "%s failed: %s\n", command, error[0] != '\0' ? error : "rejected");
    return result == 0 ? EXIT_SUCCESS : result == C1_UPDATE_BUSY ? C1_UPDATER_TRANSIENT_EXIT : EXIT_FAILURE;
}

static int tui(int prepared_only)
{
    struct c1_update_state state, current;
    struct c1_update_transaction_config config;
    struct c1_update_transaction_result prepared;
    char repository[C1_UPDATE_URL_MAX + 1U];
    char error[C1_UPDATE_ERROR_MAX] = "";
    int character;

    if (c1_update_state_load(C1_UPDATE_DEFAULT_STATE_ROOT, &state,
                             error, sizeof(error)) != 0) goto failed;
    printf("C1 core update (includes c1pkg): %s\n", c1_update_phase_name(state.phase));
    if (state.phase != C1_UPDATE_PREPARED) {
        if (prepared_only != 0) {
            puts("No prepared core update. Nothing to restart.");
            return EXIT_SUCCESS;
        }
        puts("Checking and preparing core update...");
        fflush(stdout);
        (void)memset(&config, 0, sizeof(config));
        config.staging_root = C1_UPDATE_DEFAULT_STAGING_ROOT;
        config.core_root = C1_UPDATE_DEFAULT_CORE_ROOT;
        config.state_root = C1_UPDATE_DEFAULT_STATE_ROOT;
        config.key_path = C1_UPDATE_DEFAULT_KEY;
        if (c1_update_repository_read_url(C1_UPDATE_REPOSITORY_CONFIG, repository,
                                          sizeof(repository), error, sizeof(error)) != 0 ||
            c1_update_prepare(&config, repository, &prepared,
                              error, sizeof(error)) != 0) goto failed;
        if (prepared.phase == C1_UPDATE_CONFIRMED) {
            printf("Core is up-to-date: %s (sequence %llu).\n", prepared.release,
                   (unsigned long long)prepared.sequence);
            return EXIT_SUCCESS;
        }
        if (c1_update_state_load(C1_UPDATE_DEFAULT_STATE_ROOT, &state,
                                 error, sizeof(error)) != 0) goto failed;
        if (state.phase != C1_UPDATE_PREPARED) {
            (void)snprintf(error, sizeof(error), "update is not prepared");
            goto failed;
        }
    }
    printf("Prepared core %s (sequence %llu).\n", state.release,
           (unsigned long long)state.sequence);
    puts("Press Enter to restart; q then Enter cancels.");
    fflush(stdout);
    character = getchar();
    if (character != '\n' && character != '\r') {
        puts("Update cancelled; prepared release retained.");
        return EXIT_SUCCESS;
    }
    /* Confirm only the identity shown above, never a replacement prepared
     * while this terminal was waiting. The consumer also checks the digest. */
    if (c1_update_state_load(C1_UPDATE_DEFAULT_STATE_ROOT, &current,
                             error, sizeof(error)) != 0) goto failed;
    if (current.phase != C1_UPDATE_PREPARED || strcmp(current.digest, state.digest) != 0) {
        (void)snprintf(error, sizeof(error), "prepared update changed; reopen to confirm");
        goto failed;
    }
    if (c1_update_request_write(C1_UPDATE_REQUEST_DEFAULT_PATH, state.digest,
                                error, sizeof(error)) != 0) goto failed;
    puts("Restart requested.");
    return EXIT_SUCCESS;

failed:
    fprintf(stderr, "update failed: %s\n", error[0] != '\0' ? error : "rejected");
    puts("Press Enter to return.");
    fflush(stdout);
    (void)getchar();
    return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) { usage(stdout); return EXIT_SUCCESS; }
    if (argc == 2 && strcmp(argv[1], "--log-version") == 0) {
        puts("C1RUNLOG-1");
        return 0;
    }
    if (argc >= 6 && strcmp(argv[1], "run-logged") == 0)
        return c1_update_run_logged(argv[2], argv[3], argv[4], &argv[5]);
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return C1_UPDATE_PROTOCOL_VERSION == 1U && C1_UPDATE_ABI_VERSION == 1U ? EXIT_SUCCESS : EXIT_FAILURE;
    if (argc == 2 && strcmp(argv[1], "--recovery-version") == 0) {
        puts("C1RECOVERY-VERIFIER " C1_UPDATER_VERSION);
        return 0;
    }
    if (argc == 8 && strcmp(argv[1], "recovery-supervise") == 0) {
        char error[C1_UPDATE_ERROR_MAX] = "";
        int result = c1_update_recovery_exec(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], error, sizeof(error));
        fprintf(stderr, "recovery: %s\n", error[0] != '\0' ? error : "generation rejected");
        return result == C1_UPDATE_BUSY ? C1_UPDATER_TRANSIENT_EXIT : C1_UPDATER_FATAL_EXIT;
    }
    if (argc == 2 && strcmp(argv[1], "tui") == 0) return tui(0);
    if (argc == 2 && strcmp(argv[1], "tui-prepared") == 0) return tui(1);
    if (argc == 5 && strcmp(argv[1], "verify-signature") == 0)
        return verify_signature(argv[2], argv[3], argv[4]);
    if (argc == 5 && strcmp(argv[1], "verify-manifest") == 0)
        return verify_manifest(argv[2], argv[3], argv[4]);
    if (argc == 4 && strcmp(argv[1], "verify-current") == 0) {
        char error[C1_UPDATE_ERROR_MAX] = "";
        int result = c1_update_validate_current(argv[2], argv[3], NULL, error, sizeof(error));
        if (result != 0) fprintf(stderr, "current release verification failed: %s\n", error);
        return result == 0 ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "check-configured") == 0) return check_configured();
    if (argc == 3 && strcmp(argv[1], "state") == 0) return show_state(argv[2]);
    if (argc == 7 && strcmp(argv[1], "prepare-local") == 0)
        return prepare_release(argv[2], 1, argv[3], argv[4], argv[5], argv[6]);
    if (argc == 6 && strcmp(argv[1], "prepare-configured") == 0) {
        char repository[C1_UPDATE_URL_MAX + 1U];
        char error[C1_UPDATE_ERROR_MAX] = "";
        if (c1_update_repository_read_url(C1_UPDATE_REPOSITORY_CONFIG, repository,
                                          sizeof(repository), error, sizeof(error)) != 0) {
            fprintf(stderr, "prepare failed: %s\n", error[0] != '\0' ? error : "rejected");
            return EXIT_FAILURE;
        }
        return prepare_release(repository, 0, argv[2], argv[3], argv[4], argv[5]);
    }
    if (argc == 7 && strcmp(argv[1], "prepare") == 0)
        return prepare_release(argv[2], 0, argv[3], argv[4], argv[5], argv[6]);
    if (argc == 5 && (strcmp(argv[1], "activate") == 0 ||
                      strcmp(argv[1], "bootstrap-activate") == 0 ||
                      strcmp(argv[1], "recover") == 0 ||
                      strcmp(argv[1], "confirm") == 0 || strcmp(argv[1], "rollback") == 0))
        return boot_command(argv[1], argv[2], argv[3], argv[4]);
    if (argc == 4 && strcmp(argv[1], "mark-ready") == 0) {
        char error[C1_UPDATE_ERROR_MAX] = "";
        if (c1_update_mark_ready(argv[2], argv[3], error, sizeof(error)) == 0) return EXIT_SUCCESS;
        fprintf(stderr, "mark-ready failed: %s\n", error[0] != '\0' ? error : "rejected");
        return EXIT_FAILURE;
    }
    if (argc == 7 && strcmp(argv[1], "supervise") == 0) {
        char error[C1_UPDATE_ERROR_MAX] = "";
        int status = c1_update_supervise(argv[2], argv[3], argv[4], argv[5], argv[6], error, sizeof(error));
        if (status == C1_UPDATER_SLOT_SWITCH_EXIT) fputs("supervise: prepared updater slot switch requested\n", stderr);
        else if (status == C1_UPDATER_TRANSIENT_EXIT) fputs("supervise: update transaction busy; retrying\n", stderr);
        else if (status != 0) fprintf(stderr, "supervise failed: %s\n", error[0] != '\0' ? error : "rejected");
        return status;
    }
    if (argc == 4 && strcmp(argv[1], "activate-updater-slot") == 0) {
        char error[C1_UPDATE_ERROR_MAX] = "";
        if (strlen(argv[3]) == 1U &&
            c1_update_record_active_slot(argv[2], argv[3][0],
                                         error, sizeof(error)) == 0)
            return EXIT_SUCCESS;
        fprintf(stderr, "activate-updater-slot failed: %s\n",
                error[0] != '\0' ? error : "rejected");
        return EXIT_FAILURE;
    }
    if (argc == 7 && strcmp(argv[1], "install-prepared-slot") == 0) {
        char error[C1_UPDATE_ERROR_MAX] = "", installed = '\0';
        int result = -1;
        if (strlen(argv[3]) == 1U)
            result = c1_update_install_prepared_slot(argv[2], argv[3][0], argv[4], argv[5], argv[6],
                                                     &installed, error, sizeof(error));
        if (result == 0) {
            printf("installed-slot=%c\n", installed);
            return EXIT_SUCCESS;
        }
        fprintf(stderr, "install-prepared-slot failed: %s\n",
                error[0] != '\0' ? error : "rejected");
        return result == C1_UPDATE_BUSY ? C1_UPDATER_TRANSIENT_EXIT : EXIT_FAILURE;
    }
    if (argc == 6 && strcmp(argv[1], "install-inactive-slot") == 0) {
        char error[C1_UPDATE_ERROR_MAX] = "", installed = '\0';
        if (strlen(argv[3]) == 1U && c1_update_install_inactive_slot(argv[2], argv[3][0], argv[4], argv[5],
                                                                     &installed, error, sizeof(error)) == 0) {
            printf("installed-slot=%c\n", installed);
            return EXIT_SUCCESS;
        }
        fprintf(stderr, "install-inactive-slot failed: %s\n", error[0] != '\0' ? error : "rejected");
        return EXIT_FAILURE;
    }
    usage(stderr);
    return EXIT_FAILURE;
}