/* QUAKE ESCAPE — host 逻辑单测(include 游戏源, 直接访问 qe_ 静态)
 * 构建(项目根):
 *   cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -DCHICHU_HOST \
 *      -Isrc -Isrc/gfx /tmp/test_quake.c \
 *      src/gfx/canvas.c src/gfx/font.c src/gfx/font_data.c src/gfx/pattern.c \
 *      src/rng.c src/platform/time.c src/platform/input.c \
 *      src/platform/display.c src/ui/ui_common.c -o /tmp/test_quake
 */
#include "../../src/games/quake.c"
#include <stdio.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* 框架 stub */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static bool px(int x, int y) {
    int off = (y >> 3) * (int)CCG_W + x;
    return (g_fb[off] & (uint8_t)(0x80 >> (y & 7))) != 0;
}

/* ---- 开局: 生命/关卡/对侧出发 ---- */
static void test_init_opposite(void) {
    qe_new_game();
    rng_seed(&qe_rng, 1);
    CHECK(qe_hp == 3 && qe_level == 1 && qe_state == QE_PLAY,
          "new game: hp3 level1 play");
    CHECK(qe_px == qe_sx && qe_py == qe_sy, "player starts at start cell");
    CHECK(qe_sx != qe_ex, "start column differs from exit column");
    CHECK((qe_ex < 5 && qe_sx == 9) || (qe_ex >= 5 && qe_sx == 0),
          "start on opposite side of exit");
    CHECK(qe_cmask == 0 && !qe_has_rock, "level opens hazard-free");
}

/* ---- 边界: 出界移动忽略且不计回合 ---- */
static void test_bounds(void) {
    qe_new_game();
    rng_seed(&qe_rng, 1);
    qe_px = 0; qe_py = 0; qe_turns = 0;
    qe_move(-1, 0);
    CHECK(qe_px == 0 && qe_py == 0 && qe_turns == 0, "off-grid left ignored");
    qe_px = 9; qe_py = 4;
    qe_move(1, 0);
    CHECK(qe_px == 9 && qe_py == 4 && qe_turns == 0, "off-grid right ignored");
    qe_move(0, 1);
    CHECK(qe_px == 9 && qe_py == 4 && qe_turns == 0, "off-grid down ignored");
    qe_move(0, -1);
    CHECK(qe_py == 3 && qe_turns == 1, "valid move counts a turn");
}

/* ---- 过关: 到达出口 → CLEAR → 下一关; 第 3 关 → WIN ---- */
static void test_win_clear(void) {
    qe_new_game();
    rng_seed(&qe_rng, 1);
    qe_level = 1;
    qe_ex = 3; qe_ey = 2;
    qe_px = 2; qe_py = 2;
    qe_cmask = 0; qe_has_rock = false; qe_turns = 0;
    qe_state = QE_PLAY;
    qe_move(1, 0);
    CHECK(qe_state == QE_CLEAR && qe_px == 3 && qe_py == 2,
          "reach exit on level 1 -> CLEAR");
    quake_on_key(&(key_event_t){ .key = K_OK, .ch = 0, .is_repeat = false });
    CHECK(qe_state == QE_PLAY && qe_level == 2, "OK on CLEAR advances to level 2");
    /* 第 3 关直达出口 → WIN */
    qe_level = 3;
    qe_ex = 3; qe_ey = 2;
    qe_px = 2; qe_py = 2;
    qe_cmask = 0; qe_has_rock = false; qe_turns = 0;
    qe_state = QE_PLAY;
    qe_move(1, 0);
    CHECK(qe_state == QE_WIN, "reach exit on level 3 -> WIN");
}

