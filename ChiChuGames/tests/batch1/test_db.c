/* DOTS & BOXES host 逻辑单测 — 链接 host 版框架模块, 零平台依赖 */
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "gfx/font_gen.h"
#include "platform/input.h"
#include "rng.h"

#include "../../src/games/dotsbox.c"

static void load_cursor(int seg, int x, int y) { db_seg = seg; db_ex = x; db_ey = y; }
static int cursor_ok(void) { return 1; }


/* ---- host 框架 stub ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 保存/恢复光标态, 便于穷举移动 */
static int cursor_to_edge(int seg, int x, int y) {
    return (seg == 0) ? db_eh(y, x) : db_ev(x, y);
}

/* 1. 边索引映射: eh/ev 全部唯一且在 0..39 */
static void test_indexing(void) {
    int seen[DB_EDGES];
    memset(seen, 0, sizeof(seen));
    for (int r = 0; r <= 4; r++)
        for (int c = 0; c < 4; c++) seen[db_eh(r, c)]++;
    for (int c = 0; c <= 4; c++)
        for (int r = 0; r < 4; r++) seen[db_ev(c, r)]++;
    int ok = 1;
    for (int i = 0; i < DB_EDGES; i++)
        if (seen[i] != 1) ok = 0;
    CHECK(ok, "eh/ev map to all 40 edges exactly once");
    CHECK(db_eh(0, 0) == 0 && db_eh(4, 3) == 19, "eh bounds 0..19");
    CHECK(db_ev(0, 0) == 20 && db_ev(4, 3) == 39, "ev bounds 20..39");
    CHECK(db_eh(2, 1) == 9 && db_ev(3, 2) == 34, "index arithmetic spot check");
}

/* 2. 光标边索引与初始态 */
static void test_cursor_edge(void) {
    db_new();
    CHECK(db_cursor_edge() == 0, "initial cursor on edge 0");
    load_cursor(0, 1, 2); CHECK(db_cursor_edge() == db_eh(2, 1) && db_cursor_edge() == 9,
                               "H cursor -> eh index");
    load_cursor(1, 3, 1); CHECK(db_cursor_edge() == db_ev(3, 1) && db_cursor_edge() == 33,
                               "V cursor -> ev index");
}

/* 3. 穷举移动合法性: 40 态 x 4 方向, 结果恒为合法边 */
static void test_move_validity(void) {
    for (int seg = 0; seg < 2; seg++)
        for (int x = 0; x <= 4; x++)
            for (int y = 0; y <= 4; y++) {
                if (cursor_to_edge(seg, x, y) < 0) continue;   /* 非法组合(H 需 y<=4 x<=3 等) */
                if (!((seg == 0 && y <= 4 && x <= 3) || (seg == 1 && x <= 4 && y <= 3)))
                    continue;
                for (int d = 0; d < 4; d++) {
                    load_cursor(seg, x, y);
                    db_cursor_move(d);
                    if (!cursor_ok() || db_cursor_edge() < 0 || db_cursor_edge() >= DB_EDGES) {
                        printf("FAIL: bad move seg=%d x=%d y=%d dir=%d -> seg=%d x=%d y=%d edge=%d\n",
                               seg, x, y, d, db_seg, db_ex, db_ey, db_cursor_edge());
                        s_fail++;
                        return;
                    }
                }
            }
    CHECK(1, "all 40 cursor states x 4 dirs stay on valid edges");
}

