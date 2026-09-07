/* LIFE VARIANTS 逻辑单测 — host cc 编译运行
 * 直接 #include 游戏源文件(静态状态可访问), 链接框架层源文件(-DCHICHU_HOST)
 * 覆盖: 3 规则演化(Standard/HighLife/Seeds)、边缘视为死、tick 暂停语义、
 *       按键语义(SPACE 编辑/OK 运行/规则切换/清空/新局/重复防护)、
 *       活细胞计数一致性、rng 播种安全、渲染像素、长跑不死循环 */
#define rng_seed l2_stub_seed
#define rng_next  l2_stub_next
#define rng_range l2_stub_range
#include "../../src/games/life2.c"

#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- host 框架 stub ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

/* ---- rng stub: 忠实 xorshift32 拷贝(替代链接 rng.c), 另加 zombie 模式
 *      使 rng_range 恒返回 n-1(全格死), 用于确定性触发 do-while guard+兜底 */
static bool t_zombie = false;
void l2_stub_seed(rng_t *r, uint64_t entropy) {
    uint32_t s = (uint32_t)(entropy ^ (entropy >> 32));
    if (s == 0) s = 0x9e3779b9u;
    r->s = s;
}
uint32_t l2_stub_next(rng_t *r) {
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}
uint32_t l2_stub_range(rng_t *r, uint32_t n) {
    if (n <= 1) return 0;
    if (t_zombie) return n - 1u;          /* 模拟坏 RNG: 永不命中 30% 阈值 */
    uint32_t limit = (uint32_t)(-1) / n * n;
    uint32_t v;
    do { v = l2_stub_next(r); } while (v >= limit);
    return v % n;
}

static void t_clear(void) {
    for (int i = 0; i < L2_CELLS; i++) l2_cells[i] = 0;
    l2_alive = 0;
}
static void t_set(int x, int y) { l2_cells[y * L2_COLS + x] = 1; l2_alive++; }
static int  t_get(int x, int y) { return l2_cells[y * L2_COLS + x]; }
static void t_rule(int r) { l2_rule = r; }
static int  t_count(void) { return (int)l2_count_alive(); }

/* ---- rng 安全: 种子 0 不得死循环; range 恒 < n ---- */
static void test_rng_safety(void) {
    rng_t r;
    l2_stub_seed(&r, 0);
    int bad = 0;
    for (int i = 0; i < 2000; i++) {
        uint32_t v = rng_range(&r, 100u);
        if (v >= 100u) bad++;
        if (r.s == 0) bad++;
    }
    CHECK(bad == 0, "rng seed 0: 2000 draws in range, state nonzero");
}

/* ---- STANDARD (B3/S23) ---- */
static void test_standard(void) {
    t_rule(0);
    /* 闪烁器: 横->竖->横 */
    t_clear();
    t_set(5, 5); t_set(6, 5); t_set(7, 5);
    l2_gen = 0;
    l2_step();
    CHECK(t_get(6, 4) && t_get(6, 5) && t_get(6, 6), "std blinker -> vertical");
    CHECK(!t_get(5, 5) && !t_get(7, 5), "std blinker ends died");
    CHECK(l2_alive == 3 && l2_gen == 1, "std blinker gen=1 alive=3");
    l2_step();
    CHECK(t_get(5, 5) && t_get(6, 5) && t_get(7, 5), "std blinker -> horizontal");
    CHECK(l2_alive == 3 && l2_gen == 2, "std blinker gen=2 alive=3");
    /* 方块静物 */
    t_clear();
    t_set(10, 3); t_set(11, 3); t_set(10, 4); t_set(11, 4);
    l2_gen = 0;
    l2_step();
    CHECK(t_get(10, 3) && t_get(11, 3) && t_get(10, 4) && t_get(11, 4),
          "std 2x2 block still life");
    CHECK(l2_alive == 4 && l2_gen == 1, "std block alive=4 gen=1");
    /* 孤格死亡 */
    t_clear();
    t_set(5, 5);
    l2_gen = 0;
    l2_step();
    CHECK(l2_alive == 0 && l2_gen == 1, "std lone cell dies");
    /* 过密: 十字中心 4 邻居死亡 */
    t_clear();
    t_set(5, 5); t_set(7, 5); t_set(6, 4); t_set(6, 6);   /* 十字 */
    l2_step();
    CHECK(!t_get(6, 5), "std cross center dies (4 neighbors)");
    CHECK(t_get(5, 5) && t_get(7, 5) && t_get(6, 4) && t_get(6, 6),
          "std cross arms survive (3 neighbors)");
    /* 边缘视为死: 角上 3 格 -> 满 2x2, 无环绕 */
    t_clear();
    t_set(0, 0); t_set(1, 0); t_set(0, 1);
    l2_step();
    CHECK(t_get(1, 1), "std corner gap born (3 neighbors, no wrap)");
    CHECK(l2_alive == 4, "std corner block -> 2x2, no wrap cells");
    CHECK(t_get(0, 0) && t_get(1, 0) && t_get(0, 1), "std corner cells survive");
}

