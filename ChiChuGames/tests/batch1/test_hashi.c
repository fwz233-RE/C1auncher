/* HASHI host 逻辑单测 — 直接包含 hashi.c 访问静态状态
 * 覆盖: ROM 谜题独立验证(几何/度数和/连通)、建桥循环、交叉检测、
 *       胜负判定、按键处理、DEL、重复键、边界 */
#include "../../src/games/hashi.c"
#include <stdio.h>
#include <string.h>

bool s_exit_request = false;
void game_set_tick_interval(uint32_t ms) { (void)ms; }

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); s_fail++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

static int cell(int x, int y) { return y * HA_COLS + x; }

/* 自定义棋盘(测试用, 直接铺状态) */
static void t_custom(const uint8_t *xs, const uint8_t *ys, const uint8_t *ds, int n) {
    ha_n = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        ha_islands[i].x = xs[i]; ha_islands[i].y = ys[i]; ha_islands[i].deg = ds[i];
    }
    for (int c = 0; c < HA_CELLS; c++) { ha_chh[c] = 0; ha_cvh[c] = 0; ha_island_of[c] = -1; }
    for (int i = 0; i < HA_MAX_ISL; i++)
        for (int j = 0; j < HA_MAX_ISL; j++)
            ha_pc[i][j] = 0;
    for (int i = 0; i < n; i++)
        ha_island_of[ha_islands[i].y * HA_COLS + ha_islands[i].x] = (int8_t)i;
    ha_cur = ha_islands[0].y * HA_COLS + ha_islands[0].x;
    ha_sel = -1;
    ha_over = false;
    ha_over_full = false;
}

/* ---- 独立解数据(与游戏实现无关): (岛a, 岛b, 桥数) ---- */
static const uint8_t sol1[][3] = {
    {0,1,1},{1,2,1},{0,3,1},{1,4,1},{2,5,1},{3,4,1},
    {4,5,1},{3,6,1},{4,7,1},{5,8,1},{6,7,1},{7,8,1},
};
static const uint8_t sol2[][3] = {
    {0,1,2},{0,3,2},{1,4,1},{3,4,1},{4,5,1},{4,7,1},
    {2,5,2},{5,8,2},{6,7,1},
};
static const uint8_t sol3[][3] = {
    {0,1,1},{1,2,1},{1,4,2},{3,4,1},{4,5,1},{3,6,1},
    {6,7,1},{7,8,1},{4,7,1},{5,8,1},
};

