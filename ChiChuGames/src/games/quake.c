/* QUAKE ESCAPE — 地震逃生(回合制网格)
 *
 * 10x5 格 27px 方形(270x135 居中; HUD 16px 之下只有 136px, 28px 格放不下
 * 故取 27px 保证"正方形格子优先 + 整块游戏区最大化"的 UI 偏好)
 *
 * 玩法:
 *  - 出口(8 角星)随机生成, 玩家(小人)从对侧列出发
 *  - 每 N 回合一次地震: 随机一行/一列出现裂缝(斜纹, 至少留 1 个空位可通行,
 *    出口格永不裂缝); 同时随机 1 格落石(实心 + ! 警告, 不在裂缝线与出口/起点上)
 *  - 裂缝格不可站: 踏入=掉入 → 扣 1 生命回到起点
 *  - 落石格不可站(障碍); 落石砸中玩家同样扣 1 生命回到起点
 *  - 到达出口过关; 3 关后胜利; 生命 3
 *  - 关卡难度递增: 地震间隔 4/3/2 回合, 裂缝数 3/4/5
 *  - 地震效果持续到下次地震被替换
 *
 * 显示: 裂缝=斜纹格(PAT_SLASH_S), 落石区=实心黑 + 反白 ! 警告
 *       HUD 顶栏: 左 QUAKE, 右 LV n HP n
 *
 * 集成(games_table.c 建议):
 *   .title = "QUAKE ESCAPE", .tagline = "OUTRUN THE QUAKE",
 *   .help = { "QUAKE ESCAPE", "ARROWS: MOVE  1/GRID TURN",
 *             "REACH THE STAR - 3 LEVELS",
 *             "CRACKED CELLS + ROCKFALL COST 1 HP",
 *             "P: PAUSE  N: NEW  Q: QUIT", NULL },
 *   .tick_interval_ms = 0 (纯回合制, 输入驱动, 无需 tick)
 *
 * 静态前缀 qe_; 零 malloc; 像素坐标一律 int; ASCII 文本
 */
#include "game.h"
#include "../config.h"
#include "../gfx/canvas.h"
#include "../gfx/font.h"
#include "../gfx/pattern.h"
#include "../platform/display.h"
#include "../platform/input.h"
#include "../platform/audio.h"
#include "../platform/led.h"
#include "../platform/time.h"
#include "../rng.h"

#define QE_COLS 10
#define QE_ROWS 5
#define QE_CELL 27                       /* 27px 格 → 270x135 */
#define QE_W (QE_COLS * QE_CELL)
#define QE_H (QE_ROWS * QE_CELL)
#define QE_OX ((int)((CCG_W - QE_W) / 2))/* 水平居中 */
#define QE_OY ((int)CCG_HUD_H)           /* 板区 16..150 */
#define QE_LEVELS 3
#define QE_HP_MAX 3
#define QE_CENTER (QE_CELL / 2)          /* 27/2=13 格内中心 */

typedef enum { QE_PLAY, QE_CLEAR, QE_WIN, QE_OVER } qe_state_t;

/* 关卡难度: 地震间隔回合数 / 每条裂缝线的裂缝数 */
static const int qe_period[QE_LEVELS] = { 4, 3, 2 };
static const int qe_crack_n[QE_LEVELS] = { 3, 4, 5 };

void quake_render(void);

static rng_t qe_rng;
static uint32_t qe_gens;       /* 新局计数(种子混合) */
static int qe_level;           /* 1..3 */
static int qe_hp;              /* 生命 */
static int qe_px, qe_py;       /* 玩家(小人) */
static int qe_sx, qe_sy;       /* 起点(出口对侧列) */
static int qe_ex, qe_ey;       /* 出口(星) */
static int qe_turns;           /* 距下次地震的回合数 */
static int qe_cdir;            /* 裂缝线方向: 0=行 1=列 */
static int qe_cidx;            /* 裂缝行号 0..4 / 列号 0..9 */
static uint16_t qe_cmask;      /* 线上裂缝位置位(行→列位, 列→行位) */
static int qe_rx, qe_ry;       /* 落石 */
static bool qe_has_rock;
static qe_state_t qe_state;
static bool qe_full_once;      /* 状态切换全刷只做一次 */

