/* Runtime white-box tests: exercise real poll/stdin/PBM and deterministic
 * evdev events without a display, SDL, device deployment, or root access. */
#define RT_TEST 1
#define EPAPER "./sys/"
#define STATE_DIR "."
#define BOOT_ID_PATH "./boot_id"
#include "../src/runtime.c"
#include <assert.h>
#include <sys/wait.h>

static void fixture(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static void test_budget(const char *dir) {
    char cwd[PATH_MAX];
    unsigned count;
    pid_t child;
    int status;
    assert(getcwd(cwd, sizeof cwd));
    assert(chdir(dir) == 0);
    assert(mkdir("sys", 0700) == 0);
    fixture("boot_id", "11111111-1111-1111-1111-111111111111\n");
    fixture("sys/refresh_cnt", "2\n");
    fixture("sys/fast_refresh_only", "0");
    assert(write_node(EPAPER "fast_refresh_only", "1") == 0);
    assert(read_count(&count) && count == 2);
    last_full = 0;
    init_budget();
    assert(budget_safe && budget_used == 2);
    reserve_fast();
    assert(budget_safe && budget_used == 3);
    assert(reserve_full(rt_now()));
    assert(budget_used == 11);
    /* A fresh process must inherit persisted reservations, not reset them. */
    child = fork(); assert(child >= 0);
    if (child == 0) {
        budget_used = 0; last_full = 0; init_budget();
        assert(budget_safe && budget_used == 11 && last_full != 0);
        assert(reserve_full(rt_now()) && budget_used == 19);
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    last_full = 0; init_budget();
    assert(budget_safe && budget_used == 19);
    assert(reserve_full(rt_now()) && budget_used == 27);
    assert(!reserve_full(rt_now()) && budget_used == 27);
    for (unsigned i = 0; i < 10; ++i) reserve_fast();
    assert(budget_used == 30 && !reserve_full(rt_now()));
    /* A real new boot ID is the only normal budget restart. */
    fixture("boot_id", "22222222-2222-2222-2222-222222222222\n");
    fixture("sys/refresh_cnt", "4\n");
    last_full = 0; init_budget();
    assert(budget_safe && budget_used == 4);
    fixture("sys/refresh_cnt", "12\n");
    assert(synchronize_budget() && budget_used == 12);
    fixture("sys/refresh_cnt", "3\n");
    assert(synchronize_budget() && budget_used == 30); /* counter reset != credit */
    fixture("sys/refresh_cnt", "garbage\n");
    assert(!read_count(&count));
    assert(!reserve_full(rt_now()) && !budget_safe);
    fixture("sys/refresh_cnt", "31\n"); assert(!read_count(&count));
    fixture("sys/refresh_cnt", "-1\n"); assert(!read_count(&count));
    fixture("sys/refresh_cnt", "7oops\n"); assert(!read_count(&count));
    fixture("sys/refresh_cnt", "7\n");
    fixture("refresh-budget", "truncated ledger\n");
    init_budget(); assert(!budget_safe);
    assert(unlink("refresh-budget") == 0);
    /* Persistence failure disables full refresh; it cannot create credit. */
    assert(mkdir("refresh-budget.tmp", 0700) == 0);
    init_budget(); assert(!budget_safe && !reserve_full(rt_now()));
    assert(rmdir("refresh-budget.tmp") == 0);
    unlink("boot_id"); unlink("sys/refresh_cnt"); unlink("sys/fast_refresh_only");
    rmdir("sys");
    assert(chdir(cwd) == 0);
    last_full = 0;
}

static void test_mapping_and_repeat(void) {
    struct input_event ev;
    unsigned allowed[] = {103, 108, 105, 106, 352, 28, 158};
    rt_event expected[] = {RT_UP, RT_DOWN, RT_LEFT, RT_RIGHT, RT_OK, RT_OK, RT_BACK};
    memset(held, 0, sizeof held);
    for (unsigned i = 0; i < sizeof allowed / sizeof allowed[0]; ++i)
        assert(map_device(allowed[i]) == expected[i]);
    for (unsigned c = 0; c < 768; ++c) {
        bool permitted = false;
        for (unsigned i = 0; i < sizeof allowed / sizeof allowed[0]; ++i)
            if (c == allowed[i]) permitted = true;
        assert((map_device(c) != RT_NONE) == permitted);
    }
    memset(&ev, 0, sizeof ev);
    ev.type = EV_KEY; ev.code = KEY_UP; ev.value = 1;
    assert(device_event(0, &ev, 1000) == RT_UP);
    assert(device_event(0, &ev, 1001) == RT_NONE);
    ev.value = 2;
    assert(device_event(0, &ev, 1500) == RT_NONE);
    assert(emit_repeat(1699) == RT_NONE);
    assert(repeat_deadline(UINT64_MAX) == 1700);
    assert(emit_repeat(1700) == RT_UP);
    assert(emit_repeat(2399) == RT_NONE);
    assert(emit_repeat(9000) == RT_UP);
    assert(emit_repeat(9000) == RT_NONE); /* no catch-up */
    ev.value = 0;
    assert(device_event(0, &ev, 9050) == RT_NONE);
    assert(emit_repeat(10000) == RT_NONE);
    ev.code = 352; ev.value = 1;
    assert(device_event(1, &ev, 10000) == RT_OK);
    assert(emit_repeat(20000) == RT_NONE);
    ev.code = 158;
    assert(device_event(1, &ev, 20000) == RT_BACK);
    assert(emit_repeat(30000) == RT_NONE);
    ev.code = KEY_LEFT;
    assert(device_event(1, &ev, 30000) == RT_LEFT);
    ev.type = EV_SYN; ev.code = SYN_DROPPED;
    assert(device_event(1, &ev, 30001) == RT_NONE);
    assert(emit_repeat(40000) == RT_NONE);
    ev.type = EV_KEY; ev.code = KEY_RIGHT; ev.value = 1;
    assert(device_event(1, &ev, 40001) == RT_NONE);
    ev.type = EV_SYN; ev.code = SYN_REPORT;
    assert(device_event(1, &ev, 40002) == RT_NONE);
    memset(held, 0, sizeof held);
}

static void test_preview(const char *path) {
    FILE *f;
    unsigned char bytes[CCG_FRAME_BYTES + 32];
    size_t n, header;
    memset(g_fb, 0, sizeof g_fb);
    g_fb[0] = 0x80;
    assert(rt_present() == 1);
    assert(has_last && last_frame[0] == 0x80);
    assert(rt_present() == 0);
    g_fb[0] = 0x40;
    assert(rt_present() == 0); /* 700ms throttle */
    assert(last_frame[0] == 0x80);
    last_write = rt_now() - RT_TICK_MS;
    preview_path = "/nonexistent-inkwars-directory/preview.pbm";
    assert(rt_present() == -1);
    assert(last_frame[0] == 0x80); /* failure must not poison dedupe cache */
    preview_path = path;
    assert(rt_present() == 1);
    assert(last_frame[0] == 0x40);
    f = fopen(path, "rb"); assert(f);
    n = fread(bytes, 1, sizeof bytes, f); fclose(f);
    header = strlen("P4\n296 152\n");
    assert(n == header + CCG_FRAME_BYTES);
    assert(memcmp(bytes, "P4\n296 152\n", header) == 0);
    assert(bytes[header] == 0 && bytes[header + CCG_W / 8] == 0x80);
    for (unsigned i = 0; i < CCG_FRAME_BYTES; ++i)
        assert(bytes[header + i] == (i == CCG_W / 8 ? 0x80 : 0));
    assert(!pending_full);
    rt_idle(rt_now() + 10000);
    assert(last_full == 0); /* no automatic refresh */
    rt_request_full();
    rt_idle(last_write + RT_TICK_MS - 1);
    assert(pending_full);
    rt_idle(last_write + RT_TICK_MS);
    assert(!pending_full);
    rt_request_full();
    rt_idle(last_full + RT_FULL_GAP_MS - 1);
    assert(pending_full);
    rt_idle(last_full + RT_FULL_GAP_MS);
    assert(!pending_full);
}

int main(void) {
    int p[2], saved_stdin, status;
    uint64_t start;
    clock_t cpu;
    pid_t child;
    char dir[] = "/tmp/inkwars-runtime-XXXXXX", path[256];
    rt_event sequence[] = {RT_UP, RT_LEFT, RT_DOWN, RT_RIGHT, RT_OK, RT_BACK};
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/preview.pbm", dir);
    assert(pipe(p) == 0);
    saved_stdin = dup(STDIN_FILENO); assert(saved_stdin >= 0);
    assert(dup2(p[0], STDIN_FILENO) == STDIN_FILENO); close(p[0]);
    assert(rt_init(path) == 0);
    test_budget(dir);
    test_mapping_and_repeat();
    test_preview(path);
    assert(write(p[1], "wasd\nb", 6) == 6);
    for (unsigned i = 0; i < 6; ++i) assert(rt_key(0) == sequence[i]);
    assert(write(p[1], "qpx", 3) == 3);
    assert(rt_key(0) == RT_NONE);
    cpu = clock(); start = rt_now();
    assert(rt_wait(start + RT_TICK_MS) == RT_NONE);
    assert(rt_now() - start >= RT_TICK_MS);
    assert((double)(clock() - cpu) / CLOCKS_PER_SEC < 0.15);
    /* A signal wakes an indefinite poll, not just the next scheduled tick. */
    child = fork(); assert(child >= 0);
    if (child == 0) { usleep(50000); kill(getppid(), SIGTERM); _exit(0); }
    start = rt_now();
    assert(rt_key(-1) == RT_QUIT);
    assert(rt_now() - start < 1000);
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    rt_shutdown();
    assert(rt_init(NULL) == 0);
    assert(rt_key(0) == RT_NONE);
    assert(rt_present() == 1);
    raise(SIGCONT);
    assert(rt_key(-1) == RT_REDRAW); /* resume wakes and explicitly invalidates UI */
    assert(rt_present() == 0); /* resume invalidates cache but respects throttle */
    assert(!has_last);
    last_write = rt_now() - RT_TICK_MS;
    assert(rt_present() == 1);
    close(p[1]);
    assert(rt_key(-1) == RT_QUIT);
    rt_shutdown();
    assert(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO); close(saved_stdin);
    unlink(path); rmdir(dir);
    puts("runtime: boot budget/persistence, mapping, repeats, PBM, dedupe/failure, throttle, poll, signals, EOF PASS");
    return 0;
}