/* 独立验证: 解几何合法 + 度数和 + 连通 */
static void test_rom_puzzles(void) {
    const uint8_t (*sol[HA_LEVELS])[3] = { sol1, sol2, sol3 };
    const int n_sol[HA_LEVELS] = { 12, 9, 10 };
    for (int lv = 0; lv < HA_LEVELS; lv++) {
        ha_start((uint8_t)lv);
        int deg[HA_MAX_ISL] = {0};
        int pair_cnt[HA_MAX_ISL][HA_MAX_ISL];
        int tot = 0;
        int hseg[16][3], vseg[16][3], nh = 0, nv = 0;
        memset(pair_cnt, 0, sizeof(pair_cnt));
        for (int e = 0; e < n_sol[lv]; e++) {
            int a = sol[lv][e][0], b = sol[lv][e][1], c = sol[lv][e][2];
            int legal = 0;
            if (a != b && a >= 0 && b >= 0 && a < (int)ha_n && b < (int)ha_n) {
                int ax = ha_islands[a].x, ay = ha_islands[a].y;
                int bx = ha_islands[b].x, by = ha_islands[b].y;
                if (ay == by && ax != bx) {
                    int x0 = ax < bx ? ax : bx, x1 = ax < bx ? bx : ax;
                    if (x1 - x0 >= 2) {
                        legal = 1;
                        for (int i = 0; i < (int)ha_n; i++)
                            if (i != a && i != b && ha_islands[i].y == ay &&
                                ha_islands[i].x > x0 && ha_islands[i].x < x1)
                                legal = 0;
                        hseg[nh][0] = ay; hseg[nh][1] = x0; hseg[nh][2] = x1; nh++;
                        for (int k = 0; k < nv; k++)
                            if (vseg[k][0] > x0 && vseg[k][0] < x1 &&
                                ay > vseg[k][1] && ay < vseg[k][2])
                                legal = 0;
                    }
                } else if (ax == bx && ay != by) {
                    int y0 = ay < by ? ay : by, y1 = ay < by ? by : ay;
                    if (y1 - y0 >= 2) {
                        legal = 1;
                        for (int i = 0; i < (int)ha_n; i++)
                            if (i != a && i != b && ha_islands[i].x == ax &&
                                ha_islands[i].y > y0 && ha_islands[i].y < y1)
                                legal = 0;
                        vseg[nv][0] = ax; vseg[nv][1] = y0; vseg[nv][2] = y1; nv++;
                        for (int k = 0; k < nh; k++)
                            if (hseg[k][1] < ax && ax < hseg[k][2] &&
                                hseg[k][0] > y0 && hseg[k][0] < y1)
                                legal = 0;
                    }
                }
            }
            CHECK(legal, "rom: 解几何合法(同线/无岛挡/不交叉)");
            int lo = a < b ? a : b, hi = a < b ? b : a;
            pair_cnt[lo][hi] += c;
            CHECK(pair_cnt[lo][hi] <= 2, "rom: 每对最多 2 座");
            deg[a] += c; deg[b] += c; tot += c;
        }
        int sumdeg = 0;
        for (int i = 0; i < (int)ha_n; i++) sumdeg += deg[i];
        CHECK(sumdeg == 2 * tot, "rom: 度数和 = 2*桥数");
        for (int i = 0; i < (int)ha_n; i++)
            CHECK(deg[i] == (int)ha_islands[i].deg, "rom: 岛度数吻合");
        /* 连通(BFS 岛图) */
        {
            uint8_t seen[HA_MAX_ISL], q[HA_MAX_ISL];
            int qh = 0, qt = 0, cnt = 1;
            memset(seen, 0, sizeof(seen));
            seen[0] = 1; q[qt++] = 0;
            while (qh < qt) {
                int i = q[qh++];
                for (int j = 0; j < (int)ha_n; j++)
                    if (!seen[j] && pair_cnt[i < j ? i : j][i < j ? j : i]) {
                        seen[j] = 1; q[qt++] = (uint8_t)j; cnt++;
                    }
            }
            CHECK(cnt == (int)ha_n, "rom: 解连通(一个网络)");
            printf("  L%d: %d 岛 %d 桥 度数和 %d 连通 %d/%d\n",
                   lv + 1, (int)ha_n, tot, sumdeg, cnt, (int)ha_n);
        }
    }
}

/* 建桥/拆桥/计数机制 */
static void test_mechanics(void) {
    ha_start(0);
    CHECK(ha_pair_count(0, 1) == 0, "mech: 初始桥数 0");
    CHECK(ha_build_pair(0, 1), "mech: 建 1 桥");
    CHECK(ha_pair_count(0, 1) == 1, "mech: 计数 1");
    CHECK(ha_build_pair(0, 1), "mech: 建 2 桥");
    CHECK(ha_pair_count(0, 1) == 2, "mech: 计数 2");
    CHECK(!ha_build_pair(0, 1), "mech: 第 3 座被拒");
    CHECK(ha_pair_count(0, 1) == 2, "mech: 计数保持 2");
    CHECK(!ha_can_build(0, 1), "mech: 满对不可再建");
    ha_remove_pair(0, 1);
    CHECK(ha_pair_count(0, 1) == 1, "mech: 拆 1 座");
    ha_remove_pair(0, 1);
    CHECK(ha_pair_count(0, 1) == 0, "mech: 拆至 0");
    ha_remove_pair(0, 1);           /* 空拆无害 */
    CHECK(ha_pair_count(0, 1) == 0, "mech: 空拆保持 0");
    /* 不对齐 / 被挡 */
    CHECK(!ha_can_build(0, 4), "mech: 不对齐不可建");
    CHECK(ha_pair_count(0, 4) == 0, "mech: 不对齐计数 0");
    CHECK(!ha_can_build(0, 2), "mech: 岛 1 挡视线(0-2)");
    CHECK(ha_build_pair(0, 1) && ha_pair_count(0, 1) == 1, "mech: 0-1 正常建");
    CHECK(ha_incident(0) == 1 && ha_incident(1) == 1, "mech: 度数 1/1");
    ha_remove_pair(0, 1);
    CHECK(ha_chh[cell(1, 0)] == 0, "mech: 拆桥覆盖清零");
    /* 相邻格无空隙: 自定义两岛相邻 */
    {
        static const uint8_t xs[2] = {0, 1};
        static const uint8_t ys[2] = {0, 0};
        static const uint8_t ds[2] = {1, 1};
        t_custom(xs, ys, ds, 2);
        CHECK(!ha_can_build(0, 1), "mech: 相邻岛不可建");
        CHECK(!ha_build_pair(0, 1), "mech: 相邻建桥被拒");
        CHECK(ha_pair_count(0, 1) == 0, "mech: 相邻计数 0");
    }
}