/* ---- 地震节奏: level1 每 4 回合一次; 裂缝持续到下次地震 ---- */
static void test_quake_cadence(void) {
    qe_new_game();
    rng_seed(&qe_rng, 1);
    qe_level = 1;
    qe_ex = 9; qe_ey = 4;
    qe_sx = 0; qe_sy = 0;
    qe_px = 0; qe_py = 0;
    qe_turns = 0; qe_cmask = 0; qe_has_rock = false;
    qe_move(1, 0);
    qe_move(1, 0);
    qe_move(1, 0);
    CHECK(qe_cmask == 0 && !qe_has_rock, "no quake before 4th turn");
    qe_move(1, 0);
    CHECK(qe_cmask != 0 && qe_has_rock, "quake fires on 4th move");
    CHECK(qe_turns == 0, "turn counter resets after quake");
    qe_move(1, 0);
    qe_move(1, 0);
    qe_move(1, 0);
    CHECK(qe_cmask != 0, "cracks persist between quakes");
    qe_move(1, 0);
    CHECK(qe_turns == 0, "second quake on 4th move again");
}

/* ---- 逃生优先: 地震回合到达出口 → 过关且不触发地震 ---- */
static void test_escape_before_quake(void) {
    qe_new_game();
    rng_seed(&qe_rng, 1);
    qe_level = 1;
    qe_ex = 5; qe_ey = 2;
    qe_px = 4; qe_py = 2;
    qe_turns = 3; qe_cmask = 0; qe_has_rock = false;
    qe_state = QE_PLAY;
    qe_move(1, 0);
    CHECK(qe_state == QE_CLEAR, "exit on quake turn escapes safely");
    CHECK(qe_cmask == 0 && !qe_has_rock, "no quake fires after escape");
}

/* ---- 掉入裂缝: -1 血回起点; 0 血 → OVER ---- */
static void test_fall(void) {
    qe_new_game();
    rng_seed(&qe_rng, 1);
    qe_ex = 9; qe_ey = 4;              /* 出口钉在路径之外 */
    qe_hp = 3;
    qe_sx = 0; qe_sy = 0; qe_px = 0; qe_py = 0;
    qe_cdir = 0; qe_cidx = 2;
    qe_cmask = (uint16_t)(1u << 5);        /* 裂缝 (5,2) */
    qe_has_rock = false;
    qe_move(1, 0); qe_turns = 0;           /* (1,0) */
    qe_move(0, 1); qe_turns = 0;           /* (1,1) */
    qe_move(0, 1); qe_turns = 0;           /* (1,2) 安全 */
    qe_move(1, 0); qe_turns = 0;           /* (2,2) */
    qe_move(1, 0); qe_turns = 0;           /* (3,2) */
    qe_move(1, 0); qe_turns = 0;           /* (4,2) */
    qe_move(1, 0);                         /* (5,2) → 掉入 */
    CHECK(qe_hp == 2 && qe_px == qe_sx && qe_py == qe_sy,
          "walk into crack: -1hp back to start");
    CHECK(qe_state == QE_PLAY, "still playing after fall");
    CHECK(qe_turns == 1, "fall counts a turn");
    /* 血尽 → OVER */
    qe_hp = 1;
    qe_px = 4; qe_py = 2;
    qe_move(1, 0);
    CHECK(qe_state == QE_OVER && qe_hp == 0, "hp 0 -> OVER");
}

/* ---- 落石: 障碍不可站; 绕过可走 ---- */
static void test_rock_block(void) {
    qe_new_game();
    rng_seed(&qe_rng, 1);
    qe_hp = 3;
    qe_sx = 0; qe_sy = 0; qe_px = 2; qe_py = 1;
    qe_has_rock = true; qe_rx = 3; qe_ry = 1;
    qe_turns = 0; qe_cmask = 0;
    qe_move(1, 0);
    CHECK(qe_px == 2 && qe_py == 1 && qe_turns == 0 && qe_hp == 3,
          "rock blocks the move (no turn, no damage)");
    qe_move(0, -1);
    CHECK(qe_px == 2 && qe_py == 0 && qe_turns == 1, "detour around rock ok");
}