/* ---- 查询 ---- */
static bool qe_cracked(int x, int y) {
    if (qe_cdir == 0) return y == qe_cidx && (qe_cmask & (1u << x)) != 0;
    return x == qe_cidx && (qe_cmask & (1u << y)) != 0;
}

/* ---- 结算: 落石/裂缝命中玩家 → 扣 1 血回起点(0 血 → OVER) ---- */
static void qe_hit(void) {
    qe_hp--;
    if (qe_hp <= 0) {
        qe_hp = 0;
        qe_state = QE_OVER;
        qe_full_once = false;
        audio_lose();
        led_fx_set(LED_FX_LOSE);
    } else {
        qe_px = qe_sx;
        qe_py = qe_sy;
        audio_error();
    }
}

/* ---- 随机裂缝线: 随机一行/列, 避开出口, 至少留 1 空位保通行 ---- */
static void qe_pick_cracks(void) {
    qe_cdir = (int)rng_range(&qe_rng, 2);
    qe_cidx = (int)rng_range(&qe_rng, (uint32_t)((qe_cdir == 0) ? QE_ROWS : QE_COLS));
    int len = (qe_cdir == 0) ? QE_COLS : QE_ROWS;
    int n = qe_crack_n[qe_level - 1];
    if (n > len - 1) n = len - 1;        /* 至少留 1 空位 */
    qe_cmask = 0;
    int placed = 0;
    int guard = 0;
    while (placed < n && guard < 64) {   /* do-while 等价; 带 guard */
        guard++;
        int p = (int)rng_range(&qe_rng, (uint32_t)len);
        int lx = (qe_cdir == 0) ? p : qe_cidx;
        int ly = (qe_cdir == 0) ? qe_cidx : p;
        if (lx == qe_ex && ly == qe_ey) continue;   /* 出口永不裂缝 */
        if (qe_cmask & (1u << p)) continue;
        qe_cmask |= (uint16_t)(1u << p);
        placed++;
    }
}

/* ---- 随机落石: 避开裂缝线/出口/起点 ---- */
static void qe_pick_rock(void) {
    int guard = 0;
    do {
        guard++;
        qe_rx = (int)rng_range(&qe_rng, QE_COLS);
        qe_ry = (int)rng_range(&qe_rng, QE_ROWS);
    } while (guard < 40 &&
             (qe_cracked(qe_rx, qe_ry) ||
              (qe_rx == qe_ex && qe_ry == qe_ey) ||
              (qe_rx == qe_sx && qe_ry == qe_sy)));
    if (guard >= 40) {                   /* 兜底: 顺序找首个合法格 */
        qe_rx = -1;
        qe_ry = -1;
        for (int y = 0; y < QE_ROWS && qe_rx < 0; y++)
            for (int x = 0; x < QE_COLS; x++) {
                if (x == qe_ex && y == qe_ey) continue;
                if (x == qe_sx && y == qe_sy) continue;
                if (qe_cracked(x, y)) continue;
                qe_rx = x;
                qe_ry = y;
                break;
            }
    }
    qe_has_rock = true;
}

/* ---- 地震结算(可独立测试): 落石砸中优先, 其次脚下裂缝 ---- */
static void qe_quake_resolve(void) {
    if (qe_rx == qe_px && qe_ry == qe_py) { qe_hit(); return; }
    if (qe_cracked(qe_px, qe_py)) qe_hit();
}

/* ---- 地震: 换新裂缝线 + 新落石, 结算命中 ---- */
static void qe_quake(void) {
    qe_pick_cracks();
    qe_pick_rock();
    qe_quake_resolve();
}

/* ---- 移动 1 格: 出界/落石被挡不计回合; 裂缝=掉入; 出口=过关 ---- */
static void qe_move(int dx, int dy) {
    if (qe_state != QE_PLAY) return;
    int nx = qe_px + dx;
    int ny = qe_py + dy;
    if (nx < 0 || nx >= QE_COLS || ny < 0 || ny >= QE_ROWS) return;
    if (qe_has_rock && nx == qe_rx && ny == qe_ry) return;   /* 落石堆=障碍 */
    qe_turns++;
    if (qe_cracked(nx, ny)) { qe_hit(); return; }            /* 掉入 */
    qe_px = nx;
    qe_py = ny;
    if (nx == qe_ex && ny == qe_ey) {                        /* 过关 */
        qe_full_once = false;
        if (qe_level >= QE_LEVELS) {
            qe_state = QE_WIN;
            audio_win();
            led_fx_set(LED_FX_WIN);
        } else {
            qe_state = QE_CLEAR;
            audio_clear();
        }
        return;
    }
    if (qe_turns >= qe_period[qe_level - 1]) {               /* 地震 */
        qe_turns = 0;
        qe_quake();
    }
}