/* ---- HIGH LIFE (B36/S23): B6 出生, S 仅 2,3 ---- */
static void test_highlife(void) {
    /* 死格 6 邻居: HighLife 出生, Standard 不出生 */
    t_clear();
    t_set(6, 5); t_set(4, 5); t_set(5, 6); t_set(5, 4);
    t_set(6, 4); t_set(4, 6);                       /* 中心 (5,5) 恰好 6 邻居 */
    t_rule(0);
    l2_step();
    CHECK(!t_get(5, 5), "std: dead cell with 6 neighbors stays dead");
    t_clear();
    t_set(6, 5); t_set(4, 5); t_set(5, 6); t_set(5, 4);
    t_set(6, 4); t_set(4, 6);
    t_rule(1);
    l2_step();
    CHECK(t_get(5, 5), "highlife: dead cell with 6 neighbors born (B6)");
    /* 活格 6 邻居: 依然死亡(S 仅 2,3) */
    t_clear();
    t_set(5, 5); t_set(6, 5); t_set(4, 5); t_set(5, 6); t_set(5, 4);
    t_set(6, 4); t_set(4, 6);                       /* 中心活 + 6 邻居 */
    l2_step();
    CHECK(!t_get(5, 5), "highlife: live cell with 6 neighbors dies (no S6)");
    /* 环 6 邻居围绕死中心: Standard 不生(B3), HighLife 生(B6);
     * 环本身两规则下 6 格全部 2-3 邻居存活 */
    t_clear();
    t_set(4, 5); t_set(6, 5); t_set(5, 4); t_set(5, 6); t_set(4, 4); t_set(6, 6);
    l2_gen = 0;
    t_rule(0);
    l2_step();
    CHECK(l2_alive == 6 && !t_get(5, 5), "std: ring of 6 survives, center stays dead");
    t_clear();
    t_set(4, 5); t_set(6, 5); t_set(5, 4); t_set(5, 6); t_set(4, 4); t_set(6, 6);
    l2_gen = 0;
    t_rule(1);
    l2_step();
    CHECK(l2_alive == 7 && t_get(5, 5), "highlife: ring of 6 + center born (B6)");
    /* 2x2 方块在 HighLife 是静物(与 Standard 一致): 16 代不变 */
    t_clear();
    t_set(10, 3); t_set(11, 3); t_set(10, 4); t_set(11, 4);
    l2_gen = 0;
    for (int i = 0; i < 16; i++) l2_step();
    CHECK(l2_alive == 4 && l2_gen == 16, "highlife: 2x2 block still life 16 gens");
    t_rule(0);
}

/* ---- SEEDS (B2/S): 只生不存, 2 邻居出生 ---- */
static void test_seeds(void) {
    t_rule(2);
    /* 孤格死亡 */
    t_clear();
    t_set(5, 5);
    l2_step();
    CHECK(l2_alive == 0, "seeds: lone cell dies");
    /* 横排 3 格 -> 四对角格 */
    t_clear();
    t_set(4, 5); t_set(5, 5); t_set(6, 5);
    l2_step();
    CHECK(t_get(4, 4) && t_get(6, 4) && t_get(4, 6) && t_get(6, 6),
          "seeds: blinker -> 4 diagonal cells");
    CHECK(!t_get(5, 5) && !t_get(5, 4) && !t_get(5, 6) && !t_get(4, 5) && !t_get(6, 5),
          "seeds: all parent cells die (no survive)");
    CHECK(l2_alive == 4, "seeds: alive=4");
    /* 对角对 -> 另一对角(振荡器) */
    t_clear();
    t_set(4, 4); t_set(5, 5);
    l2_step();
    CHECK(t_get(4, 5) && t_get(5, 4), "seeds: diag pair -> other diag");
    CHECK(!t_get(4, 4) && !t_get(5, 5), "seeds: diag pair parents die");
    CHECK(l2_alive == 2, "seeds: diag alive=2");
    /* 死格 3 邻居不出生 */
    t_clear();
    t_set(4, 5); t_set(5, 5); t_set(6, 5);
    l2_step();
    CHECK(!t_get(5, 4) && !t_get(5, 6), "seeds: dead with 3 neighbors not born");
    t_rule(0);
}