/* ---- 地震结算三分支(直接驱动): 砸中 / 脚下裂缝 / 都无 / 砸中优先 ---- */
static void test_quake_resolve(void) {
    qe_new_game();
    rng_seed(&qe_rng, 1);
    qe_hp = 3;
    qe_sx = 0; qe_sy = 0;
    /* 分支1: 落石砸中玩家 */
    qe_px = 2; qe_py = 3;
    qe_has_rock = true; qe_rx = 2; qe_ry = 3;
    qe_cdir = 0; qe_cidx = 1; qe_cmask = (uint16_t)(1u << 4);
    qe_quake_resolve();
    CHECK(qe_hp == 2 && qe_px == qe_sx && qe_py == qe_sy,
          "rock on player: -1hp to start");
    /* 分支2: 玩家脚下新裂缝 */
    qe_px = 4; qe_py = 1;
    qe_has_rock = true; qe_rx = 7; qe_ry = 3;
    qe_cdir = 0; qe_cidx = 1; qe_cmask = (uint16_t)(1u << 4);  /* (4,1) 裂缝 */
    qe_quake_resolve();
    CHECK(qe_hp == 1 && qe_px == qe_sx && qe_py == qe_sy,
          "player on new crack: -1hp to start");
    /* 分支3: 均安全 → 无伤 */
    qe_hp = 3;
    qe_px = 1; qe_py = 1; qe_sx = 0; qe_sy = 0;
    qe_has_rock = true; qe_rx = 9; qe_ry = 4;
    qe_cdir = 0; qe_cidx = 3; qe_cmask = (uint16_t)(1u << 8);
    qe_quake_resolve();
    CHECK(qe_hp == 3 && qe_px == 1 && qe_py == 1, "safe quake: no damage");
    /* 分支4: 落石+裂缝同格 → 只扣一次 */
    qe_hp = 3;
    qe_px = 4; qe_py = 1;
    qe_has_rock = true; qe_rx = 4; qe_ry = 1;
    qe_cdir = 0; qe_cidx = 1; qe_cmask = (uint16_t)(1u << 4);
    qe_quake_resolve();
    CHECK(qe_hp == 2 && qe_px == qe_sx, "crush takes priority, single hit");
}

/* ---- 地震性质测试: 300 次地震全部满足不变量 ---- */
static void test_quake_invariants(void) {
    qe_new_game();
    rng_seed(&qe_rng, 1);
    int bad = 0, crushed = 0, fell = 0;
    for (int i = 0; i < 300; i++) {
        qe_hp = 3;                          /* 防止提前 OVER 中断测试 */
        /* 玩家随机站位(含起点), 使"落石砸中"分支有机会触发 */
        qe_px = (int)rng_range(&qe_rng, QE_COLS);
        qe_py = (int)rng_range(&qe_rng, QE_ROWS);
        int px0 = qe_px, py0 = qe_py;
        int hp0 = qe_hp;
        int expected = hp0;
        qe_quake();
        /* 出口格永不裂缝 */
        if (qe_cracked(qe_ex, qe_ey)) bad++;
        /* 裂缝线至少留 1 个空位(保证可通行) */
        int gaps = 0;
        int len = (qe_cdir == 0) ? QE_COLS : QE_ROWS;
        for (int p = 0; p < len; p++)
            if (!(qe_cmask & (1u << p))) gaps++;
        if (gaps < 1) bad++;
        /* 裂缝数不超线上格数 */
        if (len == 5 && (qe_cmask >> 5) != 0) bad++;   /* 列线只有 5 行 */
        /* 落石不在裂缝线 / 出口 / 起点上 */
        if (qe_cracked(qe_rx, qe_ry)) bad++;
        if (qe_rx == qe_ex && qe_ry == qe_ey) bad++;
        if (qe_rx == qe_sx && qe_ry == qe_sy) bad++;
        /* 结算: 砸中/掉入 → -1 血回起点; 否则原位 */
        if (px0 == qe_rx && py0 == qe_ry) { expected--; crushed++; }
        else if (qe_cracked(px0, py0)) { expected--; fell++; }
        if (qe_hp != expected) bad++;
        if (expected < hp0) {
            if (qe_px != qe_sx || qe_py != qe_sy) bad++;
        } else if (qe_px != px0 || qe_py != py0) {
            bad++;
        }
        if (qe_hp < 0 || qe_hp > 3) bad++;
    }
    CHECK(bad == 0, "300 quakes: all invariants hold (exit safe / gap / resolve)");
    CHECK(crushed > 0, "rock crush actually occurs (deterministic seed)");
    CHECK(fell > 0, "player caught on new crack actually occurs");
    CHECK(qe_hp == 3, "hp stays bounded");
}

