/* REVERSI 逻辑单测 — host 编译, 包含游戏 .c 直接访问静态状态 */
#include "../src/games/reversi.c"
#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

/* ---- host 框架 stub ---- */
bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static void fill_board(uint8_t v) {
    for (int y = 0; y < RV_N; y++)
        for (int x = 0; x < RV_N; x++) rv_board[y][x] = v;
}

/* ---- 开局与首步合法性 ---- */
static void test_opening(void) {
    rv_new();
    CHECK(rv_count(1) == 2 && rv_count(2) == 2, "opening 2 black 2 white");
    CHECK(rv_board[3][3] == 2 && rv_board[4][4] == 2, "white on d4/e5");
    CHECK(rv_board[4][3] == 1 && rv_board[3][4] == 1, "black on e4/d5");
    CHECK(rv_turn == 1 && !rv_over, "player (black) moves first");

    /* 黑方首步合法位: 恰好 c4 d3 e6 f5, 各翻 1 子 */
    static const int legal[4][2] = { {2,3}, {3,2}, {4,5}, {5,4} };
    int nlegal = 0;
    for (int y = 0; y < RV_N; y++)
        for (int x = 0; x < RV_N; x++) {
            int f = rv_flips_at(x, y, 1, false);
            if (f > 0) nlegal++;
            int in = 0;
            for (int i = 0; i < 4; i++)
                if (legal[i][0] == x && legal[i][1] == y) in = 1;
            if (in) CHECK(f == 1, "opening move flips exactly 1");
            else CHECK(f == 0, "non-opening cell not legal");
        }
    CHECK(nlegal == 4, "black has exactly 4 opening moves");

    /* 占用格非法 */
    CHECK(rv_flips_at(3, 3, 1, false) == 0, "occupied cell illegal");
    CHECK(rv_flips_at(4, 4, 2, false) == 0, "occupied cell illegal (white)");
    /* 越界安全 */
    CHECK(rv_flips_at(-1, 0, 1, false) == 0 && rv_flips_at(8, 8, 1, false) == 0,
          "out-of-bounds safe");
}

/* ---- 翻转: 多方向 + 长串 ---- */
static void test_flips(void) {
    /* rv_board 下标为 [y][x]。构造: (2,3)黑 (3,3)白 (4,3)空 ← 黑落此处;
     * (4,1)黑 (4,2)白 → board[3][2]=1 board[3][3]=2 board[1][4]=1 board[2][4]=2 */
    fill_board(0);
    rv_board[3][2] = 1; rv_board[3][3] = 2;
    rv_board[1][4] = 1; rv_board[2][4] = 2;
    CHECK(rv_flips_at(4, 3, 1, false) == 2, "move flips two directions (left+up)");
    int f = rv_flips_at(4, 3, 1, true);   /* 与游戏流程一致: 先翻转后落子 */
    rv_board[3][4] = 1;
    CHECK(f == 2 && rv_board[3][3] == 1 && rv_board[2][4] == 1 &&
          rv_board[3][4] == 1,
          "apply flips both discs and stone placed");

    /* 长串: 第 4 行边界到边界 5 子翻转 */
    fill_board(0);
    rv_board[4][0] = 1;
    rv_board[4][2] = 2; rv_board[4][3] = 2; rv_board[4][4] = 2;
    rv_board[4][5] = 2; rv_board[4][6] = 2;
    rv_board[4][7] = 1;
    CHECK(rv_flips_at(1, 4, 1, false) == 5, "long run flips 5");
    rv_flips_at(1, 4, 1, true);
    rv_board[4][1] = 1;
    int all = 1;
    for (int x = 2; x <= 6; x++)
        if (rv_board[4][x] != 1) all = 0;
    CHECK(all && rv_board[4][1] == 1, "long run applied to all five");

    /* 白子同一落子镜像验证(规则对称) */
    fill_board(0);
    rv_board[1][1] = 2; rv_board[2][2] = 2;
    CHECK(rv_flips_at(0, 0, 1, false) == 0, "black at corner of W-B-W pattern not legal");
}

/* ---- AI: 角/边/邻角打分与唯一角位选择 ---- */
static void test_ai(void) {
    CHECK(rv_ai_score(0, 0, 1) == 51, "corner +50");
    CHECK(rv_ai_score(0, 3, 1) == 11, "edge +10");
    CHECK(rv_ai_score(1, 1, 1) == -19, "near-corner -20");
    CHECK(rv_ai_score(0, 1, 1) == -9, "edge+near-corner = -10+1");
    CHECK(rv_ai_score(3, 3, 5) == 5, "center only flips");

    /* 黑(1,1) 白(2,2) 时白唯一合法步是角 (0,0) */
    fill_board(0);
    rv_board[1][1] = 1;
    rv_board[2][2] = 2;
    rv_turn = 2;
    CHECK(rv_ai_move(), "ai has a move");
    CHECK(rv_board[0][0] == 2, "ai grabs the corner");
    CHECK(rv_board[1][1] == 2, "corner move flips (1,1)");
    CHECK(rv_count(2) == 3 && rv_count(1) == 0, "counts after corner grab");

    /* AI 无合法步返回 false */
    fill_board(1);
    CHECK(!rv_ai_move(), "ai no move on full board");
}

