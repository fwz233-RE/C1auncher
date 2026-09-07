/* QUESTIONS 逻辑测试 — host cc 编译运行; -DCHICHU_HOST
 * 链接 canvas/font/font_data/pattern/rng/time/ui_common/input/display */
#include <stdio.h>
#include <string.h>
#include "../../src/games/questions.c"

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* host stub(main.c 提供, 此处补上) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static void send(int key, char ch, bool rep) {
    key_event_t e;
    e.key = (ccg_key)key;
    e.ch = (uint8_t)ch;
    e.is_repeat = rep;
    questions_on_key(&e);
}
#define K_CH(ch) send(K_CHAR, ch, false)

/* 从 READY 选 idx 开始对局 */
static void start_game(int idx) {
    qs_state = QS_ST_READY;
    qs_phase = QS_PH_KEY;
    qs_cur = idx;
    send(K_OK, 0, false);
}

/* 如实回答当前问题 + 确认; 若进入 THINK 则推进一拍 */
static void answer_truth(void) {
    uint8_t bit = (uint8_t)(1u << qs_ask_attr);
    bool yes = (qs_animals[qs_pick].bits & bit) != 0;
    if (yes) K_CH('y'); else K_CH('n');
    send(K_OK, 0, false);
    if ((qs_state == QS_ST_ASK || qs_state == QS_ST_GUESS) &&
        qs_phase == QS_PH_THINK)
        questions_tick(qs_timer + 1);
}

/* 打到 GUESS 阶段(含 THINK 推进) */
static void round_to_guess(void) {
    int guard = 0;
    while (qs_state == QS_ST_ASK && guard < 30) { answer_truth(); guard++; }
    if (qs_state == QS_ST_GUESS && qs_phase == QS_PH_THINK)
        questions_tick(qs_timer + 1);
}

/* 按固定答案表回答(与所选动物无关, 用于制造矛盾) */
static void round_fixed(const uint8_t ans[QS_ATTR_N]) {
    int guard = 0;
    while (qs_state == QS_ST_ASK && guard < 30) {
        if (ans[qs_ask_attr] != 0) K_CH('y'); else K_CH('n');
        send(K_OK, 0, false);
        if ((qs_state == QS_ST_ASK || qs_state == QS_ST_GUESS) &&
            qs_phase == QS_PH_THINK)
            questions_tick(qs_timer + 1);
        guard++;
    }
    if (qs_state == QS_ST_GUESS && qs_phase == QS_PH_THINK)
        questions_tick(qs_timer + 1);
}

static int fb_nonzero(void) {
    unsigned i;
    int n = 0;
    for (i = 0; i < CCG_FRAME_BYTES; i++) if (g_fb[i]) n++;
    return n;
}

/* ---- 数据表完整性: 30 动物, 合法名, 位图合法, 同向量组恰 3 组 ---- */
static void test_table(void) {
    int i, j;
    int dup_groups = 0;
    CHECK(QS_N == 30, "exactly 30 animals");
    for (i = 0; i < QS_N; i++) {
        const char *n = qs_animals[i].name;
        size_t l = strlen(n);
        if (l == 0 || l > 11) { CHECK(0, "name length 1..11"); printf("  bad: %s\n", n); }
        for (j = 0; j < (int)l; j++)
            if (n[j] < 'a' || n[j] > 'z') { CHECK(0, "name lowercase"); break; }
        if (qs_animals[i].bits > 0x1f) { CHECK(0, "bits within 5 bits"); }
        for (j = i + 1; j < QS_N; j++)
            if (qs_animals[i].bits == qs_animals[j].bits)
                dup_groups++;
    }
    CHECK(dup_groups == 3, "exactly 3 duplicate-vector pairs");
    printf("ok: table checked, %d dup pairs\n", dup_groups);
}

