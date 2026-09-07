/* ELEMENT QUIZ 逻辑单测 — host 编译运行; 直接包含 elements.c 访问静态状态
 * 链接 canvas/font/font_data/pattern/rng/time/ui_common/input/display (-DCHICHU_HOST) */
#include "../../src/config.h"
#include "../../src/gfx/canvas.h"
#include "../../src/gfx/font.h"
#include "../../src/gfx/pattern.h"
#include "../../src/rng.h"
#include "../../src/platform/time.h"
#include "../../src/games/elements.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 框架全局 stub(main.c 定义) */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

/* ---- 题库完整性 ---- */
static void test_bank(void) {
    int i, ok = 1;
    for (i = 0; i < EL_N; i++) {
        const el_elem_t *e = &el_elems[i];
        int l;
        if (e->num != (uint8_t)(i + 1)) ok = 0;               /* 原子序数 = 下标+1 */
        if (e->sym[0] == 0 || (e->sym[1] != 0 && e->sym[2] != 0)) ok = 0; /* 1-2 字符 */
        if (e->name[0] == 0) ok = 0;
        l = 0;
        while (e->name[l]) l++;
        if (l > EL_MAX_LEN - 1) ok = 0;                        /* 名字在缓冲上限内 */
    }
    CHECK(ok, "bank: 30 entries, numbers 1-30, symbols 1-2 chars, names fit");
    ok = 1;
    for (i = 0; i < EL_N; i++)
        for (int j = i + 1; j < EL_N; j++) {
            if (strcmp(el_elems[i].name, el_elems[j].name) == 0) ok = 0;
            if (strcmp(el_elems[i].sym, el_elems[j].sym) == 0) ok = 0;
        }
    CHECK(ok, "bank: all names and symbols unique");
    CHECK(strcmp(el_elems[0].sym, "H") == 0 &&
          strcmp(el_elems[0].name, "HYDROGEN") == 0, "spot: 1 H HYDROGEN");
    CHECK(strcmp(el_elems[25].sym, "Fe") == 0 &&
          strcmp(el_elems[25].name, "IRON") == 0, "spot: 26 Fe IRON");
    CHECK(strcmp(el_elems[29].sym, "Zn") == 0 &&
          strcmp(el_elems[29].name, "ZINC") == 0, "spot: 30 Zn ZINC");
}

/* ---- 选项生成: 4 个互异且题目恰好一次 ---- */
static void test_opts(void) {
    int iter, ok = 1;
    rng_seed(&el_rng, 0x1234u);
    for (iter = 0; iter < 200; iter++) {
        int has = 0;
        el_subject = (int)rng_range(&el_rng, EL_N);
        el_build_opts();
        if (el_opts[0] == el_opts[1] || el_opts[0] == el_opts[2] ||
            el_opts[0] == el_opts[3] || el_opts[1] == el_opts[2] ||
            el_opts[1] == el_opts[3] || el_opts[2] == el_opts[3]) ok = 0;
        for (int k = 0; k < 4; k++)
            if (el_opts[k] == el_subject) has++;
        if (has != 1) ok = 0;
        for (int k = 0; k < 4; k++)
            if (el_opts[k] < 0 || el_opts[k] >= EL_N) ok = 0;
    }
    CHECK(ok, "options: 200 draws -> 4 distinct in range, subject exactly once");
}

