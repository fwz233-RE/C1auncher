/* Reuse all WAL, cancellation, hash/manifest, retained-payload and skip fixtures.
 * A failed statistics queue must not change any successful install result. */
#define c1pkg_device_metrics_record_install fixture_record_install
#define main transaction_fixture_main
#include "test_pkg_transaction.c"
#undef main
#undef c1pkg_device_metrics_record_install
static int recorded;
int fixture_record_install(const struct c1pkg_config *config, const struct c1pkg_package *package)
{
    ++recorded;
    expect(config != NULL && package != NULL, "event has online origin and verified package");
    expect(access(TRANSACTION_PATH, F_OK) != 0, "event occurs only after WAL completion");
    expect(strcmp(package->version, "5.0.0") == 0 || strcmp(package->version, "6.0.0") == 0 ||
           strcmp(package->version, "7.0.0") == 0, "only actual successful versions create events");
    errno = EIO; /* Simulate an unavailable/full private statistics queue. */
    return -1;
}
int main(void)
{
    int result = transaction_fixture_main();
    expect(recorded == 4, "four real install successes; no skips, failures, rollback or recovery events");
    if (result != 0 || failures != 0) return 1;
    puts("Installation metrics hook tests passed.");
    return 0;
}
