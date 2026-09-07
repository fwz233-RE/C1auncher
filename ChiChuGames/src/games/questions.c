/* 20 QUESTIONS — AI 猜动物(简化版)
 * 30 动物 × 5 二元属性(会飞/会游泳/食肉/有毛/体型大), 决策树提问:
 *   每步选「平分度最大」的属性提问(平手随机), 玩家 Y/N 回答(OK 确认),
 *   候选唯一或 20 题预算问完即猜; 猜中 = AI 赢, 猜错 = 玩家赢(亮出答案)
 * 键位: READY 方向键选动物 + OK 开始; 提问 Y/N 选择 + OK 确认;
 *       结束 OK/N 再来一局, BACK/Q 退出; 对局中 BACK/P 暂停 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"

#define QS_N        30      /* 动物数 */
#define QS_MAX_Q    20      /* 提问预算 */
#define QS_ATTR_N   5       /* 属性数 */
#define QS_THINK_MS 600u    /* AI 思考停顿(游戏内节拍) */

/* 属性位: 会飞/会游泳/食肉/有毛/体型大 */
#define QS_FLY  0x01u
#define QS_SWIM 0x02u
#define QS_CARN 0x04u
#define QS_FUR  0x08u
#define QS_BIG  0x10u

typedef struct {
    const char *name;
    uint8_t bits;
} qs_animal_t;

/* 30 个动物, 5 位属性位图(bit0 会飞, bit1 会游泳, bit2 食肉, bit3 有毛, bit4 大)。
 * 位图尽量互异: 狐狸/猫、骆驼/马、老鹰/猫头鹰 三组同图, AI 在组内猜中一半
 * (猜错则玩家赢, 亮出所选答案, 闭环成立)。 */
static const qs_animal_t qs_animals[QS_N] = {
    {"ant",        0x00}, {"bee",        0x01}, {"goldfish",   0x02},
    {"duck",       0x03}, {"snake",      0x04}, {"eagle",      0x05},
    {"frog",       0x06}, {"seagull",    0x07}, {"rabbit",     0x08},
    {"bat",        0x09}, {"beaver",     0x0a}, {"fox",        0x0c},
    {"otter",      0x0e}, {"elephant",   0x10}, {"albatross",  0x11},
    {"whale",      0x12}, {"pterodactyl",0x13}, {"trex",       0x14},
    {"vulture",    0x15}, {"shark",      0x16}, {"pelican",    0x17},
    {"camel",      0x18}, {"pegasus",    0x19}, {"walrus",     0x1a},
    {"lion",       0x1c}, {"griffin",    0x1d}, {"bear",       0x1e},
    {"cat",        0x0c}, {"horse",      0x18}, {"owl",        0x05},
};

/* 属性提问文字 */
static const char *const qs_qtext[QS_ATTR_N] = {
    "DOES IT FLY?", "CAN IT SWIM?", "DOES IT EAT MEAT?",
    "DOES IT HAVE FUR?", "IS IT BIG?",
};

/* ---- 对局状态 ---- */
typedef enum {
    QS_ST_READY,        /* 列表选动物 */
    QS_ST_ASK,          /* AI 提问 */
    QS_ST_GUESS,        /* AI 亮出猜测 */
    QS_ST_WIN_AI,       /* AI 猜中 */
    QS_ST_WIN_PLAYER    /* AI 猜错, 玩家赢 */
} qs_state_t;
typedef enum { QS_PH_KEY, QS_PH_THINK } qs_phase_t;

static qs_state_t qs_state;
static qs_phase_t qs_phase;
static int qs_cur;               /* READY 光标 0..29 */
static int qs_pick;              /* 玩家选中的动物 */
static int qs_guess;             /* AI 的猜测 */
static int qs_ask_attr;          /* 当前提问属性 0..4 */
static uint8_t qs_asked;         /* 已问属性位图 */
static uint8_t qs_ansvec;        /* 已知答案(仅已问位有效) */
static uint8_t qs_cand[QS_N];    /* 候选动物下标 */
static int qs_cand_n;
static int qs_remain;            /* 剩余题数 */
static int qs_qnum;              /* 已回答题数 */
static bool qs_sel_yes;          /* Y/N 选择 */
static bool qs_over_full;        /* 结束全刷只做一次 */
static uint64_t qs_timer;        /* THINK 结束时刻 */
static rng_t qs_rng;
static uint64_t qs_seed_cnt;

void questions_enter(void);
void questions_render(void);

/* 十进制写入 b 并补 '\0', 返回长度 */
static int qs_putint(char *b, int v) {
    char tmp[6];
    int n = 0, k = 0;
    do { tmp[k++] = (char)('0' + v % 10); v /= 10; } while (v > 0 && k < 6);
    while (k > 0) b[n++] = tmp[--k];
    b[n] = 0;
    return n;
}