/* ---- enter 进入 READY, rng 播种 ---- */
static void test_enter(void) {
    questions_enter();
    CHECK(qs_state == QS_ST_READY, "enter -> READY");
    CHECK(qs_cur == 0 && qs_pick == 0, "cursor reset");
}

/* ---- READY 光标循环移动(行内环绕) ---- */
static void test_nav(void) {
    qs_state = QS_ST_READY; qs_cur = 0;
    send(K_DOWN, 0, false);  CHECK(qs_cur == 3, "DOWN +3");
    send(K_LEFT, 0, false);  CHECK(qs_cur == 5, "LEFT wraps within row (3->5)");
    send(K_UP, 0, false);    CHECK(qs_cur == 2, "UP -3 wraps to last row");
    send(K_RIGHT, 0, false); CHECK(qs_cur == 0, "RIGHT wraps within row (2->0)");
    send(K_RIGHT, 0, true);  CHECK(qs_cur == 1, "repeat RIGHT still moves");
    send(K_OK, 0, true);     CHECK(qs_state == QS_ST_READY, "repeat OK ignored");
}

/* ---- 如实回答: 唯一向量动物必被猜中; 同向量组猜组内成员 ---- */
static void test_truthful_rounds(void) {
    int i, j, ok = 0;
    for (i = 0; i < QS_N; i++) {
        int twin = -1;
        for (j = 0; j < QS_N; j++)
            if (j != i && qs_animals[j].bits == qs_animals[i].bits) { twin = j; break; }
        start_game(i);
        round_to_guess();
        if (qs_state != QS_ST_GUESS || qs_phase != QS_PH_KEY) {
            CHECK(0, "reached GUESS key phase");
            printf("  stuck on %s\n", qs_animals[i].name);
            continue;
        }
        if (twin < 0) {
            if (qs_guess != i) {
                CHECK(0, "unique animal guessed exactly");
                printf("  %s -> %s\n", qs_animals[i].name, qs_animals[qs_guess].name);
                continue;
            }
            ok++;
        } else {
            if (qs_guess != twin && qs_guess != i) {
                CHECK(0, "dup-group guess within group");
                printf("  %s -> %s\n", qs_animals[i].name, qs_animals[qs_guess].name);
            }
        }
    }
    printf("ok: %d unique animals guessed exactly, dup groups in-group\n", ok);
    CHECK(ok == 24, "24 unique-vector animals all guessed");
}

/* ---- 唯一动物: 猜中 -> WIN_AI, 亮出所选 ---- */
static void test_win_ai(void) {
    start_game(4);                 /* snake: 唯一向量 */
    round_to_guess();
    CHECK(qs_state == QS_ST_GUESS && qs_guess == 4, "guess == snake");
    K_CH('y');
    send(K_OK, 0, false);
    CHECK(qs_state == QS_ST_WIN_AI, "YES -> AI WINS");
    CHECK(qs_guess == qs_pick && qs_pick == 4, "reveal == picked animal");
}

/* ---- 同向量组: 猜组内一只; 玩家说 NO -> 玩家赢, 亮出所选 ---- */
static void test_dup_lose(void) {
    start_game(27);                /* cat 与 fox 同向量 */
    round_to_guess();
    CHECK(qs_state == QS_ST_GUESS, "cat round reaches GUESS");
    CHECK(qs_guess == 11 || qs_guess == 27, "guess is the fox/cat twin");
    K_CH('n');
    send(K_OK, 0, false);
    CHECK(qs_state == QS_ST_WIN_PLAYER, "NO -> player wins");
    CHECK(qs_pick == 27, "reveal is the picked cat");
}

