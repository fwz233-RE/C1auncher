# 游戏开发规格 — 批量开发必读（agent 规格）

所有游戏必须遵守以下规格，否则集成时会编译失败或违反平台约束。

## 平台约束（硬性）
- 屏幕 296×152 1bit；帧缓冲 `g_fb[5624]`（display.c 提供），黑=1
- 像素坐标一律 `int`（禁止 uint8_t 存像素坐标——曾经引发头身分离 bug）
- 零 malloc：静态分配；栈上小数组可以
- 编译参数（Makefile 已有，勿改）：`-Os -static -Wl,--gc-sections`
- 全刷（disp_full）只用于场景切换；游戏内移动一律 disp_fast
- 长按重复由 input 层合成，游戏勿自实现

## 框架 API（src/games/game.h）
```c
typedef struct {
    game_id_t id;
    const char *title;      // 菜单名(ASCII)
    const char *tagline;    // 副标题
    const char *help[6];    // 说明页行(<=5 行 + NULL)
    void (*enter)(void);    // 初始化+render+disp_full
    void (*exit)(void);
    void (*tick)(uint64_t now);  // tick_interval_ms=0 则不调用
    void (*render)(void);   // 全帧重绘(fb_clear 开头)+结束 disp_fast
    void (*on_key)(const key_event_t *ev);
    uint32_t tick_interval_ms;
    uint32_t repeat_init_ms, repeat_ms;
} game_desc_t;
extern const game_desc_t g_games[GAME_COUNT];
```
- 静态变量前缀：游戏名缩写 + `_`（如数独 `sd_`，黑白棋 `rv_`）防测试包含冲突
- 文件名：`src/games/<name>.c`；导出函数 `<name>_enter/_exit/_tick/_render/_on_key`
- render 必须以 `fb_clear(false)` 开头
- 游戏结束：HUD 区显示（`fb_fill_rect(0,0,CCG_W,CCG_HUD_H-1,false)` 后左上状态 + 右上 `OK/N:RETRY BACK:QUIT`），并 `disp_force_full()` 一次（用 `s_xxx_over_full` 标志防重复）
- 暂停：`K_BACK` → `ui_pause_run(&sel)`；`K_PAUSE` 直接暂停显示

## 键位（统一）
- 方向/WASD 移动光标；OK/ENTER 确认；BACK 暂停菜单；P 暂停；N 新局；Q 退出
- `key_event_t { ccg_key key; uint8_t ch; bool is_repeat; }`；K_CHAR 时 ch 为 'a'-'z'
- 游戏逻辑必须忽略 `ev.is_repeat` 的确认键/字母（方向键重复可响应）

## UI 偏好（用户明确）
1. **正方形格子优先**；整块游戏区域最大化利用屏幕
2. HUD 顶栏：左标题黑字，右标签（MOVES/SCORE 等）+ 数值；**黑字白底**（禁用反白标题）
3. 游戏结束提示在 HUD 区两行：左上结果、右上 `OK/N:RETRY BACK:QUIT`；**墙内不放任何提示文字**
4. 光标：反白格或反色边框（黑格白边/白格黑边，四周对称——必须在所有格子绘制完之后画光标）
5. 侧栏（如有）：小字标签 + 2× 反白大字数值（标题与值区分）；内容均衡
6. 数字/符号要够大清晰（如 3× 放大图标）

## 测试要求
- 每款游戏在 `tests/test_logic.c` 加 `test_<name>` 函数（静态变量直接访问），断言关键逻辑：
  状态转换、胜负判定、边界条件；`main()` 中注册调用
- host 测试编译含全部游戏文件（static 变量冲突 → 前缀必须唯一）

## 内存纪律（50M 约束）
- 全静态/栈分配，零 malloc，零动态
- 大型表（题库/关卡）用 `static const` 放 .rodata
- 单游戏静态数据 < 2KB 目标