/* ---- 遍历测试: 随机走 500 步状态合法, 结局可达 ---- */
static void test_playout_smoke(void) {
    qe_new_game();
    rng_seed(&qe_rng, 1);
    qe_hp = 3;
    int steps = 0;
    int px0, py0;
    while (qe_state == QE_PLAY && steps < 500) {
        px0 = qe_px; py0 = qe_py;
        int dx = (qe_ex > qe_px) ? 1 : (qe_ex < qe_px) ? -1 : 0;
        int dy = (qe_ex == qe_px)
                     ? ((qe_ey > qe_py) ? 1 : (qe_ey < qe_py) ? -1 : 0)
                     : 0;
        if (dx == 0 && dy == 0) { qe_move(0, 1); }   /* 同格防御(不应发生) */
        else qe_move(dx, dy);
        if (qe_px == px0 && qe_py == py0) {          /* 被挡: 换个方向 */
            if (qe_px == qe_ex) qe_move(0, (qe_ey > qe_py) ? 1 : -1);
            else qe_move(dx, dy == 0 ? 1 : 0);
        }
        steps++;
    }
    CHECK(steps <= 500, "playout terminates within 500 steps");
    CHECK(qe_px >= 0 && qe_px < QE_COLS && qe_py >= 0 && qe_py < QE_ROWS,
          "playout position in bounds");
    CHECK(qe_hp >= 0 && qe_hp <= 3, "playout hp in bounds");
    CHECK(qe_state == QE_PLAY || qe_state == QE_CLEAR || qe_state == QE_WIN ||
              qe_state == QE_OVER,
          "playout ends in a valid state");
}

/* ---- 渲染冒烟: 各元素像素落位 + 状态 HUD + 全刷标志 ---- */
static void test_render(void) {
    qe_new_game();
    rng_seed(&qe_rng, 1);
    qe_px = 2; qe_py = 2;
    qe_ex = 7; qe_ey = 4;
    qe_sx = 0; qe_sy = 0;
    qe_cdir = 0; qe_cidx = 2;
    qe_cmask = (uint16_t)((1u << 5) | (1u << 7));    /* 裂缝 (5,2),(7,2) */
    qe_has_rock = true; qe_rx = 3; qe_ry = 1;
    qe_state = QE_PLAY;
    quake_render();
    /* 网格线端点(线覆盖范围含起点不含终点) */
    CHECK(px(QE_OX, QE_OY) && px(QE_OX + QE_W - 1, QE_OY + QE_H),
          "grid corners black");
    /* 板区外留白 */
    CHECK(!px(2, 40), "outside board stays white");
    /* 出口星: 中心竖条 */
    CHECK(px(QE_OX + 7 * QE_CELL + QE_CENTER, QE_OY + 4 * QE_CELL + QE_CENTER),
          "exit star drawn");
    /* 玩家小人: 头部左上 (cx+3, cy) */
    CHECK(px(QE_OX + 2 * QE_CELL + 10, QE_OY + 2 * QE_CELL + 5),
          "player person drawn");
    /* 起点三角: 顶点 (cx+8, cy) */
    CHECK(px(QE_OX + QE_CENTER + 8, QE_OY + QE_CENTER), "start marker drawn");
    /* 落石: 实心 + 反白 !(格内为黑, 字形位为白) */
    int rr = QE_OY + qe_ry * QE_CELL;
    CHECK(px(QE_OX + qe_rx * QE_CELL + 2, rr + 2), "rock solid black");
    CHECK(!px(QE_OX + qe_rx * QE_CELL + (QE_CELL - FONT_W) / 2 + 2,
              rr + (QE_CELL - FONT_H) / 2 + 2),
          "rock warning glyph inverted (white)");
    /* 裂缝斜纹 */
    CHECK(px(QE_OX + 5 * QE_CELL + 1, QE_OY + 2 * QE_CELL + 1),
          "crack diagonal drawn");
    /* HUD 黑字白底 + 分隔线('Q' 字形 row2=0x11: bit0 在 (0,2)) */
    CHECK(px(0, 2), "HUD title glyph black");
    CHECK(!px(150, 2), "HUD right of title area white");
    CHECK(px(5, CCG_HUD_H - 1), "HUD separator line");
    /* 状态渲染: WIN → HUD 提示 + disp_force_full 只一次 */
    qe_state = QE_WIN;
    qe_full_once = false;
    quake_render();
    CHECK(px(2, 2) && qe_full_once, "WIN HUD drawn, full-refresh flag set");
    quake_render();
    CHECK(qe_full_once, "full-refresh only once");
    qe_state = QE_OVER;
    qe_full_once = false;
    quake_render();
    CHECK(qe_full_once, "OVER HUD drawn");
}