/* ---- 新关卡: 出口随机, 起点在出口对侧列 ---- */
static void qe_new_level(int lvl) {
    qe_level = lvl;
    qe_ex = (int)rng_range(&qe_rng, QE_COLS);
    qe_ey = (int)rng_range(&qe_rng, QE_ROWS);
    qe_sx = (qe_ex < QE_COLS / 2) ? QE_COLS - 1 : 0;   /* 出口另一侧 */
    qe_sy = (int)rng_range(&qe_rng, QE_ROWS);
    qe_px = qe_sx;
    qe_py = qe_sy;
    qe_turns = 0;
    qe_cdir = 0;
    qe_cidx = 0;
    qe_cmask = 0;
    qe_has_rock = false;
    qe_state = QE_PLAY;
    qe_full_once = false;
}

static void qe_new_game(void) {
    qe_gens++;
    rng_seed(&qe_rng, now_ms() ^ ((uint64_t)qe_gens * 0x9E3779B1u));
    qe_hp = QE_HP_MAX;
    qe_new_level(1);
}

void quake_enter(void) {
    qe_new_game();
    quake_render();
    disp_full();
}

/* ---- 绘制: 8 角星(出口), 以 (x,y) 为中心 ~17px ---- */
static void qe_draw_star(int x, int y) {
    fb_fill_rect(x - 2, y - 8, 5, 17, true);   /* 竖条 */
    fb_fill_rect(x - 8, y - 2, 17, 5, true);   /* 横条 */
    for (int i = -6; i <= 6; i++) {            /* 双对角 */
        fb_fill_rect(x + i, y + i, 2, 2, true);
        fb_fill_rect(x - i, y + i, 2, 2, true);
    }
}

/* ---- 绘制: 小人 13x16 ---- */
static void qe_draw_person(int cx, int cy) {
    fb_fill_rect(cx + 3, cy, 7, 5, true);      /* 头 */
    fb_fill_rect(cx + 4, cy + 5, 6, 6, true);  /* 身 */
    fb_fill_rect(cx, cy + 7, 13, 2, true);     /* 臂 */
    fb_fill_rect(cx + 3, cy + 11, 2, 5, true); /* 腿 */
    fb_fill_rect(cx + 8, cy + 11, 2, 5, true);
}

/* ---- 绘制: 起点三角(向右) 9x9 ---- */
static void qe_draw_start(int cx, int cy) {
    for (int i = 0; i <= 8; i++) {
        int h = (8 - i) / 2;
        fb_fill_rect(cx + i, cy - h, 1, h * 2 + 1, true);
    }
}