/* ---- 4 选 1 作答 ---- */
static void test_mc_submit(void) {
    rng_seed(&el_rng, 0x5678u);
    el_state = EL_ST_PLAY;
    el_mode = 0;
    el_subject = 0;
    el_last = 1;
    el_opts[0] = 0; el_opts[1] = 1; el_opts[2] = 2; el_opts[3] = 3;
    el_score = 0; el_correct = 0; el_wrong = 0;
    el_submit_idx(0);
    CHECK(el_score == 10u && el_correct == 1u, "mc: correct +10");
    CHECK(el_state == EL_ST_PLAY, "mc: correct -> next question");
    CHECK(el_subject != 0, "mc: subject advances");
    el_last = el_subject;
    el_subject = 5;
    el_opts[0] = 5; el_opts[1] = 6; el_opts[2] = 7; el_opts[3] = 8;
    el_submit_idx(1);
    CHECK(el_state == EL_ST_FBK && el_wrong == 1u, "mc: wrong -> FBK");
    CHECK(el_fbk_ticks == EL_FBK_TICKS, "mc: FBK duration set");
    el_submit_idx(2);
    CHECK(el_wrong == 1u && el_score == 10u, "mc: submit ignored during FBK");
    el_next_q();
    CHECK(el_state == EL_ST_PLAY && el_cur == 0, "mc: next after FBK resets cursor");
}

/* ---- 输入模式 ---- */
static void test_typein(void) {
    static const char word[] = "HYDROGEN";
    rng_seed(&el_rng, 0x9ABCu);
    el_state = EL_ST_PLAY;
    el_mode = 1;
    el_subject = 0;              /* HYDROGEN */
    el_last = -1;
    el_typed_n = 0;
    el_score = 0; el_correct = 0; el_wrong = 0;
    el_submit_text();
    CHECK(el_state == EL_ST_PLAY && el_typed_n == 0, "type-in: empty submit ignored");
    for (int i = 0; word[i]; i++) el_type((uint8_t)word[i]);
    CHECK(el_typed_n == 8 && el_typed[0] == 'H' &&
          el_typed[7] == 'N', "type-in: letters appended uppercase");
    el_submit_text();
    CHECK(el_score == 10u && el_correct == 1u && el_state == EL_ST_PLAY,
          "type-in: correct spelling +10 -> next");
    el_subject = 0;
    el_typed_n = 0;
    el_type('h'); el_type('y'); el_type('d'); el_type('r');
    el_type('o'); el_type('g'); el_type('e');
    el_submit_text();
    CHECK(el_state == EL_ST_FBK && el_wrong == 1u, "type-in: short spelling wrong -> FBK");
    el_typed_n = 0;
    el_state = EL_ST_PLAY;
    el_type('z'); el_type('i'); el_type('n'); el_type('c');
    el_submit_text();
    CHECK(el_state == EL_ST_FBK && el_wrong == 2u, "type-in: wrong word -> FBK");
    el_state = EL_ST_PLAY;
    el_typed_n = 3;
    el_typed[0] = 'H'; el_typed[1] = 'Y'; el_typed[2] = 'D';
    el_typed_n--;
    CHECK(el_typed_n == 2, "type-in: DEL backspace");
    el_type('9');
    CHECK(el_typed_n == 2, "type-in: digits ignored");
    el_type('h');
    {
        key_event_t ev = { K_CHAR, 'h', true };
        elements_on_key(&ev);
    }
    CHECK(el_typed_n == 3, "type-in: repeated char ignored (is_repeat)");
    el_type('x'); el_type('y'); el_type('z'); el_type('a'); el_type('b');
    el_type('c'); el_type('d'); el_type('e'); el_type('f');
    CHECK(el_typed_n == EL_MAX_LEN - 1, "type-in: buffer capped at MAX_LEN-1");
}

