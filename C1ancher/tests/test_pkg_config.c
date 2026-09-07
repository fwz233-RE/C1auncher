/* Standalone main.c configuration precedence test. No device files or network. */
#define main c1pkg_program_main
#include "../src/pkg/main.c"
#undef main

static const char *persistent;
static int persistent_invalid;
static int reads;
static int failures;
static char selected[1025];
static int install_result = C1PKG_INSTALL_PACKAGE_ERROR;
static struct c1pkg_package install_package = {.id = "app", .version = "1.0.0"};

int c1pkg_repo_read_url(const char *path, char *url, size_t size, char *error, size_t error_size)
{
    (void)path;
    ++reads;
    (void)snprintf(url, size, "%s", persistent != NULL ? persistent : "");
    if (persistent_invalid) {
        (void)snprintf(error, error_size, "invalid test repository.url");
        return -1;
    }
    return 0;
}

int c1pkg_tui(const struct c1pkg_config *config)
{
    (void)snprintf(selected, sizeof(selected), "%s", config->repo_base);
    return 0;
}

const char *c1pkg_font_license(void) { return "test font license\n"; }

int c1pkg_gui(const struct c1pkg_config *config)
{
    return c1pkg_tui(config);
}

int c1pkg_repo_refresh(const struct c1pkg_config *config, struct c1pkg_index *index, char *error, size_t size)
{ (void)config; (void)error; (void)size; memset(index, 0, sizeof(*index)); return 0; }
int c1pkg_store_list(struct c1pkg_installed_list *list, char *error, size_t size)
{ (void)list; (void)error; (void)size; return -1; }
const struct c1pkg_package *c1pkg_repo_find(const struct c1pkg_index *index, const char *id)
{ (void)index; (void)id; return &install_package; }
int c1pkg_install_decision(const char *installed, const char *available)
{ (void)installed; (void)available; return 0; }
int c1pkg_safe_id(const char *id) { (void)id; return 1; }
int c1pkg_store_install(const struct c1pkg_config *config, const struct c1pkg_package *package, char *error, size_t size)
{ (void)config; (void)package; (void)snprintf(error, size, "test outcome"); return install_result; }
int c1pkg_store_remove(const char *id, char *error, size_t size)
{ (void)id; (void)error; (void)size; return -1; }
int c1pkg_store_rollback(const char *id, char *error, size_t size)
{ (void)id; (void)error; (void)size; return -1; }
int c1pkg_store_launch(const char *id, char *const args[], char *error, size_t size)
{ (void)id; (void)args; (void)error; (void)size; return -1; }
bool c1_power_config_auto_suspend_enabled(void) { return true; }
int c1_power_config_set_auto_suspend(bool enabled, char *error, size_t size)
{ (void)enabled; (void)error; (void)size; return -1; }

static void check(const char *environment, const char *stored, const char *cli,
                  int invalid, const char *expected, int expected_reads)
{
    char *args[] = {"c1pkg", "--repo", (char *)cli, "tui", NULL};
    char *defaults[] = {"c1pkg", "tui", NULL};
    if (environment != NULL) (void)setenv("C1PKG_REPO", environment, 1);
    else (void)unsetenv("C1PKG_REPO");
    persistent = stored;
    persistent_invalid = invalid;
    reads = 0;
    if (c1pkg_program_main(cli != NULL ? 4 : 2, cli != NULL ? args : defaults) != 0 ||
        strcmp(selected, expected) != 0 || reads != expected_reads) {
        fprintf(stderr, "FAIL: repository precedence expected %s; got %s (%d reads)\n", expected, selected, reads);
        ++failures;
    }
}

int main(void)
{
    check(NULL, NULL, NULL, 0, "http://www.fwz233.com/c1/v2", 1);
    check("", NULL, NULL, 0, "http://www.fwz233.com/c1/v2", 1);
    check(NULL, "http://stored.example/c1/v2", NULL, 0, "http://stored.example/c1/v2", 1);
    check("", "http://stored.example/c1/v2", NULL, 0, "http://stored.example/c1/v2", 1);
    check("https://env.example/custom", "http://stored.example/c1/v2", NULL, 0, "https://env.example/custom", 0);
    check("http://env.example", "http://stored.example", "https://cli.example/custom", 0, "https://cli.example/custom", 0);
    check(NULL, NULL, NULL, 1, "", 1);
    check("http://env.example", NULL, NULL, 1, "http://env.example", 0);
    check(NULL, NULL, "http://cli.example", 1, "http://cli.example", 0);
    {
        struct c1pkg_config config = {.repo_base = "http://local.test", .public_key = "unused"};
        const int results[] = {C1PKG_INSTALL_OK, C1PKG_INSTALL_SKIPPED,
                               C1PKG_INSTALL_PACKAGE_ERROR, C1PKG_INSTALL_CANCELLED,
                               C1PKG_INSTALL_STORAGE_ERROR};
        size_t i;
        for (i = 0U; i < sizeof(results) / sizeof(results[0]); ++i) {
            install_result = results[i];
            if (command_install(&config, "app") != (results[i] < 0 ? 1 : 0)) {
                fprintf(stderr, "FAIL: CLI install result %d\n", results[i]);
                ++failures;
            }
        }
    }
    if (failures != 0) return 1;
    puts("All package configuration precedence tests passed.");
    return 0;
}