/* 4. 移动图连通性: BFS 从起点可达全部 40 条边 */
static void test_move_reach(void) {
    int vis[DB_EDGES];
    memset(vis, 0, sizeof(vis));
    int queue[DB_EDGES][3], qh = 0, qt = 0;
    load_cursor(0, 0, 0);
    int e = db_cursor_edge();
    vis[e] = 1;
    queue[qt][0] = 0; queue[qt][1] = 0; queue[qt][2] = 0; qt++;
    while (qh < qt) {
        int seg = queue[qh][0], x = queue[qh][1], y = queue[qh][2]; qh++;
        for (int d = 0; d < 4; d++) {
            load_cursor(seg, x, y);
            db_cursor_move(d);
            if (!vis[db_cursor_edge()]) {
                vis[db_cursor_edge()] = 1;
                if (qt < DB_EDGES) {
                    queue[qt][0] = db_seg; queue[qt][1] = db_ex; queue[qt][2] = db_ey; qt++;
                }
            }
        }
    }
    int all = 1;
    for (int i = 0; i < DB_EDGES; i++)
        if (!vis[i]) { printf("FAIL: edge %d unreachable\n", i); all = 0; }
    CHECK(all, "cursor movement graph reaches all 40 edges");
}

/* 5. 格边数统计 */
static void test_sides(void) {
    db_new();
    CHECK(db_sides(0, 0) == 0, "empty box has 0 sides");
    db_edges[db_eh(0, 0)] = 1;
    db_edges[db_ev(0, 0)] = 1;
    db_edges[db_ev(1, 0)] = 1;
    CHECK(db_sides(0, 0) == 3, "box with 3 sides detected");
    CHECK(db_sides(1, 1) == 0, "unrelated box still 0");
}

/* 6. 单格捕获: 第四条边 → 归属当前玩家, 不换手 */
static void test_single_capture(void) {
    db_new();
    db_place(1, db_eh(0, 0));   /* 顶 */
    db_place(2, db_ev(0, 0));   /* 左 */
    db_place(1, db_ev(1, 0));   /* 右 */
    db_place(2, db_eh(2, 0));   /* 无关边, 使轮到玩家 */
    CHECK(db_sides(0, 0) == 3 && db_turn == 1, "3 sides, player to move");
    int cap = db_place(1, db_eh(1, 0));   /* 底 = 第 4 条 */
    CHECK(cap == 1, "4th side captures 1 box");
    CHECK(db_cells[0] == 1, "box owned by player");
    CHECK(db_count(1) == 1 && db_count(2) == 0, "score counted");
    CHECK(db_turn == 1, "capturer keeps turn");
}

/* 7. 双格捕获: 一条边同时完成两格 */
static void test_double_capture(void) {
    db_new();
    /* 格(0,0): 顶 H(0,0)=0; 格(1,0): 底 H(2,0)=8 */
    db_place(1, db_eh(0, 0));
    db_place(2, db_eh(2, 0));
    db_place(1, db_ev(0, 0));   /* 左(0,0) 公共边 */
    db_place(2, db_ev(0, 1));   /* 左(1,0) */
    db_place(1, db_ev(1, 0));   /* 右(0,0) 公共边 */
    db_place(2, db_ev(1, 1));   /* 右(1,0) */
    CHECK(db_sides(0, 0) == 3 && db_sides(1, 0) == 3, "both boxes 3-sided");
    int cap = db_place(1, db_eh(1, 0));   /* 两格公共的底/顶 */
    CHECK(cap == 2, "one edge captures 2 boxes");
    CHECK(db_cells[0] == 1 && db_cells[4] == 1, "both boxes to player");
    CHECK(db_turn == 1, "still player's turn after double capture");
}

/* 8. 不捕获换手 */
static void test_turn_switch(void) {
    db_new();
    CHECK(db_turn == 1 && db_moves == 0, "new game: player first, 0 moves");
    db_place(1, 0);
    CHECK(db_turn == 2 && db_moves == 1, "non-capturing move switches to AI");
    db_place(2, 1);
    CHECK(db_turn == 1, "AI non-capture switches back");
    CHECK(db_cells[0] == 0, "no box claimed");
}

/* 9. AI 捕获计数 */
static void test_capture_count(void) {
    db_new();
    db_edges[db_eh(0, 0)] = 1;
    db_edges[db_ev(0, 0)] = 1;
    db_edges[db_ev(1, 0)] = 1;
    CHECK(db_capture_count(db_eh(1, 0)) == 1, "completing edge detected");
    CHECK(db_capture_count(db_eh(2, 0)) == 0, "unrelated edge no capture");
    CHECK(db_gift_count(db_eh(1, 0)) == 0, "completing edge gifts nothing");
}