/* ---- 故意矛盾答案: 多轮随机序下必然终止, 猜测必须自洽 ---- */
static void test_contradiction(void) {
    /* 答案组合 0b01011(飞+游泳+毛, 不吃肉, 不大) — 表中无此向量 */
    static const uint8_t ans[QS_ATTR_N] = { 1, 1, 0, 1, 0 };
    int run, i;
    for (run = 0; run < 20; run++) {
        rng_seed(&qs_rng, 0xC0FFEE0000000001ULL + (uint64_t)run);
        start_game(0);             /* ant */
        round_fixed(ans);
        CHECK(qs_state == QS_ST_GUESS, "contradiction still guesses");
        /* 候选非空 → 猜测必须在候选内; 候选空 → 猜测必须是最近向量之一 */
        int any_match = 0, guess_in = 0, best_d = 6;
        for (i = 0; i < QS_N; i++) {
            if ((qs_animals[i].bits & qs_asked) == qs_ansvec) {
                any_match = 1;
                if (i == qs_guess) guess_in = 1;
            } else {
                int d = qs_popcnt((uint8_t)(qs_animals[i].bits ^ qs_ansvec));
                if (d < best_d) best_d = d;
            }
        }
        if (any_match) CHECK(guess_in, "guess within candidate set");
        else CHECK(qs_popcnt((uint8_t)(qs_animals[qs_guess].bits ^ qs_ansvec)) == best_d,
                   "empty candidates -> nearest hamming");
        K_CH('n');
        send(K_OK, 0, false);
        CHECK(qs_state == QS_ST_WIN_PLAYER && qs_pick == 0,
              "reject guess -> player wins, reveal ant");
    }
}

/* ---- 候选清空路径(确定性构造): 最近向量 = duck ---- */
static void test_nearest(void) {
    qs_state = QS_ST_ASK;
    qs_phase = QS_PH_KEY;
    qs_ask_attr = 4;               /* big */
    qs_asked = 0x0f;               /* 飞/游/肉/毛 已问 */
    qs_ansvec = 0x0b;              /* 飞+游+毛 — 表中无此向量 */
    qs_remain = 15;
    qs_qnum = 5;
    qs_init_cands();
    qs_filter_cands();
    CHECK(qs_cand_n == 0, "contradictory vector -> empty candidates");
    K_CH('n');                     /* big=NO */
    send(K_OK, 0, false);
    CHECK(qs_state == QS_ST_GUESS, "empty -> GUESS");
    if (qs_phase == QS_PH_THINK) questions_tick(qs_timer + 1);
    CHECK(qs_guess == 3, "nearest by hamming = duck");
}

/* ---- 属性不重复问 + 候选数一致 + REMAIN 递减 ---- */
static void test_invariants(void) {
    int prev_asked = 0;
    start_game(4);
    CHECK(qs_remain == QS_MAX_Q, "REMAIN starts at 20");
    while (qs_state == QS_ST_ASK) {
        int a = qs_ask_attr;
        CHECK((qs_asked & (1u << a)) == 0, "attribute never asked twice");
        answer_truth();
        if (qs_state == QS_ST_ASK) {
            CHECK(qs_asked > prev_asked, "asked mask grows");
            prev_asked = qs_asked;
            /* 候选数 = 与已知答案一致的动物数 */
            int n = 0, i;
            for (i = 0; i < QS_N; i++)
                if ((qs_animals[i].bits & qs_asked) == qs_ansvec) n++;
            CHECK(qs_cand_n == n, "candidate count consistent");
        }
    }
    CHECK(qs_qnum >= 4 && qs_qnum <= 5, "snake solved in 5 questions");
    CHECK(qs_remain == QS_MAX_Q - qs_qnum, "REMAIN == 20 - asked");
    CHECK(qs_state == QS_ST_GUESS, "round ends at GUESS");
}

/* ---- THINK 节拍: 时间未到仍 THINK, 到点后 PH_KEY ---- */
static void test_think_timing(void) {
    start_game(4);
    K_CH('y');
    send(K_OK, 0, false);          /* 第一问 commit -> 继续下一问 */
    CHECK(qs_phase == QS_PH_THINK, "commit enters THINK");
    CHECK(qs_sel_yes, "answer recorded YES");
    questions_tick(qs_timer - 1);
    CHECK(qs_phase == QS_PH_THINK, "not yet, still THINK");
    questions_tick(qs_timer + 1);
    CHECK(qs_phase == QS_PH_KEY, "time up -> KEY phase");
}