/* ---- tick: 暂停不动, 运行演化 ---- */
static void test_tick_pause(void) {
    t_rule(0);
    t_clear();
    t_set(5, 5);
    l2_running = true;
    l2_gen = 0;
    life2_tick(1234);
    CHECK(l2_gen == 1 && l2_alive == 0, "tick running: one gen, lone dies");
    t_clear();
    t_set(5, 5);
    l2_running = false;
    life2_tick(1);
    life2_tick(2);
    CHECK(l2_gen == 1 && l2_alive == 1, "tick paused: no change");
    l2_running = true;
}

/* ---- 活细胞计数一致性 ---- */
static void test_counter(void) {
    t_rule(0);
    l2_random_fill();
    CHECK(l2_alive == (uint32_t)t_count(), "random fill: counter == manual count");
    CHECK(l2_alive > 0, "random fill: guaranteed non-empty (do-while guard)");
    l2_step();
    CHECK(l2_alive == (uint32_t)t_count(), "step: counter == manual count");
    /* 随机填充多轮不空(guard 生效, 兜底 1 格) */
    for (int i = 0; i < 50; i++) {
        l2_random_fill();
        if (l2_alive == 0) { CHECK(0, "random fill 50 rounds: never empty"); return; }
    }
    CHECK(1, "random fill 50 rounds: never empty");
    /* zombie RNG: 8 次全死 -> do-while guard 退出 -> 兜底强制 1 活格 */
    t_zombie = true;
    for (int i = 0; i < L2_CELLS; i++) l2_cells[i] = 0;
    l2_alive = 0;
    l2_random_fill();
    t_zombie = false;
    CHECK(l2_alive == 1 && t_count() == 1, "zombie rng: guard exits, fallback 1 live cell");
}

/* ---- 按键语义 ---- */
static void test_keys(void) {
    key_event_t ev;
    memset(&ev, 0, sizeof(ev));
    t_rule(0);

    /* SPACE 编辑 + 重复防护 */
    t_clear();
    l2_cx = 3; l2_cy = 3;
    l2_running = false;
    ev.key = K_SPACE; ev.is_repeat = false;
    life2_on_key(&ev);
    CHECK(t_get(3, 3) == 1 && l2_alive == 1, "SPACE toggles cell on, counter +1");
    CHECK(!l2_running, "SPACE does not affect run state");
    ev.is_repeat = true;
    life2_on_key(&ev);
    CHECK(t_get(3, 3) == 1, "SPACE repeat ignored");
    ev.is_repeat = false;
    life2_on_key(&ev);
    CHECK(t_get(3, 3) == 0 && l2_alive == 0, "SPACE toggles cell off, counter -1");

    /* OK 运行/暂停 */
    l2_running = false;
    ev.key = K_OK;
    life2_on_key(&ev);
    CHECK(l2_running, "OK starts run");
    life2_on_key(&ev);
    CHECK(!l2_running, "OK stops run");
    ev.is_repeat = true;
    life2_on_key(&ev);
    CHECK(!l2_running, "OK repeat ignored");
    ev.is_repeat = false;
    /* K_PAUSE 与 'p' 同样切换 */
    ev.key = K_PAUSE;
    life2_on_key(&ev);
    CHECK(l2_running, "K_PAUSE resumes");
    ev.key = K_CHAR; ev.ch = 'p';
    life2_on_key(&ev);
    CHECK(!l2_running, "'p' pauses");

    /* 方向重复可响应 + 边界 */
    l2_cx = 10; l2_cy = 5;
    ev.key = K_LEFT; ev.is_repeat = true;
    life2_on_key(&ev);
    CHECK(l2_cx == 9, "K_LEFT repeat moves cursor");
    ev.is_repeat = false;
    l2_cx = 0; l2_cy = 0;
    ev.key = K_LEFT;  life2_on_key(&ev);
    ev.key = K_UP;    life2_on_key(&ev);
    CHECK(l2_cx == 0 && l2_cy == 0, "cursor clamped top-left");
    l2_cx = L2_COLS - 1; l2_cy = L2_ROWS - 1;
    ev.key = K_RIGHT; life2_on_key(&ev);
    ev.key = K_DOWN;  life2_on_key(&ev);
    CHECK(l2_cx == L2_COLS - 1 && l2_cy == L2_ROWS - 1, "cursor clamped bottom-right");
    /* WASD */
    l2_cx = 5; l2_cy = 5;
    ev.key = K_CHAR; ev.ch = 'w'; life2_on_key(&ev);
    CHECK(l2_cy == 4, "'w' moves up");
    ev.ch = 's'; life2_on_key(&ev);
    CHECK(l2_cy == 5, "'s' moves down");
    ev.ch = 'a'; life2_on_key(&ev);
    CHECK(l2_cx == 4, "'a' moves left");
    ev.ch = 'd'; life2_on_key(&ev);
    CHECK(l2_cx == 5, "'d' moves right");
    ev.is_repeat = true; ev.ch = 'd'; life2_on_key(&ev);
    CHECK(l2_cx == 5, "'d' repeat ignored (letter)");
    ev.is_repeat = false;

    /* R 循环切换规则 0->1->2->0, gen 清零 */
    t_rule(0);
    l2_gen = 42;
    ev.ch = 'r'; life2_on_key(&ev);
    CHECK(l2_rule == 1 && l2_gen == 0, "'r' cycles to HIGH LIFE, gen reset");
    ev.ch = 'r'; life2_on_key(&ev);
    CHECK(l2_rule == 2, "'r' cycles to SEEDS");
    ev.ch = 'r'; life2_on_key(&ev);
    CHECK(l2_rule == 0, "'r' wraps to STANDARD");
    ev.is_repeat = true; ev.ch = 'r'; life2_on_key(&ev);
    CHECK(l2_rule == 0, "'r' repeat ignored (letter)");
    ev.is_repeat = false;
    l2_rule = 0;

    /* C 清空 */
    t_set(4, 4); t_set(5, 5);
    l2_running = true;
    l2_gen = 9;
    ev.ch = 'c'; life2_on_key(&ev);
    CHECK(l2_alive == 0 && l2_gen == 0 && !l2_running, "'c' clears, gen=0, paused");

    /* N 新局 */
    ev.ch = 'n'; life2_on_key(&ev);
    CHECK(l2_gen == 0 && l2_running, "'n' new game: gen=0 running");
    CHECK(l2_alive > 0, "'n' pattern non-empty");
    CHECK(l2_cx == L2_COLS / 2 && l2_cy == L2_ROWS / 2, "'n' cursor centered");

    /* Q / K_QUIT 退出 */
    s_exit_request = false;
    ev.ch = 'q'; life2_on_key(&ev);
    CHECK(s_exit_request, "'q' sets exit request");
    s_exit_request = false;
    ev.key = K_QUIT; life2_on_key(&ev);
    CHECK(s_exit_request, "K_QUIT sets exit request");
    s_exit_request = false;
    ev.is_repeat = true;
    ev.key = K_QUIT; life2_on_key(&ev);
    CHECK(!s_exit_request, "K_QUIT repeat ignored");
    s_exit_request = false;
}