/* 10. AI: 有捕获必选(即使存在大量安全边) */
static void test_ai_capture_priority(void) {
    db_new();
    rng_seed(&db_rng, 12345);
    db_edges[db_eh(0, 0)] = 1;
    db_edges[db_ev(0, 0)] = 2;
    db_edges[db_ev(1, 0)] = 1;
    int idx = db_ai_move();
    CHECK(idx == db_eh(1, 0), "AI takes the capture edge (index 4)");
    CHECK(db_capture_count(idx) == 1, "chosen edge indeed captures");
}

/* 11. AI: 有安全边时不送格 */
static void test_ai_avoid_gift(void) {
    db_new();
    rng_seed(&db_rng, 777);
    /* 格(0,0) 2 边(V 左+右) → H(0,0)/H(1,0) 是送格边; 其余大部分边安全 */
    db_edges[db_ev(0, 0)] = 1;
    db_edges[db_ev(1, 0)] = 2;
    for (int t = 0; t < 8; t++) {
        int idx = db_ai_move();
        CHECK(idx != db_eh(0, 0) && idx != db_eh(1, 0), "AI avoids gifting edge");
        if (idx == db_eh(0, 0) || idx == db_eh(1, 0)) return;
        CHECK(db_capture_count(idx) == 0 && db_gift_count(idx) == 0, "chosen edge is safe");
    }
}

/* 12. AI: 无安全边 → 最小送格 */
static void test_ai_min_gift(void) {
    db_new();
    rng_seed(&db_rng, 4242);
    /* 仅空边 H(0,0)=0 与 V(0,0)=20: 格(0,0) 恰 2 边已画(H(1,0)+V(1,0)),
     * 两条候选边都恰好送 1 格, 无捕获; 其余格全 4 边(不影响计数) */
    for (int i = 0; i < DB_EDGES; i++) db_edges[i] = 1;
    db_edges[db_eh(0, 0)] = 0;
    db_edges[db_ev(0, 0)] = 0;
    CHECK(db_capture_count(db_eh(0, 0)) == 0 && db_gift_count(db_eh(0, 0)) == 1,
          "H(0,0) is a pure gift");
    CHECK(db_capture_count(db_ev(0, 0)) == 0 && db_gift_count(db_ev(0, 0)) == 1,
          "V(0,0) is a pure gift");
    for (int t = 0; t < 8; t++) {
        int idx = db_ai_move();
        CHECK(idx == db_eh(0, 0) || idx == db_ev(0, 0), "AI picks a min-gift edge");
        CHECK(db_capture_count(idx) == 0 && db_gift_count(idx) == 1, "gift count is minimal");
    }
}

/* 13. AI: 最后一条空边 */
static void test_ai_last_edge(void) {
    db_new();
    rng_seed(&db_rng, 99);
    for (int i = 0; i < DB_EDGES; i++)
        if (i != db_ev(1, 1)) db_edges[i] = 1;
    db_edges[db_ev(1, 1)] = 0;
    int idx = db_ai_move();
    CHECK(idx == db_ev(1, 1), "AI returns the only empty edge");
}