/* ---- THINK 期按键忽略 ---- */
static void test_think_ignore(void) {
    start_game(4);
    K_CH('n');
    send(K_OK, 0, false);
    CHECK(qs_phase == QS_PH_THINK, "in THINK");
    int sel = qs_sel_yes;
    K_CH('y');
    send(K_OK, 0, false);
    send(K_BACK, 0, false);
    CHECK(qs_sel_yes == sel, "keys ignored during THINK");
    CHECK(qs_state == QS_ST_ASK, "state unchanged during THINK");
}

/* ---- Y/N 选择与重复事件 ---- */
static void test_yn(void) {
    start_game(4);
    CHECK(qs_sel_yes, "default selection YES");
    send(K_CHAR, 'n', true);
    CHECK(qs_sel_yes, "repeat 'n' ignored");
    K_CH('n');
    CHECK(!qs_sel_yes, "'n' selects NO");
    K_CH('Y');
    CHECK(qs_sel_yes, "uppercase Y normalizes");
    K_CH('x');
    CHECK(qs_sel_yes, "other letters ignored");
}

/* ---- 结束按键: OK/N 新局, BACK 退出 ---- */
static void test_over_keys(void) {
    start_game(27);
    round_to_guess();
    K_CH('n');
    send(K_OK, 0, false);
    CHECK(qs_state == QS_ST_WIN_PLAYER, "in WIN_PLAYER");
    s_exit_request = false;
    send(K_OK, 0, false);
    CHECK(qs_state == QS_ST_READY, "OK -> new round (READY)");
    CHECK(qs_cur == 27, "cursor kept for replay");

    start_game(27);
    round_to_guess();
    K_CH('n');
    send(K_OK, 0, false);
    s_exit_request = false;
    K_CH('n');
    CHECK(qs_state == QS_ST_READY, "'n' also starts new round");
    CHECK(!s_exit_request, "no quit on 'n'");

    start_game(27);
    round_to_guess();
    K_CH('n');
    send(K_OK, 0, false);
    s_exit_request = false;
    send(K_BACK, 0, false);
    CHECK(s_exit_request, "BACK quits after game over");
    s_exit_request = false;
    send(K_QUIT, 0, false);
    CHECK(s_exit_request, "Q quits after game over");
}

/* ---- 渲染冒烟: 各状态画帧非空 ---- */
static void test_render(void) {
    questions_enter();
    questions_render();
    CHECK(fb_nonzero() > 0, "READY frame nonempty");

    start_game(4);
    questions_render();
    CHECK(fb_nonzero() > 0, "ASK frame nonempty");

    K_CH('y');
    send(K_OK, 0, false);
    questions_render();
    CHECK(fb_nonzero() > 0, "THINK frame nonempty");

    round_to_guess();
    questions_render();
    CHECK(fb_nonzero() > 0, "GUESS frame nonempty");

    K_CH('y');
    send(K_OK, 0, false);
    questions_render();
    CHECK(fb_nonzero() > 0, "WIN_AI frame nonempty");

    start_game(27);
    round_to_guess();
    K_CH('n');
    send(K_OK, 0, false);
    questions_render();
    CHECK(fb_nonzero() > 0, "WIN_PLAYER frame nonempty");
}

int main(void) {
    printf("== questions logic tests ==\n");
    test_table();
    test_enter();
    test_nav();
    test_truthful_rounds();
    test_win_ai();
    test_dup_lose();
    test_contradiction();
    test_nearest();
    test_invariants();
    test_think_timing();
    test_think_ignore();
    test_yn();
    test_over_keys();
    test_render();
    if (s_fail) { printf("TOTAL FAIL: %d\n", s_fail); return 1; }
    printf("ALL PASS\n");
    return 0;
}
