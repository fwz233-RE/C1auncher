/* 单调时钟 — SIGSTOP/SIGCONT 安全 */
#ifndef CCG_TIME_H
#define CCG_TIME_H

#include <stdint.h>

uint64_t now_ms(void);          /* clock_gettime(CLOCK_MONOTONIC) */
void sleep_until(uint64_t t_ms);/* 绝对时间点休眠 */

#endif