void quake_render(void) {
    fb_clear(false);
    /* 网格线 */
    for (int x = 0; x <= QE_COLS; x++)
        fb_vline(QE_OX + x * QE_CELL, QE_OY, QE_H, true);
    for (int y = 0; y <= QE_ROWS; y++)
        fb_hline(QE_OX, QE_OY + y * QE_CELL, QE_W, true);
    /* 裂缝: 斜纹格 */
    if (qe_cmask) {
        int len = (qe_cdir == 0) ? QE_COLS : QE_ROWS;
        for (int p = 0; p < len; p++) {
            if (!(qe_cmask & (1u << p))) continue;
            int cx = (qe_cdir == 0) ? p : qe_cidx;
            int cy = (qe_cdir == 0) ? qe_cidx : p;
            fb_fill_tile(QE_OX + cx * QE_CELL + 1, QE_OY + cy * QE_CELL + 1,
                         QE_CELL - 2, QE_CELL - 2, pat_get(PAT_SLASH_S));
        }
    }
    /* 落石: 实心 + 反白 ! 警告 */
    if (qe_has_rock) {
        int rx = QE_OX + qe_rx * QE_CELL;
        int ry = QE_OY + qe_ry * QE_CELL;
        fb_fill_rect(rx + 1, ry + 1, QE_CELL - 2, QE_CELL - 2, true);
        fb_text_inv(rx + (QE_CELL - FONT_W) / 2, ry + (QE_CELL - FONT_H) / 2, "!");
    }
    /* 起点标记 */
    qe_draw_start(QE_OX + qe_sx * QE_CELL + QE_CENTER,
                  QE_OY + qe_sy * QE_CELL + QE_CENTER);
    /* 出口星 */
    qe_draw_star(QE_OX + qe_ex * QE_CELL + QE_CENTER,
                 QE_OY + qe_ey * QE_CELL + QE_CENTER);
    /* 玩家小人(最后画, 最上层) */
    qe_draw_person(QE_OX + qe_px * QE_CELL + 7, QE_OY + qe_py * QE_CELL + 5);

    /* HUD 顶栏: 黑字白底 */
    if (qe_state != QE_PLAY) {
        fb_fill_rect(0, 0, CCG_W, CCG_HUD_H - 1, false);
        if (qe_state == QE_CLEAR) {
            char lv[16];
            unsigned n = 0;
            const char *p = "LEVEL ";
            while (*p && n < 15) lv[n++] = *p++;
            lv[n++] = (char)('0' + qe_level);
            p = " CLEAR!";
            while (*p && n < 15) lv[n++] = *p++;
            lv[n] = 0;
            fb_text(2, 2, lv, true);
            fb_text(CCG_W - 2 - text_width("OK:NEXT BACK:QUIT"), 2,
                    "OK:NEXT BACK:QUIT", true);
        } else if (qe_state == QE_WIN) {
            fb_text(2, 2, "YOU ESCAPED!", true);
            fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                    "OK/N:RETRY BACK:QUIT", true);
        } else {
            fb_text(2, 2, "HP 0 - QUAKE!", true);
            fb_text(CCG_W - 2 - text_width("OK/N:RETRY BACK:QUIT"), 2,
                    "OK/N:RETRY BACK:QUIT", true);
        }
        if (!qe_full_once) { qe_full_once = true; disp_force_full(); }
    } else {
        fb_text(0, 0, "QUAKE", true);
        char buf[12];
        unsigned n = 0;
        const char *p = "LV ";
        while (*p && n < 11) buf[n++] = *p++;
        buf[n++] = (char)('0' + qe_level);
        p = " HP ";
        while (*p && n < 11) buf[n++] = *p++;
        buf[n++] = (char)('0' + qe_hp);
        buf[n] = 0;
        fb_text(CCG_W - 4 - text_width(buf), 0, buf, true);
        fb_hline(0, CCG_HUD_H - 1, CCG_W, true);
    }
}

void quake_on_key(const key_event_t *ev) {
    bool dir = ev->key == K_UP || ev->key == K_DOWN ||
               ev->key == K_LEFT || ev->key == K_RIGHT;
    if (ev->is_repeat && !dir) return;   /* 确认键/字母忽略重复, 方向可重复 */
    switch (qe_state) {
    case QE_PLAY:
        switch (ev->key) {
        case K_UP: qe_move(0, -1); break;
        case K_DOWN: qe_move(0, 1); break;
        case K_LEFT: qe_move(-1, 0); break;
        case K_RIGHT: qe_move(1, 0); break;
        case K_CHAR:
            switch (ev->ch) {
            case 'w': qe_move(0, -1); break;
            case 's': qe_move(0, 1); break;
            case 'a': qe_move(-1, 0); break;
            case 'd': qe_move(1, 0); break;
            case 'n': qe_new_game(); break;
            default: break;
            }
            break;
        case K_BACK:
        case K_PAUSE: {
            pause_sel_t sel;
            if (ui_pause_run(&sel)) {
                if (sel == PAUSE_RESTART) qe_new_game();
            } else {
                s_exit_request = true;
            }
            break;
        }
        case K_QUIT: s_exit_request = true; break;
        default: break;
        }
        break;
    case QE_CLEAR:
        if (ev->key == K_OK) qe_new_level(qe_level + 1);
        else if (ev->key == K_BACK || ev->key == K_QUIT) s_exit_request = true;
        break;
    case QE_WIN:
    case QE_OVER:
        if (ev->key == K_OK || (ev->key == K_CHAR && ev->ch == 'n'))
            qe_new_game();
        else if (ev->key == K_BACK || ev->key == K_QUIT) s_exit_request = true;
        break;
    }
}

void quake_tick(uint64_t now) { (void)now; }
void quake_exit(void) {}