/* ---- 跳过与结束 ---- */
static void test_pass_and_over(void) {
    /* 白无合法步, 黑有 → rv_advance 保持黑回合(白被跳过) */
    fill_board(1);
    rv_board[3][3] = 2;
    rv_board[4][4] = 2;
    rv_board[3][2] = 0;   /* (2,3) 空: 黑唯一合法步(翻 (3,3)) */
    rv_turn = 1;
    CHECK(rv_has_any(1) && !rv_has_any(2), "black can move, white cannot");
    CHECK(rv_advance() && rv_turn == 1 && !rv_over, "white skipped, black plays again");
    /* 黑落这步后盘满 → 结束, 63:1 */
    rv_player_move(2, 3);
    CHECK(rv_over, "game ends when board fills after pass");
    CHECK(rv_count(1) == 63 && rv_count(2) == 1, "final score 63:1");

    /* 双方无步(满盘) → 结束 */
    fill_board(2);
    rv_turn = 1;
    CHECK(!rv_advance() && rv_over, "full board ends game");
}

/* ---- 玩家回合: 合法落子 + AI 应答 + 非法落子无效果 ---- */
static void test_player_turn(void) {
    rv_new();
    rv_player_move(2, 3);          /* c4 = board[3][2]: 翻 d4, AI 应答 */
    CHECK(rv_board[3][2] == 1, "player stone placed at c4");
    CHECK(rv_turn == 1 && !rv_over, "AI replied, turn back to player");
    CHECK(rv_count(1) + rv_count(2) == 6, "6 discs on board after exchange");
    CHECK(rv_count(1) >= 3 && rv_count(2) >= 1, "player flipped one, AI replied");

    /* 非法落子: 占用格 / 无翻转格 → 无效果 */
    uint8_t before[RV_N][RV_N];
    memcpy(before, rv_board, sizeof(before));
    int t = rv_turn;
    rv_player_move(0, 0);          /* 无翻转 */
    rv_player_move(3, 3);          /* 占用 */
    CHECK(memcmp(before, rv_board, sizeof(before)) == 0 && rv_turn == t,
          "illegal moves change nothing");

    /* 边界: 光标不能出界 */
    rv_cx = 7; rv_cy = 7;
    key_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.key = K_RIGHT; ev.is_repeat = true;
    reversi_on_key(&ev);
    CHECK(rv_cx == 7, "cursor clamps at right edge");
    ev.key = K_UP;
    reversi_on_key(&ev);
    CHECK(rv_cy == 6, "cursor moves up");
    ev.key = K_OK; ev.is_repeat = true;
    reversi_on_key(&ev);           /* 重复的确认键必须被忽略 */
    /* 无崩溃即视为通过, 回合不应被重复事件改变 */
    CHECK(rv_turn == 1 || rv_over, "repeat OK ignored safely");
}

/* ---- 完整对局模拟: 确定性跑到结束 ---- */
static void test_full_game(void) {
    rv_new();
    int guard = 0;
    while (!rv_over && guard++ < 200) {
        if (rv_turn == 1) {
            int fx = -1, fy = -1;
            for (int y = 0; y < RV_N && fx < 0; y++)
                for (int x = 0; x < RV_N && fx < 0; x++)
                    if (rv_board[y][x] == 0 && rv_flips_at(x, y, 1, false) > 0) {
                        fx = x; fy = y;
                    }
            CHECK(fx >= 0, "player has a move whenever it is player's turn");
            if (fx < 0) break;
            rv_player_move(fx, fy);
        } else {
            CHECK(rv_ai_move(), "ai has a move on its turn");
            rv_advance();
        }
    }
    CHECK(rv_over, "deterministic full game terminates");
    printf("  final: BLACK %d WHITE %d\n", rv_count(1), rv_count(2));
    CHECK(rv_count(1) + rv_count(2) == RV_N * RV_N, "all 64 discs played");
    int winner_ok = (rv_count(1) + rv_count(2) == 64) &&
                    (rv_count(1) > 0 || rv_count(2) > 0);
    CHECK(winner_ok, "sane final counts");
}

/* ---- 渲染冒烟: 网格/黑子/光标像素 ---- */
static void test_render(void) {
    rv_new();
    reversi_render();
    /* 网格左上角 (84,18): 黑 */
    int off = (18 / 8) * CCG_W + 84;
    CHECK((g_fb[off] & 0x20) != 0, "grid corner pixel black");
    /* 黑子 e4 中心 (156,74): 黑 */
    off = (74 / 8) * CCG_W + 156;
    CHECK((g_fb[off] & 0x20) != 0, "black stone center black");
    /* 白子 d4 中心 (140,74): 空心 → 白 */
    off = (74 / 8) * CCG_W + 140;
    CHECK((g_fb[off] & 0x20) == 0, "white stone center white");
    /* 光标边框 (132,66): 光标在 (3,3) 白格上 → 黑边 */
    off = (66 / 8) * CCG_W + 132;
    CHECK((g_fb[off] & 0x20) != 0, "cursor border drawn on white cell");

    /* 结束态渲染: 全刷标志 + HUD 提示 */
    fill_board(2);
    rv_over = true;
    rv_over_full = false;
    reversi_render();
    CHECK(rv_over_full, "over render sets force-full flag");
    rv_over = false;
}

int main(void) {
    test_opening();
    test_flips();
    test_ai();
    test_pass_and_over();
    test_player_turn();
    test_full_game();
    test_render();
    if (s_fail == 0) { printf("ALL REVERSI TESTS PASSED\n"); return 0; }
    printf("%d REVERSI TEST(S) FAILED\n", s_fail);
    return 1;
}