/* 14. 完整对局模拟(AI vs AI): 终局一致性与 AI 不重复画边 */
static void test_full_game(void) {
    static const uint64_t seeds[] = { 1, 42, 20260830, 0xDEADBEEF, 31337 };
    for (unsigned s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
        db_new();
        rng_seed(&db_rng, seeds[s]);
        int guard = 0;
        while (!db_over) {
            int idx = db_ai_move();
            CHECK(idx >= 0 && idx < DB_EDGES && db_edges[idx] == 0, "AI edge valid & unused");
            db_place(db_turn == 1 ? 1 : 2, idx);
            db_check_end();
            guard++;
            if (guard > 100) { printf("FAIL: game did not end (seed %llu)\n",
                                      (unsigned long long)seeds[s]); s_fail++; break; }
        }
        int drawn = 0, total = 0;
        for (int i = 0; i < DB_EDGES; i++)
            if (db_edges[i]) drawn++;
        CHECK(drawn == DB_EDGES, "all 40 edges drawn at end");
        CHECK(db_moves == DB_EDGES, "move counter matches");
        CHECK(db_over, "game over flag set");
        total = db_count(1) + db_count(2);
        CHECK(total == DB_GRID * DB_GRID, "all 16 boxes owned");
        CHECK(db_count(1) >= 0 && db_count(2) >= 0 && db_count(1) + db_count(2) == 16,
              "score partition consistent");
        /* 复检: 每个已捕获格的 4 条边都确实画了 */
        int ok = 1;
        for (int r = 0; r < DB_GRID; r++)
            for (int c = 0; c < DB_GRID; c++)
                if (db_cells[r * DB_GRID + c] && db_sides(r, c) != 4) ok = 0;
        CHECK(ok, "every owned box has all 4 sides drawn");
    }
}

/* 15. on_key: 方向/OK/连招/重复忽略/重开 */
static void test_on_key(void) {
    key_event_t ev;
    db_new();
    /* 方向键移动 */
    memset(&ev, 0, sizeof(ev));
    ev.key = K_RIGHT;
    dotsbox_on_key(&ev);
    CHECK(db_ex == 1 && db_ey == 0 && db_seg == 0, "RIGHT moves cursor on edge row");
    /* WASD */
    ev.key = K_CHAR; ev.ch = 's';
    dotsbox_on_key(&ev);
    CHECK(db_ey == 1 && db_seg == 0, "s moves down");
    /* 重复确认键忽略 */
    ev.key = K_OK; ev.is_repeat = true;
    dotsbox_on_key(&ev);
    CHECK(db_moves == 0, "repeated OK ignored");
    /* OK 画边 → AI 应手 */
    ev.is_repeat = false;
    load_cursor(0, 0, 0);
    dotsbox_on_key(&ev);
    CHECK(db_moves == 2 && db_turn == 1, "player edge + AI reply, back to player");
    CHECK(db_edges[db_cursor_edge()] != 0, "cursor edge now drawn (AI may have taken it)");
    /* 已画边上 OK 无操作 */
    int m = db_moves;
    dotsbox_on_key(&ev);
    CHECK(db_moves == m, "OK on drawn edge is no-op");
    /* 'n' 重开 */
    ev.key = K_CHAR; ev.ch = 'n';
    dotsbox_on_key(&ev);
    CHECK(db_moves == 0 && !db_over && db_turn == 1, "n restarts game");
}

/* 16. on_key: 捕获连招(玩家捕获后不换手, AI 不应手) */
static void test_capture_chain(void) {
    db_new();
    /* 布置: 格(0,0) 前 3 边 + 1 条无关边, 使轮到玩家且格(0,0) 3 边 */
    db_place(1, db_eh(0, 0));
    db_place(2, db_ev(0, 0));
    db_place(1, db_ev(1, 0));
    db_place(2, db_eh(2, 0));
    CHECK(db_turn == 1 && db_sides(0, 0) == 3, "setup: player turn, box 3-sided");
    key_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.key = K_OK;
    load_cursor(0, 0, 1);       /* H(1,0) = 格(0,0) 底边 */
    dotsbox_on_key(&ev);
    CHECK(db_cells[0] == 1, "OK captures the box");
    CHECK(db_turn == 1, "capturer keeps turn, AI does not reply");
    CHECK(db_moves == 5, "only 5 edges drawn (no AI reply)");
}

