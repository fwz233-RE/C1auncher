/* NIM 核心逻辑测试(手写补充) */
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
#include "../../src/games/nim.c"
static int s_fail = 0;
#define CHECK(c, m) do { if(!(c)){printf("FAIL: %s\n", m); s_fail++;} else printf("ok: %s\n", m);} while(0)
int main(void) {
    /* nim-sum 必胜步 */
    rng_seed(&nm_rng, 1);
    nm_heaps[0] = 5; nm_heaps[1] = 1; nm_heaps[2] = 1; nm_n = 3;
    int h, t;
    nm_plan_move(nm_heaps, nm_n, false, &h, &t);
    CHECK(h == 0 && t == 1, "nim {5,1,1} -> take 1 from heap 0");
    nm_heaps[0] = 1; nm_heaps[1] = 2; nm_heaps[2] = 3;
    nm_plan_move(nm_heaps, nm_n, false, &h, &t);
    CHECK(h >= 0 && t >= 1 && t <= 3 && nm_heaps[h] >= t, "nim losing position stays legal");
    /* 终局: 单堆 */
    nm_heaps[0] = 3; nm_n = 1;
    nm_plan_move(nm_heaps, nm_n, false, &h, &t);
    CHECK(h == 0 && t == 3, "nim single heap 3 -> take 3");
    if (s_fail == 0) { printf("ALL NIM TESTS PASSED\n"); return 0; }
    printf("%d FAILED\n", s_fail);
    return 1;
}
