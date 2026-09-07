/* 游戏描述表 — 框架主循环的唯一契约 */
#ifndef CCG_GAME_H
#define CCG_GAME_H

#include <stdbool.h>
#include <stdint.h>
#include "../platform/input.h"

typedef enum {
    G_SNAKE = 0,    G_TETRIS,
    G_2048,
    G_MINES,
    G_SOKOBAN,
    G_MEMORY,
    G_SUDOKU,
    G_REVERSI,
    G_FIFTEEN,
    G_LIGHTSOUT,
    G_WORDLE,
    G_24GAME,
    G_KLOTSKI,
    G_MAZE,
    G_PEG,
    G_CONNECT4,
    G_LIFE,
    G_MASTERMIND,
    G_HANGMAN,
    G_NONOGRAM,
    G_BULLSCOWS,
    G_DOTSBOX,
    G_BLACKJACK,
    G_PYRAMID,
    G_GOLF,
    G_KLONDIKE,
    G_FREECELL,
    G_WHACK,
    G_KAKURO,
    G_NIM,
    G_BATTLESHIP,
    G_YAHTZEE,
    G_HEX,
    G_HEXMINES,
    G_CHECKERS,
    G_ISOLA,
    G_CHESS,
    G_DARKCHESS,
    G_BINARYPUZZLE,
    G_FUTOSHIKI,
    G_SKYSCRAPERS,
    G_NUMBERLINK,
    G_ANAGRAMS,
    G_QUESTIONS,
    G_KILLERSUDOKU,
    G_NURIKABE,
    G_PENTOMINO,
    G_MATCHSTICK,
    G_SIMON,
    G_SPIDER,
    G_POKERDRAW,
    G_TOWERDEFENSE,
    G_CCHECKERS,
    G_BACKGAMMON,
    G_BOGGLE,
    G_WORDCHAIN,
    G_MATHTRAIN,
    G_PRIMERUSH,
    G_BINMORSE,
    G_TICTACDICE,
    G_SLITHERLINK,
    G_HASHI,
    G_STARBATTLE,
    G_TENTS,
    G_KENKEN,
    G_JIGSAW,
    G_LIFE2,
    G_MEMDIGITS,
    G_GEO,
    G_FLAGQUIZ,
    G_ELEMENTS,
    G_TIMESTRAIN,
    G_CLOCKQUIZ,
    G_RIDDLE,
    G_DICEQUEST,
    G_LIARSDICE,
    G_MINER,
    G_RAILS,
    G_CITYBUILDER,
    G_BEEFARM,
    G_MUSHGARDEN,
    G_QUAKE,
    G_ANTCOLONY,
    G_MAHJONGMATCH,
    G_GOMOKU,
    G_HANOI,
    GAME_COUNT
} game_id_t;

/* 菜单分类 */
typedef enum {
    CAT_LOGIC = 0,    /* 逻辑谜题 */
    CAT_BOARD,        /* 棋类博弈 */
    CAT_CARDS,        /* 纸牌 */
    CAT_WORDS,        /* 文字单词 */
    CAT_NUMBERS,      /* 数字/心算 */
    CAT_DICE,         /* 骰子/桌面 */
    CAT_QUIZ,         /* 知识问答 */
    CAT_SIM,          /* 策略/模拟 */
    CAT_ACTION,       /* 动作/反应 */
    CAT_COUNT
} game_cat_t;

extern const char *const g_cat_names[CAT_COUNT];   /* 菜单分类名(ASCII) */

typedef struct {
    game_id_t id;
    game_cat_t cat;             /* 菜单分类 */
    const char *title;          /* 菜单显示名(ASCII) */
    const char *tagline;        /* 一行副标题 */
    const char *help[6];        /* 说明页行(<=6, NULL 结尾) */
    void (*enter)(void);        /* 开局: 初始化+渲染+全刷 */
    void (*exit)(void);
    void (*tick)(uint64_t now); /* 周期逻辑; tick_interval_ms=0 则不调用 */
    void (*render)(void);       /* 状态 → g_fb; 结束调用 disp_fast */
    void (*on_key)(const key_event_t *ev);
    uint32_t tick_interval_ms;
    uint32_t repeat_init_ms, repeat_ms;   /* 0 表示用默认 */
} game_desc_t;

extern const game_desc_t g_games[GAME_COUNT];

/* 框架全局: 游戏内置此标志请求退出回主菜单(main.c 定义) */
extern bool s_exit_request;
/* 框架: 动态调整 tick 间隔(蛇加速等; main.c 定义) */
void game_set_tick_interval(uint32_t ms);

/* 暂停覆盖层(框架提供): 在游戏渲染后叠加 Resume/Restart/Quit */
typedef enum { PAUSE_RESUME, PAUSE_RESTART, PAUSE_QUIT, PAUSE_ITEMS } pause_sel_t;
/* 返回 false = 选择退出游戏 */
bool ui_pause_run(pause_sel_t *sel);

/* 通用: 顶栏 HUD(标题 + 分数), 游戏区 y 从 CCG_HUD_H 开始 */
void hud_draw(const char *title, uint32_t score);
/* 居中消息 + 反白框(游戏结束等) */
void msg_center(const char *line1, const char *line2);

#endif