/* 交叉检测: A(0,2) B(4,2) C(2,0) D(2,4) */
static void test_crossing(void) {
    static const uint8_t xs[4] = {0, 4, 2, 2};
    static const uint8_t ys[4] = {2, 2, 0, 4};
    static const uint8_t ds[4] = {2, 2, 2, 2};
    t_custom(xs, ys, ds, 4);
    /* 竖桥 C(2,0)-D(2,4) 先建 */
    CHECK(ha_build_pair(2, 3), "cross: 竖桥可建");
    CHECK(ha_cvh[cell(2, 1)] == 1 && ha_cvh[cell(2, 2)] == 1 && ha_cvh[cell(2, 3)] == 1,
          "cross: 竖桥覆盖 3 个空隙格");
    CHECK(!ha_can_build(0, 1), "cross: 横桥被竖桥穿越拒绝");
    CHECK(!ha_build_pair(0, 1), "cross: 交叉建桥被拒");
    CHECK(ha_pair_count(0, 1) == 0, "cross: 横桥计数未变");
    /* 拆竖桥后横桥可建 */
    ha_remove_pair(2, 3);
    CHECK(ha_cvh[cell(2, 2)] == 0, "cross: 拆除覆盖清零");
    CHECK(ha_build_pair(0, 1), "cross: 拆后横桥可建");
    CHECK(!ha_build_pair(2, 3), "cross: 此时竖桥被横桥拒绝");
    ha_remove_pair(0, 1);
    /* 先横后竖 / 先竖后横 均拒绝交叉 */
    CHECK(ha_build_pair(2, 3), "cross: 全清后竖桥可建");
    CHECK(!ha_build_pair(0, 1), "cross: 竖桥上横桥被拒");
    ha_remove_pair(2, 3);
    /* 平行双桥合法, 覆盖计数叠加 */
    CHECK(ha_build_pair(2, 3) && ha_build_pair(2, 3), "cross: 双竖桥");
    CHECK(ha_cvh[cell(2, 2)] == 2, "cross: 双桥覆盖计数 2");
    CHECK(!ha_build_pair(2, 3), "cross: 三座被拒");
    CHECK(ha_pair_count(2, 3) == 2, "cross: 计数保持 2");
    ha_remove_pair(2, 3);
    CHECK(ha_cvh[cell(2, 2)] == 1, "cross: 拆一座覆盖剩 1");
    ha_remove_pair(2, 3);
    CHECK(ha_cvh[cell(2, 2)] == 0, "cross: 全拆覆盖清零");
}

/* 胜利判定: 按解搭桥 -> WIN; 少一座 -> 不赢; 度数满但断网 -> 不赢 */
static void test_win(void) {
    const uint8_t (*sol[HA_LEVELS])[3] = { sol1, sol2, sol3 };
    const int n_sol[HA_LEVELS] = { 12, 9, 10 };
    for (int lv = 0; lv < HA_LEVELS; lv++) {
        ha_start((uint8_t)lv);
        CHECK(!ha_over, "win: 开局未赢");
        for (int e = 0; e < n_sol[lv]; e++)
            for (int k = 0; k < sol[lv][e][2]; k++)
                CHECK(ha_build_pair(sol[lv][e][0], sol[lv][e][1]), "win: 解中每步可建");
        ha_check_win();
        CHECK(ha_net_count() == (int)ha_n, "win: 全岛入网");
        CHECK(ha_satisfied() == (int)ha_n, "win: 全岛度数满足");
        CHECK(ha_over, "win: 按解搭完判定胜利");
        /* 拆一座 -> 度数不满 -> 不赢 */
        ha_over = false;
        ha_remove_pair(sol[lv][0][0], sol[lv][0][1]);
        ha_check_win();
        CHECK(!ha_over, "win: 拆一座不再赢");
        for (int k = 0; k < sol[lv][0][2]; k++)
            ha_build_pair(sol[lv][0][0], sol[lv][0][1]);
        ha_check_win();
        CHECK(ha_over, "win: 补回又赢");
    }
    /* 度数未满不赢 */
    ha_start(0);
    CHECK(ha_build_pair(0, 1) && ha_build_pair(0, 3), "win: 星形半边");
    CHECK(ha_incident(0) == 2 && ha_incident(1) == 1 && ha_incident(3) == 1,
          "win: 半边度数");
    ha_check_win();
    CHECK(!ha_over, "win: 度数未满不赢");
    /* 度数全满但断网: 两个独立组件各自满足 -> 不赢 */
    {
        static const uint8_t xs[4] = {0, 2, 3, 5};
        static const uint8_t ys[4] = {0, 0, 3, 3};
        static const uint8_t ds[4] = {1, 1, 1, 1};
        t_custom(xs, ys, ds, 4);
        CHECK(ha_build_pair(0, 1) && ha_build_pair(2, 3), "win: 双组件各自满足");
        CHECK(ha_satisfied() == 4, "win: 度数全满");
        ha_check_win();
        CHECK(!ha_over, "win: 断网不赢");
        CHECK(ha_net_count() == 2, "win: 网络仅 2 岛");
    }
}

