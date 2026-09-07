/* ChiChuGames 全局配置 — C1-Slim 墨水屏游戏合集 */
#ifndef CCG_CONFIG_H
#define CCG_CONFIG_H

#ifndef CCG_VERSION
#define CCG_VERSION "0.1.1"
#endif
#define CCG_NAME "CHICHU GAMES"

/* ---- 屏幕 ---- */
#define CCG_W 296U
#define CCG_H 152U
#define CCG_STRIP_H 8U
#define CCG_STRIP_COUNT 19U            /* 152/8 */
#define CCG_FRAME_BYTES 5624U          /* 296*19 */

/* ---- 显示刷新预算 ---- */
#define CCG_FULL_GAP_MIN_MS 2200U      /* 全刷硬间隔(30/min 上限留余量) */
#define CCG_AUTO_FULL_FAST_MIN 8U      /* 自动插刷: 快刷次数阈值 */
#define CCG_AUTO_FULL_GAP_MS 3000U     /* 自动插刷: 时间阈值 */
#define CCG_REFRESH_MAX_DEFAULT 30U    /* sysfs refresh_max 读取失败的兜底 */

/* ---- HUD 顶栏 ---- */
#define CCG_HUD_H 16

/* ---- 主循环 ---- */
#define CCG_IDLE_POLL_MS 250U          /* 静止期 poll 超时(省电) */
#define CCG_INPUT_POLL_MS 50U          /* 输入驱动游戏的最小轮询间隔 */

/* ---- 按键重复(默认) ---- */
#define CCG_REPEAT_INIT_MS 500U
#define CCG_REPEAT_MS 150U

#endif
