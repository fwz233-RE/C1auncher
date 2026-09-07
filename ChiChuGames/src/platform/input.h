/* 输入 — evdev 双设备(event0 矩阵 + event1 GPIO), 语义映射, 长按重复合成 */
#ifndef CCG_INPUT_H
#define CCG_INPUT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    K_NONE = 0,
    K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_OK,           /* OK(352) + ENTER(28) */
    K_BACK,         /* BACK(158) */
    K_DEL,          /* DELETE(111) */
    K_PAUSE,        /* P(25) */
    K_WAKEUP,       /* WAKEUP(143) */
    K_SPACE, K_SHIFT,
    K_VOLUP, K_VOLDOWN,
    K_CHAR,         /* 字母/数字透传, ch 字段携带 */
    K_QUIT,         /* Q(16) — 菜单退出 */
    K_COUNT
} ccg_key;

typedef struct {
    ccg_key key;
    uint8_t ch;         /* K_CHAR 时: 'a'-'z' / '1'-'9','0' */
    bool is_repeat;
} key_event_t;

int input_init(void);                       /* 打开两个 evdev, 失败返回 -1 */
void input_cleanup(void);
/* poll 双 fd, 读事件入环形队列; timeout_ms<0 无限等 */
void input_poll(int timeout_ms);
/* 出队(含合成重复), 无事件返回 false */
bool input_get(key_event_t *ev);
void input_drain(void);
/* 长按重复参数(在游戏 enter 时调用; 全部按键默认不重复) */
void input_set_repeat(uint32_t init_ms, uint32_t repeat_ms);

#endif