/* 按键处理 */
static void test_keys(void) {
    ha_start(0);
    CHECK(ha_cur == cell(0, 0), "keys: 光标起始岛0");
    key_event_t ev;
    /* 方向键 + 重复 */
    ev.key = K_RIGHT; ev.ch = 0; ev.is_repeat = false;
    hashi_on_key(&ev);
    CHECK(ha_cur == cell(1, 0), "keys: RIGHT 移动");
    ev.is_repeat = true;
    hashi_on_key(&ev);
    CHECK(ha_cur == cell(2, 0), "keys: RIGHT 重复可响应");
    /* WASD */
    ev.key = K_CHAR; ev.ch = 's'; ev.is_repeat = false;
    hashi_on_key(&ev);
    CHECK(ha_cur == cell(2, 1), "keys: s 下移");
    ev.ch = 'a'; hashi_on_key(&ev);
    CHECK(ha_cur == cell(1, 1), "keys: a 左移");
    ev.ch = 'd'; hashi_on_key(&ev);
    CHECK(ha_cur == cell(2, 1), "keys: d 右移");
    ev.ch = 'w'; hashi_on_key(&ev);
    CHECK(ha_cur == cell(2, 0), "keys: w 上移");
    /* 边界不动 */
    ev.key = K_LEFT; ev.ch = 0; hashi_on_key(&ev);
    ev.key = K_LEFT; ev.ch = 0; hashi_on_key(&ev);
    ev.key = K_LEFT; ev.ch = 0; hashi_on_key(&ev);
    CHECK(ha_cur == cell(0, 0), "keys: 左边界夹住");
    /* 确认键重复忽略 */
    ev.key = K_OK; ev.is_repeat = true;
    hashi_on_key(&ev);
    CHECK(ha_sel == -1, "keys: OK 重复忽略");
    /* 选岛 -> 建桥 1->2->0 循环 */
    ha_cur = cell(0, 0);
    ev.is_repeat = false; hashi_on_key(&ev);
    CHECK(ha_sel == 0, "keys: OK 选中岛0");
    ha_cur = cell(2, 0);
    hashi_on_key(&ev);
    CHECK(ha_pair_count(0, 1) == 1 && ha_sel == 0, "keys: 建 1 座(选中保留)");
    hashi_on_key(&ev);
    CHECK(ha_pair_count(0, 1) == 2, "keys: 建 2 座");
    hashi_on_key(&ev);
    CHECK(ha_pair_count(0, 1) == 0, "keys: 2->0 拆桥");
    /* 同格 OK 取消选中 */
    ha_cur = cell(0, 0);
    hashi_on_key(&ev);
    CHECK(ha_sel == -1, "keys: 同格 OK 取消选中");
    /* 空格 OK 取消 */
    ha_cur = cell(0, 0); hashi_on_key(&ev);
    CHECK(ha_sel == 0, "keys: 再选中");
    ha_cur = cell(5, 4);
    hashi_on_key(&ev);
    CHECK(ha_sel == -1, "keys: 空格 OK 取消选中");
    /* 视线被挡: 选 0, 点 2 号(0,0)-(4,0) 被岛1挡 -> 改选 */
    ha_cur = cell(0, 0); hashi_on_key(&ev);
    ha_cur = cell(4, 0); hashi_on_key(&ev);
    CHECK(ha_sel == 2 && ha_pair_count(0, 2) == 0, "keys: 被挡改选岛2");
    /* 非确认键重复忽略 */
    ev.key = K_CHAR; ev.ch = 'n'; ev.is_repeat = true;
    hashi_on_key(&ev);
    CHECK(ha_level == 0, "keys: 字母重复忽略");
    ev.is_repeat = false;
    /* n 循环关卡 */
    hashi_on_key(&ev);
    CHECK(ha_level == 1, "keys: n 下一关");
    hashi_on_key(&ev);
    CHECK(ha_level == 2, "keys: n 到 L3");
    hashi_on_key(&ev);
    CHECK(ha_level == 0, "keys: n 回绕 L1");
    ev.ch = 'r'; hashi_on_key(&ev);
    CHECK(ha_level == 0 && ha_sel == -1 && ha_pair_count(0, 1) == 0, "keys: r 重开");
    /* Q 退出 */
    ev.key = K_QUIT; ev.ch = 0;
    hashi_on_key(&ev);
    CHECK(s_exit_request, "keys: Q 请求退出");
    s_exit_request = false;
}