/* 17. on_key: 终局重开 / 退出请求 */
static void test_on_key_over(void) {
    db_new();
    for (int i = 0; i < DB_EDGES; i++) db_edges[i] = 1;
    db_moves = DB_EDGES;
    db_check_end();
    CHECK(db_over, "full board -> over");
    key_event_t ev;
    memset(&ev, 0, sizeof(ev));
    s_exit_request = false;
    ev.key = K_BACK;
    dotsbox_on_key(&ev);
    CHECK(s_exit_request, "BACK at game over requests quit");
    ev.key = K_OK;
    dotsbox_on_key(&ev);
    CHECK(!db_over && db_moves == 0, "OK at game over restarts");
}

/* 18. render 冒烟: 像素级验证画边/格标记/光标/终局 */
static void test_render(void) {
    db_new();
    db_place(1, db_eh(0, 0));   /* 顶边 */
    db_place(2, db_ev(0, 0));   /* 左边 */
    db_place(1, db_ev(1, 0));   /* 右边 */
    db_place(1, db_eh(1, 0));   /* 底边 → 格(0,0) 归玩家 */
    load_cursor(0, 0, 0);
    dotsbox_render();
    CHECK(g_fb[(38 >> 3) * (int)CCG_W + (DB_OX + 1)] & (0x80 >> (38 & 7)),
          "drawn edge pixels black");
    CHECK(!(g_fb[(40 >> 3) * (int)CCG_W + (DB_OX + 64)] & (0x80 >> (40 & 7))),
          "undrawn region white");
    CHECK(g_fb[(34 >> 3) * (int)CCG_W + (DB_OX + 16)] & (0x80 >> (34 & 7)),
          "player box marker solid (center black)");
    CHECK(g_fb[(16 >> 3) * (int)CCG_W + (DB_OX + 16)] & (0x80 >> (16 & 7)),
          "cursor band visible above edge");
    CHECK(!(g_fb[(DB_OY >> 3) * (int)CCG_W + (DB_OX + 16)] & (0x80 >> (DB_OY & 7))),
          "drawn edge under cursor inverted");
}

/* 19. render 终局冒烟: AI 空心标记 + 全画满不死 */
static void test_render_over(void) {
    db_new();
    /* 让格(0,0) 归 AI */
    db_place(1, db_eh(0, 0));
    db_place(2, db_ev(0, 0));
    db_place(1, db_ev(1, 0));
    db_place(2, db_eh(1, 0));
    for (int i = 0; i < DB_EDGES; i++) db_edges[i] = 1;
    db_moves = DB_EDGES;
    db_check_end();
    load_cursor(0, 2, 2);
    dotsbox_render();
    CHECK(db_over_full, "over-full flag set after render");
    /* AI 空心: 中心白, 边框黑 */
    int cx = DB_OX + 16, cy = DB_OY + 16;
    CHECK(!(g_fb[(cy >> 3) * (int)CCG_W + cx] & (0x80 >> (cy & 7))), "AI marker hollow center");
    CHECK(g_fb[(26 >> 3) * (int)CCG_W + (DB_OX + 8)] & (0x80 >> (26 & 7)),
          "AI marker border black");
    /* HUD 区有结束文案(非全白) */
    int hud_ink = 0;
    for (int i = 0; i < (CCG_HUD_H - 1) * (int)CCG_W; i++)
        if (g_fb[i]) hud_ink++;
    CHECK(hud_ink > 0, "HUD shows result text at game over");
}

int main(void) {
    test_indexing();
    test_cursor_edge();
    test_move_validity();
    test_move_reach();
    test_sides();
    test_single_capture();
    test_double_capture();
    test_turn_switch();
    test_capture_count();
    test_ai_capture_priority();
    test_ai_avoid_gift();
    test_ai_min_gift();
    test_ai_last_edge();
    test_full_game();
    test_on_key();
    test_capture_chain();
    test_on_key_over();
    test_render();
    test_render_over();
    if (s_fail) {
        printf("== %d FAILURE(S) ==\n", s_fail);
        return 1;
    }
    printf("== ALL TESTS PASS ==\n");
    return 0;
}