static int qs_popcnt(uint8_t v) {
    int c = 0;
    while (v) { c += (int)(v & 1u); v >>= 1; }
    return c;
}

/* 候选 = 全部 */
static void qs_init_cands(void) {
    int i;
    for (i = 0; i < QS_N; i++) qs_cand[i] = (uint8_t)i;
    qs_cand_n = QS_N;
}

/* 按已答结果过滤候选 */
static void qs_filter_cands(void) {
    int i, n = 0;
    for (i = 0; i < QS_N; i++)
        if ((qs_animals[i].bits & qs_asked) == qs_ansvec)
            qs_cand[n++] = (uint8_t)i;
    qs_cand_n = n;
}

/* 选下一问: 未问属性中「平分度」最大(候选尽可能均分), 平手随机 */
static int qs_pick_attr(void) {
    int best[QS_ATTR_N];
    int bn = 0, best_score = -1, a, i;
    for (a = 0; a < QS_ATTR_N; a++) {
        int yes = 0, no, sc;
        if (qs_asked & (1u << a)) continue;
        for (i = 0; i < qs_cand_n; i++)
            if (qs_animals[qs_cand[i]].bits & (1u << a)) yes++;
        no = qs_cand_n - yes;
        sc = yes < no ? yes : no;
        if (sc > best_score) { best_score = sc; bn = 0; }
        if (sc == best_score && bn < QS_ATTR_N) best[bn++] = a;
    }
    if (bn == 0) return 0;
    return best[rng_range(&qs_rng, (uint32_t)bn)];
}