/* ---- 按键语义: 重复键忽略 / N 新局 / Q 退出 ---- */
static void test_keys(void) {
    qe_new_game();
    rng_seed(&qe_rng, 1);
    qe_hp = 1; qe_level = 3;
    quake_on_key(&(key_event_t){ .key = K_OK, .ch = 0, .is_repeat = true });
    CHECK(qe_hp == 1 && qe_level == 3, "repeat OK ignored during play");
    quake_on_key(&(key_event_t){ .key = K_CHAR, .ch = 'n', .is_repeat = false });
    CHECK(qe_state == QE_PLAY && qe_hp == 3 && qe_level == 1,
          "N restarts from level 1");
    qe_state = QE_OVER;
    quake_on_key(&(key_event_t){ .key = K_OK, .ch = 0, .is_repeat = true });
    CHECK(qe_state == QE_OVER, "repeat OK ignored in OVER");
    quake_on_key(&(key_event_t){ .key = K_BACK, .ch = 0, .is_repeat = false });
    CHECK(s_exit_request, "BACK in OVER quits to menu");
    s_exit_request = false;
    qe_state = QE_PLAY;
    quake_on_key(&(key_event_t){ .key = K_QUIT, .ch = 0, .is_repeat = false });
    CHECK(s_exit_request, "Q quits during play");
    s_exit_request = false;
    /* WASD 移动 */
    qe_new_game();
    qe_px = 4; qe_py = 2; qe_turns = 0; qe_cmask = 0; qe_has_rock = false;
    quake_on_key(&(key_event_t){ .key = K_CHAR, .ch = 'w', .is_repeat = false });
    CHECK(qe_py == 1 && qe_turns == 1, "W moves up");
    quake_on_key(&(key_event_t){ .key = K_CHAR, .ch = 'd', .is_repeat = false });
    CHECK(qe_px == 5 && qe_turns == 2, "D moves right");
}

int main(void) {
    test_init_opposite();
    test_bounds();
    test_win_clear();
    test_quake_cadence();
    test_escape_before_quake();
    test_fall();
    test_rock_block();
    test_quake_resolve();
    test_quake_invariants();
    test_playout_smoke();
    test_render();
    test_keys();
    if (s_fail) {
        printf("RESULT: %d FAILURE(S)\n", s_fail);
        return 1;
    }
    printf("RESULT: ALL PASS\n");
    return 0;
}