/* DEL 清除 */
static void test_del(void) {
    ha_start(0);
    key_event_t ev;
    ev.key = K_OK; ev.ch = 0; ev.is_repeat = false;
    ha_cur = cell(0, 0);
    hashi_on_key(&ev);               /* 选 0 */
    ha_cur = cell(2, 0);
    hashi_on_key(&ev);
    hashi_on_key(&ev);               /* 建 2 座 */
    CHECK(ha_pair_count(0, 1) == 2, "del: 2 座就绪");
    ev.key = K_DEL;
    hashi_on_key(&ev);               /* 选中态: 清该对 */
    CHECK(ha_pair_count(0, 1) == 0, "del: 清选中对");
    /* 无选中: 清光标岛全部桥(含 4 个方向) */
    ha_build_pair(0, 1);
    ha_build_pair(1, 2);
    ha_build_pair(1, 4);
    ha_build_pair(4, 7);
    CHECK(ha_incident(1) == 3 && ha_incident(4) == 2, "del: 桥网就绪");
    ha_sel = -1;
    ha_cur = cell(2, 0);             /* 岛1 */
    hashi_on_key(&ev);
    CHECK(ha_pair_count(0, 1) == 0 && ha_pair_count(1, 2) == 0 && ha_pair_count(1, 4) == 0,
          "del: 光标岛全部桥清空");
    CHECK(ha_pair_count(4, 7) == 1, "del: 他岛桥不受影响");
    /* 光标不在岛上: DEL 无效 */
    ha_cur = cell(5, 4);             /* 空格 */
    hashi_on_key(&ev);
    CHECK(ha_pair_count(4, 7) == 1, "del: 空格 DEL 无效");
}

/* 胜利画面按键 */
static void test_over_keys(void) {
    ha_start(0);
    for (int e = 0; e < 12; e++)
        for (int k = 0; k < sol1[e][2]; k++)
            ha_build_pair(sol1[e][0], sol1[e][1]);
    ha_check_win();
    CHECK(ha_over, "over: 已胜利");
    key_event_t ev;
    ev.key = K_OK; ev.ch = 0; ev.is_repeat = false;
    hashi_on_key(&ev);               /* 胜利 OK = 重开本关 */
    CHECK(!ha_over && ha_pair_count(0, 1) == 0 && ha_level == 0, "over: OK 重开本关");
    for (int e = 0; e < 12; e++)
        for (int k = 0; k < sol1[e][2]; k++)
            ha_build_pair(sol1[e][0], sol1[e][1]);
    ha_check_win();
    ev.key = K_CHAR; ev.ch = 'n';
    hashi_on_key(&ev);               /* 胜利 N = 重开(非下一关) */
    CHECK(!ha_over && ha_level == 0, "over: N 重开本关");
    for (int e = 0; e < 12; e++)
        for (int k = 0; k < sol1[e][2]; k++)
            ha_build_pair(sol1[e][0], sol1[e][1]);
    ha_check_win();
    ev.key = K_BACK;
    hashi_on_key(&ev);               /* 胜利 BACK = 退出 */
    CHECK(s_exit_request, "over: BACK 退出");
    s_exit_request = false;
}

int main(void) {
    printf("== HASHI ROM 谜题独立验证 ==\n");
    test_rom_puzzles();
    printf("== 建桥机制 ==\n");
    test_mechanics();
    printf("== 交叉检测 ==\n");
    test_crossing();
    printf("== 胜利判定 ==\n");
    test_win();
    printf("== 按键处理 ==\n");
    test_keys();
    printf("== DEL ==\n");
    test_del();
    printf("== 胜利画面 ==\n");
    test_over_keys();
    if (s_fail) { printf("\n%d CHECKS FAILED\n", s_fail); return 1; }
    printf("\nALL HASHI TESTS PASSED\n");
    return 0;
}
