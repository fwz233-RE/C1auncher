/* 扬声器 — 常驻 aplay 播放器, 合成方波音效
 * 非阻塞: 短音效写入 pipe 缓冲即返回, 游戏循环零停顿
 * 零 malloc; 播放失败静默忽略 */
#ifndef CCG_AUDIO_H
#define CCG_AUDIO_H

#include <stdint.h>

/* 单音: freq=Hz, dur_ms=时长 */
void audio_beep(uint16_t freq_hz, uint16_t dur_ms);

/* 常用音效(全部非阻塞) */
void audio_tick(void);     /* 短促确认音(菜单选择) */
void audio_move(void);     /* 移动/光标音 */
void audio_select(void);   /* 翻开/选择音 */
void audio_clear(void);    /* 消除/配对/得分音 */
void audio_error(void);    /* 非法操作低音 */
void audio_win(void);      /* 胜利上行琶音 */
void audio_lose(void);     /* 失败下行音 */

/* 退出前关闭播放器子进程 */
void audio_cleanup(void);

/* 统计: played=已写入的播放请求, dropped=缓冲满丢弃 */
typedef struct { uint32_t played; uint32_t dropped; } audio_stats_t;
void audio_get_stats(audio_stats_t *st);

#endif