/* ---- 渲染: 活格像素 / 光标反色边框 / 各规则不崩溃 ---- */
static void test_render(void) {
    t_rule(0);
    t_clear();
    t_set(0, 0);                       /* 实心格 */
    l2_cx = 20; l2_cy = 7;             /* 光标放远处(死格 -> 黑边框) */
    l2_running = true;
    l2_gen = 7;
    life2_render();
    /* 格 (0,0) 中心 (OX+6, OY+6) = (10,22): 字节 602, mask 0x02 */
    CHECK((g_fb[602] & 0x02) != 0, "live cell pixel drawn");
    /* 光标边框角像素 (244-2, 100-2) = (242,98): 字节 3794, mask 0x20 */
    CHECK((g_fb[3794] & 0x20) != 0, "cursor black border on dead cell");
    /* HUD 标题 'L' 首像素 (2,2): 字节 2, mask 0x20 */
    CHECK((g_fb[2] & 0x20) != 0, "HUD title pixel drawn");
    /* 活格在光标下 -> 白边框 */
    t_clear();
    t_set(20, 7);
    life2_render();
    CHECK((g_fb[3794] & 0x20) == 0, "cursor white border on live cell");
    /* 三规则渲染不崩溃 */
    for (int r = 0; r < L2_RULES; r++) {
        t_rule(r);
        l2_running = (r & 1) == 0;
        life2_render();
    }
    CHECK(1, "render ok for all 3 rules, running and paused");
    l2_rule = 0;
    l2_running = true;
}

/* ---- 长跑: 5000 代无死循环, 代数与计数一致 ---- */
static void test_soak(void) {
    t_rule(1);                          /* HighLife 最活跃 */
    l2_new_game();
    l2_running = false;
    uint32_t start = l2_gen;
    for (int i = 0; i < 5000; i++) {
        l2_step();
        if (l2_alive != l2_count_alive()) {
            CHECK(0, "soak: alive counter consistent at every gen");
            return;
        }
    }
    CHECK(l2_gen == start + 5000, "soak 5000 gens: counter exact");
    CHECK(l2_alive == (uint32_t)t_count(), "soak: final alive consistent");
    l2_rule = 0;
}

int main(void) {
    test_rng_safety();
    test_standard();
    test_highlife();
    test_seeds();
    test_tick_pause();
    test_counter();
    test_keys();
    test_render();
    test_soak();
    if (s_fail == 0) printf("ALL LIFE2 TESTS PASSED\n");
    else printf("%d FAILURES\n", s_fail);
    return s_fail ? 1 : 0;
}
