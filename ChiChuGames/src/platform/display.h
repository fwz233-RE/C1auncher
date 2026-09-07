/* 电子纸显示 — 快刷/全刷/帧去重/全刷预算/统计
 * 帧缓冲: extern uint8_t g_fb[5624], gfx 直接写入 */
#ifndef CCG_DISPLAY_H
#define CCG_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include "../config.h"

extern uint8_t g_fb[CCG_FRAME_BYTES];

int disp_init(void);            /* 打开 /dev/epaper_lcd + refresh 节点, 失败返回 -1 */
void disp_cleanup(void);

/* 快刷: memcmp 相同则跳过; 返回是否实际写屏 */
bool disp_fast(void);
/* 全刷: 受预算节流, 超限置 pending 延后; 阻塞 ~1.5s */
void disp_full(void);
/* 无视预算(启动/退出/SIGCONT) */
void disp_force_full(void);
/* 清屏(黑底) + 尽力全刷 */
void disp_blank(void);
/* 主循环空闲点调用: 执行延后的全刷 */
void disp_drain_pending(void);
/* 防残影插刷检查: 快刷次数>=阈值 且 距上次全刷>=阈值 → 全刷
 * 本面板快刷经实测无残影, 默认关闭; 若某场景出现残影可启用 */
void disp_maybe_auto_full(void);
void disp_set_auto_full(bool on);
/* 挂起/唤醒: 唤醒后必须调用(清缓存 + 全刷) */
void disp_suspend(void);
void disp_resume(void);

bool disp_refresh_available(void);
bool disp_full_pending(void);
/* 写帧耗时统计(us): 面板忙等导致 write 阻塞, 是跳帧/延迟的根源 */
uint32_t disp_last_write_us(void);
uint32_t disp_max_write_us(void);
uint32_t disp_avg_write_us(void);

typedef struct {
    uint32_t fast_writes;       /* 实际快刷写屏次数 */
    uint32_t full_refreshes;    /* 全刷次数 */
    uint32_t skips;             /* 帧去重跳过次数 */
    uint32_t refresh_errors;    /* refresh 写失败次数(降级快刷) */
    uint32_t last_full_age_ms;  /* 距上次全刷 */
    uint32_t fast_since_full;   /* 距上次全刷的快刷次数 */
} disp_stats_t;
void disp_get_stats(disp_stats_t *out);

#endif
