/* MATCHSTICK 核心逻辑测试(手写补充, 离线验证题库 + 游戏流程) */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "rng.h"
#include "platform/input.h"
#include "platform/time.h"
#include "ui/ui_common.h"
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }
#include "../../src/games/matchstick.c"
static int s_fail = 0;
#define CHECK(c, m) do { if(!(c)){printf("FAIL: %s\n", m); s_fail++;} else printf("ok: %s\n", m);} while(0)

/* 模拟按键 */
static void ms_press(int key, char ch, bool rep) {
    key_event_t ev;
    ev.key = (ccg_key)key;
    ev.ch = (uint8_t)ch;
    ev.is_repeat = rep;
    matchstick_on_key(&ev);
}
static void ms_tap(int key) { ms_press(key, 0, false); }
static void ms_char(char ch) { ms_press(K_CHAR, ch, false); }

int main(void) {
    /* ---- 1. 七段码表: 完整且互异 ---- */
    {
        int dups = 0;
        for (int i = 0; i < 10; i++) {
            CHECK(ms_digit_of(ms_digit[i]) == i, "digit code roundtrip");
            for (int j = i + 1; j < 10; j++)
                if (ms_digit[i] == ms_digit[j]) dups++;
        }
        CHECK(dups == 0, "digit codes distinct");
        CHECK(ms_digit_of(0xFF) == -1, "invalid mask not a digit");
        CHECK(ms_digit[8] == 127u, "8 = all seven segments");
    }

    /* ---- 2. 字形合法性 ---- */
    {
        CHECK(ms_glyph_ok(1, MS_OP_PLUS), "op plus mask ok");
        CHECK(ms_glyph_ok(1, MS_OP_MINUS), "op minus mask ok");
        CHECK(!ms_glyph_ok(1, 0u), "empty op cell invalid");
        CHECK(!ms_glyph_ok(1, 2u), "vertical-only op cell invalid");
        CHECK(ms_glyph_ok(3, MS_OP_EQ), "eq mask ok");
        CHECK(!ms_glyph_ok(3, MS_OP_MINUS), "single-bar eq invalid");
        CHECK(ms_nslots(0) == 7 && ms_nslots(2) == 7 && ms_nslots(4) == 7,
              "digit cells 7 slots");
        CHECK(ms_nslots(1) == 2 && ms_nslots(3) == 2, "op cells 2 slots");
    }

    /* ---- 3. 求值 ---- */
    {
        uint8_t m[5];
        m[0] = ms_digit[3]; m[1] = MS_OP_PLUS; m[2] = ms_digit[4];
        m[3] = MS_OP_EQ; m[4] = ms_digit[7];
        CHECK(ms_eval(m), "3+4=7 true");
        m[4] = ms_digit[8];
        CHECK(!ms_eval(m), "3+4=8 false");
        m[1] = MS_OP_MINUS; m[2] = ms_digit[1]; m[4] = ms_digit[2];
        CHECK(ms_eval(m), "3-1=2 true");
        m[1] = MS_OP_MINUS; m[0] = ms_digit[1]; m[2] = ms_digit[7];
        CHECK(!ms_eval(m), "1-7=2 false (negative result)");
        m[1] = 0;   /* 非法运算符 */
        CHECK(!ms_eval(m), "invalid op eval false");
    }

    /* ---- 4. 题库离线验证(每题: 错误等式 + 唯一一步正解) ---- */
    {
        for (int i = 0; i < MS_NPUZ; i++) {
            const ms_puzzle_t *p = &ms_puzzles[i];
            uint8_t w[5];
            w[0] = ms_digit[p->a]; w[1] = p->op; w[2] = ms_digit[p->b];
            w[3] = MS_OP_EQ; w[4] = ms_digit[p->c];
            CHECK(ms_glyph_ok(1, p->op), "puzzle op glyph valid");
            CHECK(ms_digit_of(w[0]) >= 0 && ms_digit_of(w[2]) >= 0 &&
                  ms_digit_of(w[4]) >= 0, "puzzle digits valid");
            CHECK(!ms_eval(w), "wrong state not true");
            /* 题面解可走通 */
            uint8_t sol[5];
            CHECK(ms_try_move(w, p->pc, p->ps, p->dc, p->ds, sol),
                  "intended move legal");
            CHECK(ms_eval(sol), "intended move makes equation true");
            /* 唯一性: 恰一个解字形 */
            CHECK(ms_count_solutions(w) == 1, "unique one-move solution");
            /* 解不碰 '='; 源槽亮、目标槽暗 */
            CHECK(p->pc != 3 && p->dc != 3, "solution avoids = cell");
            CHECK((w[p->pc] & (1u << p->ps)) != 0, "pick slot lit in puzzle");
            CHECK((w[p->dc] & (1u << p->ds)) == 0, "drop slot dark in puzzle");
        }
        printf("  (%d puzzles verified)\n", MS_NPUZ);
    }

    /* ---- 5. 完整游戏流程(键盘驱动): 题 0 = "1+5=2" ---- */
    {
        rng_seed(&ms_rng, 42);
        ms_puzzle = 0;
        ms_load_puzzle(0);
        CHECK(ms_state == MS_PLAY && ms_moves == 0, "load resets state");
        CHECK(ms_mask[0] == ms_digit[1] && ms_mask[1] == MS_OP_PLUS &&
              ms_mask[2] == ms_digit[5] && ms_mask[4] == ms_digit[2],
              "puzzle 0 is 1+5=2");

        /* 光标边界: 格 4 右 -> 0 (数字格 7 槽保留); 数字格 -> 运算符格钳位 */
        ms_cur_cell = 4; ms_cur_slot = 6;
        ms_tap(K_RIGHT);
        CHECK(ms_cur_cell == 0 && ms_cur_slot == 6, "cell wrap right keeps slot");
        ms_tap(K_RIGHT);
        CHECK(ms_cur_cell == 1 && ms_cur_slot == 0, "slot clamped on op cell");
        ms_cur_cell = 4; ms_cur_slot = 6;
        ms_tap(K_DOWN);
        CHECK(ms_cur_slot == 0, "slot wrap down");
        ms_tap(K_UP);
        CHECK(ms_cur_slot == 6, "slot wrap up");
        ms_tap(K_UP);
        CHECK(ms_cur_slot == 5, "slot cycle");

        /* 空槽不可拿起: '1' 的 T(槽0) 未亮 */
        ms_cur_cell = 0; ms_cur_slot = 0;
        ms_tap(K_OK);
        CHECK(ms_state == MS_PLAY, "pick empty slot ignored");

        /* '=' 不可拿起 */
        ms_cur_cell = 3; ms_cur_slot = 0;
        ms_tap(K_OK);
        CHECK(ms_state == MS_PLAY, "pick from = ignored");

        /* 拿起 op 的竖(V=槽1): 方向键到位 */
        ms_cur_cell = 1; ms_cur_slot = 0;
        ms_tap(K_DOWN);
        CHECK(ms_cur_cell == 1 && ms_cur_slot == 1, "cursor at op V slot");
        ms_tap(K_OK);
        CHECK(ms_state == MS_CARRY && ms_pc == 1 && ms_ps == 1,
              "pick up op vertical");

        /* 放到已有火柴的槽 -> 取消 */
        ms_cur_cell = 2; ms_cur_slot = 0;      /* '5' 的 T 已亮 */
        ms_tap(K_OK);
        CHECK(ms_state == MS_PLAY && ms_moves == 0,
              "drop on occupied slot cancelled");
        CHECK(ms_status == MS_ST_TAKEN, "occupied status set");

        /* 放到 '=' -> 保持手持 */
        ms_cur_cell = 1; ms_cur_slot = 1;
        ms_tap(K_OK);
        CHECK(ms_state == MS_CARRY, "re-pick after cancel");
        ms_cur_cell = 3; ms_cur_slot = 1;
        ms_tap(K_OK);
        CHECK(ms_state == MS_CARRY && ms_status == MS_ST_EQFIX,
              "drop on = refused, still holding");

        /* 原位放回 = 取消 */
        ms_cur_cell = 1; ms_cur_slot = 1;
        ms_tap(K_OK);
        CHECK(ms_state == MS_PLAY && ms_moves == 0 && ms_status == MS_ST_SAME,
              "drop back on source cancels");
        CHECK(ms_mask[1] == MS_OP_PLUS, "op mask restored after cancel");

        /* 解: 拿起 op V, 放到 '1' 的 T -> 7-5=2 WIN, 计 1 步 */
        ms_cur_cell = 1; ms_cur_slot = 1;
        ms_tap(K_OK);
        ms_cur_cell = 0; ms_cur_slot = 0;
        ms_tap(K_OK);
        CHECK(ms_state == MS_WIN && ms_moves == 1, "solution move wins");
        CHECK(ms_eval(ms_mask), "win state equation true");
        CHECK(ms_digit_of(ms_mask[0]) == 7 && ms_mask[1] == MS_OP_MINUS,
              "win state is 7-5=2");

        /* WIN: OK/N -> 下一题; BACK/Q -> 退出 */
        s_exit_request = false;
        ms_char('n');
        CHECK(ms_state == MS_PLAY && ms_moves == 0 &&
              ms_mask[1] == MS_OP_PLUS && ms_digit_of(ms_mask[0]) == 9,
              "N from win loads next puzzle (9+1=0)");
        /* 解题 1 (9+1=0): op V -> M of 0, 得到 9-1=8 */
        ms_cur_cell = 1; ms_cur_slot = 1; ms_tap(K_OK);
        ms_cur_cell = 4; ms_cur_slot = 3; ms_tap(K_OK);
        CHECK(ms_state == MS_WIN && ms_moves == 1, "puzzle 1 solved");
        CHECK(ms_digit_of(ms_mask[4]) == 8 && ms_mask[1] == MS_OP_MINUS,
              "win state is 9-1=8");
        /* 重复的确认键被忽略 */
        key_event_t rep;
        rep.key = K_OK; rep.ch = 0; rep.is_repeat = true;
        matchstick_on_key(&rep);
        CHECK(ms_state == MS_WIN, "repeat OK ignored in win");
        /* BACK 从 WIN 退出 */
        s_exit_request = false;
        ms_tap(K_BACK);
        CHECK(s_exit_request, "BACK quits from win");
    }

    /* ---- 6. 错误但合法的走法 + 计步 + 无解提示 ---- */
    {
        rng_seed(&ms_rng, 7);
        ms_puzzle = 0;
        ms_load_puzzle(0);                       /* 1+5=2 */
        /* 拿起 op V 放到 '5' 的 UR -> 1+9=2(合法但错) */
        ms_cur_cell = 1; ms_cur_slot = 1; ms_tap(K_OK);
        ms_cur_cell = 2; ms_cur_slot = 2; ms_tap(K_OK);
        CHECK(ms_state == MS_PLAY && ms_moves == 1,
              "wrong-but-valid move counted, back to play");
        CHECK(ms_digit_of(ms_mask[2]) == 9 && ms_mask[1] == MS_OP_MINUS,
              "state is 1-9=2");
        CHECK(!ms_eval(ms_mask), "wrong state still false");
        CHECK(ms_count_solutions(ms_mask) == 1, "undo is the only fix");
        CHECK(ms_status == MS_ST_WRONG, "wrong status shown");
        /* 撤销(拿起 9 的 UR 放回 op V) */
        ms_cur_cell = 2; ms_cur_slot = 2; ms_tap(K_OK);
        ms_cur_cell = 1; ms_cur_slot = 1; ms_tap(K_OK);
        CHECK(ms_moves == 2 && ms_mask[1] == MS_OP_PLUS &&
              ms_digit_of(ms_mask[2]) == 5, "undo move works");
        /* 题 6 (1-8=8): 走到无解局面并验证提示 */
        ms_puzzle = 6;
        ms_load_puzzle(6);
        ms_cur_cell = 4; ms_cur_slot = 3; ms_tap(K_OK);   /* 拿起右 8 的中横 */
        ms_cur_cell = 0; ms_cur_slot = 0; ms_tap(K_OK);   /* 放到 1 的上横 */
        CHECK(ms_state == MS_PLAY && ms_moves == 1 &&
              ms_digit_of(ms_mask[0]) == 7 && ms_digit_of(ms_mask[4]) == 0,
              "move makes 7-8=0 (valid glyphs, false)");
        CHECK(ms_count_solutions(ms_mask) == 0,
              "7-8=0 has no one-move fix");
        CHECK(ms_status == MS_ST_NOHINT, "no-fix status shown");
        /* 非法落子(字形坏): 保持手持 */
        ms_cur_cell = 4; ms_cur_slot = 1; ms_tap(K_OK);   /* 拿起 0 的左上竖 */
        ms_cur_cell = 0; ms_cur_slot = 4; ms_tap(K_OK);   /* 放到 7 的左下 */
        CHECK(ms_state == MS_CARRY && ms_status == MS_ST_INVALID,
              "invalid drop keeps holding");
    }

    /* ---- 7. 洗牌确定性(同种子同序) ---- */
    {
        rng_seed(&ms_rng, 1234);
        int order_a[MS_NPUZ];
        for (int i = 0; i < MS_NPUZ; i++) ms_order[i] = i;
        for (int i = MS_NPUZ - 1; i > 0; i--) {
            int j = (int)rng_range(&ms_rng, (uint32_t)(i + 1));
            int t = ms_order[i]; ms_order[i] = ms_order[j]; ms_order[j] = t;
        }
        for (int i = 0; i < MS_NPUZ; i++) order_a[i] = ms_order[i];
        rng_seed(&ms_rng, 1234);
        for (int i = 0; i < MS_NPUZ; i++) ms_order[i] = i;
        for (int i = MS_NPUZ - 1; i > 0; i--) {
            int j = (int)rng_range(&ms_rng, (uint32_t)(i + 1));
            int t = ms_order[i]; ms_order[i] = ms_order[j]; ms_order[j] = t;
        }
        int same = 1;
        for (int i = 0; i < MS_NPUZ; i++)
            if (order_a[i] != ms_order[i]) same = 0;
        CHECK(same, "shuffle deterministic per seed");
        int perm = 1;
        int seen[MS_NPUZ] = {0};
        for (int i = 0; i < MS_NPUZ; i++) {
            if (ms_order[i] < 0 || ms_order[i] >= MS_NPUZ) perm = 0;
            else seen[ms_order[i]]++;
        }
        for (int i = 0; i < MS_NPUZ; i++)
            if (seen[i] != 1) perm = 0;
        CHECK(perm, "shuffle is a permutation");
    }

    /* ---- 8. 渲染冒烟(帧缓冲可安全绘制) ---- */
    {
        rng_seed(&ms_rng, 99);
        ms_puzzle = 0;
        ms_load_puzzle(0);
        matchstick_render();
        /* 顶栏标题首像素应有内容 */
        CHECK(g_fb[0] != 0, "HUD title pixels drawn");
        ms_cur_cell = 1; ms_cur_slot = 1;
        ms_tap(K_OK);                            /* 拿起 */
        matchstick_render();
        CHECK(ms_state == MS_CARRY, "render in carry state safe");
    }

    if (s_fail == 0) { printf("ALL MATCHSTICK TESTS PASSED\n"); return 0; }
    printf("%d FAILED\n", s_fail);
    return 1;
}