/* ---- 计时与状态流转 ---- */
static void test_timer(void) {
    rng_seed(&el_rng, 0xDDD5u);
    el_state = EL_ST_PLAY;
    el_mode = 0;
    el_time_ms = 150;
    el_fbk_ticks = 0;
    el_best = 0; el_score = 0;
    elements_tick(0);
    CHECK(el_time_ms == 50u && el_state == EL_ST_PLAY, "timer: decrements per tick");
    elements_tick(0);
    CHECK(el_state == EL_ST_OVER, "timer: 0 -> OVER");
    CHECK(el_best == 0u, "timer: best kept (0 >= 0)");
    elements_tick(0);
    CHECK(el_state == EL_ST_OVER, "timer: tick idle in OVER");
    /* FBK 反馈节拍 */
    el_state = EL_ST_FBK;
    el_fbk_ticks = 2;
    el_time_ms = 500;
    elements_tick(0);
    CHECK(el_fbk_ticks == 1 && el_state == EL_ST_FBK, "tick: FBK counts down");
    elements_tick(0);
    CHECK(el_state == EL_ST_PLAY, "tick: FBK end -> next question");
    /* 时间到压过 FBK */
    el_state = EL_ST_FBK;
    el_fbk_ticks = 5;
    el_time_ms = 50;
    elements_tick(0);
    CHECK(el_state == EL_ST_OVER, "tick: timeout during FBK -> OVER");
    /* best 更新 */
    el_state = EL_ST_PLAY;
    el_time_ms = 100;
    el_score = 70;
    el_best = 50;
    elements_tick(0);
    CHECK(el_best == 70u, "timer: best updated on OVER with higher score");
}

/* ---- 模式选择 + 键位 ---- */
static void test_mode_keys(void) {
    key_event_t ev;
    el_state = EL_ST_MODE;
    el_sel = 0;
    ev.key = K_DOWN; ev.ch = 0; ev.is_repeat = false;
    elements_on_key(&ev);
    CHECK(el_sel == 1, "mode: DOWN toggles to TYPE-IN");
    ev.key = K_UP;
    elements_on_key(&ev);
    CHECK(el_sel == 0, "mode: UP toggles back");
    ev.key = K_OK;
    elements_on_key(&ev);
    CHECK(el_state == EL_ST_PLAY && el_mode == 0, "mode: OK starts 4-CHOICE");
    /* 4 选 1 光标导航(2x2 循环) */
    el_cur = 0;
    ev.key = K_RIGHT; elements_on_key(&ev);
    CHECK(el_cur == 1, "nav: RIGHT 0->1");
    ev.key = K_DOWN; elements_on_key(&ev);
    CHECK(el_cur == 3, "nav: DOWN 1->3");
    ev.key = K_LEFT; elements_on_key(&ev);
    CHECK(el_cur == 2, "nav: LEFT 3->2");
    ev.key = K_UP; elements_on_key(&ev);
    CHECK(el_cur == 0, "nav: UP 2->0");
    /* 方向键重复可响应 */
    {
        key_event_t rep = { K_LEFT, 0, true };
        elements_on_key(&rep);
        CHECK(el_cur == 1, "nav: repeat LEFT moves");
    }
    /* N 新局: 同模式重开且清零 */
    el_score = 90; el_time_ms = 100;
    ev.key = K_CHAR; ev.ch = 'n'; ev.is_repeat = false;
    elements_on_key(&ev);
    CHECK(el_state == EL_ST_PLAY && el_mode == 0 && el_score == 0u,
          "mc: N restarts same mode, score reset");
    CHECK(el_time_ms == EL_GAME_MS, "mc: N restarts timer");
    /* TYPE-IN 模式: 字母入缓冲, N 不入 */
    el_mode = 1;
    el_typed_n = 0;
    ev.key = K_CHAR; ev.ch = 'n';
    elements_on_key(&ev);
    CHECK(el_typed_n == 1 && el_typed[0] == 'N',
          "type-in: letter N goes to buffer, not restart");
    /* OK 键重复忽略 */
    ev.key = K_OK; ev.is_repeat = true;
    elements_on_key(&ev);
    CHECK(el_state == EL_ST_PLAY, "type-in: repeated OK ignored");
    /* OVER 态: OK 回到模式选择 */
    el_state = EL_ST_OVER;
    ev.is_repeat = false;
    ev.key = K_OK;
    elements_on_key(&ev);
    CHECK(el_state == EL_ST_MODE, "over: OK -> mode select (retry)");
}

int main(void) {
    test_bank();
    test_opts();
    test_mc_submit();
    test_typein();
    test_timer();
    test_mode_keys();
    printf(s_fail ? "RESULT: %d FAILED\n" : "RESULT: ALL PASS\n", s_fail);
    return s_fail ? 1 : 0;
}