/* AI 猜测: 唯一候选 → 它; 同向量多候选 → 随机; 答案矛盾(空候选) → 最近向量 */
static int qs_pick_guess(void) {
    int i, best = 0, best_d = QS_ATTR_N + 1;
    if (qs_cand_n > 0) {
        if (qs_cand_n == 1) return qs_cand[0];
        return qs_cand[rng_range(&qs_rng, (uint32_t)qs_cand_n)];
    }
    for (i = 0; i < QS_N; i++) {
        int d = qs_popcnt((uint8_t)(qs_animals[i].bits ^ qs_ansvec));
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* 进入猜阶段(思考一拍后亮出猜测) */
static void qs_go_guess(void) {
    qs_guess = qs_pick_guess();
    qs_sel_yes = true;
    qs_state = QS_ST_GUESS;
    qs_phase = QS_PH_THINK;
    qs_timer = now_ms() + QS_THINK_MS;
}

/* 确认当前问题的答案(Y/N 已选定) */
static void qs_commit(void) {
    uint8_t bit = (uint8_t)(1u << qs_ask_attr);
    if (qs_sel_yes) qs_ansvec |= bit;
    qs_asked |= bit;
    qs_remain--;
    qs_qnum++;
    qs_filter_cands();
    if (qs_cand_n <= 1 || qs_asked == 0x1fu) {  /* 唯一确定或属性问完 */
        qs_go_guess();
        return;
    }
    qs_ask_attr = qs_pick_attr();
    qs_phase = QS_PH_THINK;
    qs_timer = now_ms() + QS_THINK_MS;
}

/* 开始对局(READY 选好动物后) */
static void qs_begin(void) {
    qs_asked = 0;
    qs_ansvec = 0;
    qs_remain = QS_MAX_Q;
    qs_qnum = 0;
    qs_over_full = false;
    qs_init_cands();
    qs_ask_attr = qs_pick_attr();
    qs_sel_yes = true;
    qs_state = QS_ST_ASK;
    qs_phase = QS_PH_KEY;
}

/* READY 光标移动(3 列 × 10 行, 循环) */
static void qs_nav(ccg_key k) {
    int c = qs_cur % 3, r = qs_cur / 3;
    switch (k) {
    case K_UP:    r = (r + 9) % 10; break;
    case K_DOWN:  r = (r + 1) % 10; break;
    case K_LEFT:  c = (c + 2) % 3;  break;
    case K_RIGHT: c = (c + 1) % 3;  break;
    default:      return;
    }
    qs_cur = r * 3 + c;
}

/* 暂停菜单 */
static void qs_pause(void) {
    pause_sel_t sel;
    if (ui_pause_run(&sel)) {
        if (sel == PAUSE_RESTART) questions_enter();
    } else {
        s_exit_request = true;
    }
}

/* 表情脸: 黑圆盘 + 白眼 + 白嘴(smile 微笑 / 苦脸) */
static void qs_face(int cx, int cy, int r, bool smile) {
    int dx, dy;
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                fb_pixel(cx + dx, cy + dy, true);
    fb_pixel(cx - r / 2, cy - r / 3, false);   /* 眼 */
    fb_pixel(cx + r / 2, cy - r / 3, false);
    {   /* 嘴: 整数抛物线, smile 向下弯 / frown 向上弯 */
        int m = (2 * r) / 3;
        int end_y = cy + r / 3;
        int mid_y = smile ? cy + (2 * r) / 3 : cy - r / 3;
        for (dx = -m; dx <= m; dx++) {
            int t = 255 - (dx * dx * 255) / (m * m);
            fb_pixel(cx + dx, end_y + (mid_y - end_y) * t / 255, false);
        }
    }
}

void questions_enter(void) {
    qs_state = QS_ST_READY;
    qs_phase = QS_PH_KEY;
    /* 光标保留: 结束后 OK 直接可重玩同一种动物 */
    qs_pick = 0;
    qs_over_full = false;
    qs_seed_cnt++;
    rng_seed(&qs_rng, (uint64_t)now_ms() ^ ((uint64_t)qs_seed_cnt << 32) ^ 0x14D1ULL);
    questions_render();
    disp_full();
}

void questions_exit(void) {}

void questions_tick(uint64_t now) {
    if (qs_state != QS_ST_ASK && qs_state != QS_ST_GUESS) return;
    if (qs_phase != QS_PH_THINK) return;
    if (now < qs_timer) return;
    qs_phase = QS_PH_KEY;   /* 思考完, 亮出下一问/猜测 */
}

void questions_render(void) {
    fb_clear(false);

    /* THINK 节拍兜底: 无 tick 驱动时(如 tick_interval_ms=0)靠渲染推进 */
    if (qs_phase == QS_PH_THINK && now_ms() >= qs_timer)
        qs_phase = QS_PH_KEY;

    /* ---- 结束画面: HUD 两行 + 右上操作提示; 墙内大名字 + 表情 ---- */
    if (qs_state == QS_ST_WIN_AI || qs_state == QS_ST_WIN_PLAYER) {
        const char *nm = qs_animals[qs_pick].name;
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        fb_text(2, 1, qs_state == QS_ST_WIN_AI ? "AI WINS!" : "YOU WIN!", true);
        fb_text(2, 9, "IT WAS THE ", true);
        if (qs_state == QS_ST_WIN_AI) {
            fb_text(2 + text_width("IT WAS THE "), 9, qs_animals[qs_guess].name, true);
        } else {
            fb_text(2 + text_width("IT WAS THE "), 9, nm, true);
        }
        fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 1,
                "OK/N:RETRY BACK:QUIT", true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
        if (!qs_over_full) {
            qs_over_full = true;
            disp_force_full();
        }
        if (qs_state == QS_ST_WIN_AI) {
            fb_text_center(26, "I GUESSED YOUR ANIMAL", true);
            fb_text_scale2((CCG_W - text_width(nm) * 2) / 2, 58, nm, true);
            qs_face(148, 120, 13, true);
        } else {
            char lb[24];
            int n = 0;
            static const char pre[] = "MY GUESS: ";
            while (pre[n]) { lb[n] = pre[n]; n++; }
            {
                const char *g = qs_animals[qs_guess].name;
                int k = 0;
                while (g[k] && n < (int)sizeof(lb) - 2) lb[n++] = g[k++];
            }
            lb[n] = 0;
            fb_text_center(26, lb, true);
            fb_text_scale2((CCG_W - text_width(nm) * 2) / 2, 58, nm, true);
            qs_face(148, 120, 13, false);
        }
        return;
    }

    /* ---- HUD 顶栏: 黑字白底 ---- */
    if (qs_state == QS_ST_READY) {
        fb_text_scale2(2, 1, "20 QUESTIONS", true);
        fb_text_scale2(CCG_W - 2 - 6 * 12, 1, "PICK 1", true);
    } else {
        char rb[12];
        int ri = 0, n;
        static const char lab[] = "REMAIN ";
        fb_text_scale2(2, 1, "20 QUESTIONS", true);
        while (lab[ri]) { rb[ri] = lab[ri]; ri++; }
        n = qs_putint(rb + ri, qs_remain);
        fb_text_scale2(CCG_W - 2 - text_width(rb) * 2, 1, rb, true);
        (void)n;
    }
    fb_hline(0, CCG_HUD_H - 1, CCG_W, true);

    /* ---- READY: 3 列 × 10 行动物表 ---- */
    if (qs_state == QS_ST_READY) {
        int i;
        for (i = 0; i < QS_N; i++) {
            int x = 31 + (i % 3) * 78;
            int y = 24 + (i / 3) * 10;
            if (i == qs_cur) fb_text_inv(x, y, qs_animals[i].name);
            else fb_text(x, y, qs_animals[i].name, true);
        }
        fb_text_center(134, "UP/DOWN PICK  OK=START", true);
        return;
    }

    /* ---- 对局: 第 1 行 题号 + 候选数 ---- */
    {
        char l1[16];
        int n = 0;
        l1[n++] = 'Q';
        l1[n++] = ' ';
        n += qs_putint(l1 + n, qs_qnum + 1);
        fb_text(2, 18, l1, true);
        {
            char lr[12];
            int m = 0;
            static const char clab[] = "CAND ";
            while (clab[m]) { lr[m] = clab[m]; m++; }
            m += qs_putint(lr + m, qs_cand_n);
            fb_text(CCG_W - 2 - text_width(lr), 18, lr, true);
        }
    }

    /* ---- 问题框(思考期显示 THINKING) ---- */
    {
        static char gb[24];
        const char *box;
        if (qs_phase == QS_PH_THINK) {
            box = "THINKING...";
        } else if (qs_state == QS_ST_GUESS) {
            int n = 0;
            static const char pre[] = "IS IT THE ";
            while (pre[n]) { gb[n] = pre[n]; n++; }
            {
                const char *g = qs_animals[qs_guess].name;
                int k = 0;
                while (g[k] && n < (int)sizeof(gb) - 2) gb[n++] = g[k++];
            }
            gb[n] = 0;
            box = gb;
        } else {
            box = qs_qtext[qs_ask_attr];
        }
        {
            int tw = text_width(box) * 2;
            int bx = (CCG_W - tw - 16) / 2;
            fb_stroke_rect_thick(bx, 30, tw + 16, 34, 2, true);
            fb_text_scale2((CCG_W - tw) / 2, 38, box, true);
        }
    }

    /* ---- 答案选择 / 最近回答 ---- */
    if (qs_phase == QS_PH_KEY) {
        if (qs_sel_yes) {
            fb_text_inv(115, 76, "Y:YES");
            fb_text(157, 76, "N:NO", true);
        } else {
            fb_text(115, 76, "Y:YES", true);
            fb_text_inv(157, 76, "N:NO");
        }
        fb_text_center(96, qs_state == QS_ST_GUESS ? "OK=ANSWER" : "OK=CONFIRM", true);
    } else {
        fb_text_center(76, qs_sel_yes ? "YOU: YES" : "YOU: NO", true);
    }
}

void questions_on_key(const key_event_t *ev) {
    if (ev->is_repeat) {
        /* 方向键重复仅 READY 列表导航响应 */
        if (qs_state == QS_ST_READY && (ev->key == K_UP || ev->key == K_DOWN ||
                                        ev->key == K_LEFT || ev->key == K_RIGHT))
            qs_nav(ev->key);
        return;
    }
    switch (qs_state) {
    case QS_ST_READY:
        switch (ev->key) {
        case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT:
            qs_nav(ev->key);
            audio_move();
            break;
        case K_OK:
            qs_pick = qs_cur;
            qs_begin();
            break;
        case K_BACK:
        case K_PAUSE:
            qs_pause();
            break;
        case K_QUIT:
            s_exit_request = true;
            break;
        default:
            break;
        }
        break;

    case QS_ST_ASK:
    case QS_ST_GUESS:
        if (qs_phase == QS_PH_THINK) break;   /* AI 思考期忽略按键 */
        switch (ev->key) {
        case K_CHAR: {
            char ch = (char)ev->ch;
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            if (ch == 'y') qs_sel_yes = true;
            else if (ch == 'n') qs_sel_yes = false;
            break;
        }
        case K_OK:
            if (qs_state == QS_ST_ASK) {
                qs_commit();
                audio_select();
            } else {
                if (qs_sel_yes) {
                    qs_state = QS_ST_WIN_AI;
                    audio_lose();
                    led_fx_set(LED_FX_LOSE);
                } else {
                    qs_state = QS_ST_WIN_PLAYER;
                    audio_win();
                    led_fx_set(LED_FX_WIN);
                }
                qs_phase = QS_PH_KEY;
            }
            break;
        case K_BACK:
        case K_PAUSE:
            qs_pause();
            break;
        case K_QUIT:
            s_exit_request = true;
            break;
        default:
            break;
        }
        break;

    case QS_ST_WIN_AI:
    case QS_ST_WIN_PLAYER:
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            questions_enter();
        else if (ev->key == K_BACK || ev->key == K_QUIT)
            s_exit_request = true;
        break;
    }
}
