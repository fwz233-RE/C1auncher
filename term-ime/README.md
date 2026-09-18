# TTY 中文输入虚拟终端 (term-ime)

在 Linux TTY 环境中运行的虚拟终端，内置多语言输入法支持。

本目录现作为 [C1auncher](https://github.com/fwz233-RE/C1auncher/tree/main/term-ime) 的普通源码文件夹维护，不再是独立 Git 仓库或子模块。`deps/` 中的依赖源码随仓库一起提供，克隆时无需 `--recursive`。原项目来源、版本与第三方许可见 [第三方说明](THIRD_PARTY_NOTICES.md)。

本次纳入的是源码，不提供新的输入法 Release 安装包；Linux 主机测试不代表 C1-Slim 的 MIPS 构建或设备验收已完成。

## 特性

- **PTY 虚拟终端**: 支持常用光标、滚动和擦除序列，隔离子程序主/备用屏幕；重绘采用 16 色模型，不是完整 xterm 实现
- **多语言输入法**: 基于 librime，支持简体中文
- **可扩展架构**: 语言配置化，避免硬编码
- **异步事件驱动**: 基于 libuv 的高性能事件循环
- **UTF-8 支持**: 完整的 UTF-8 编解码，支持 CJK 宽字符
- **FTXUI 渲染**: 函数式终端 UI 组件

## 依赖

### 运行时依赖

**无需运行时共享库** —— term-ime 生成完全静态链接的可执行文件（`ldd` 显示 "not a dynamic executable"）。中文输入仍需要随包提供的 Rime 方案、词库和 OpenCC 数据；安装时保留 `share/term-ime/rime-data`，或通过 `rime_shared_data_dir` 指定位置。

### 构建依赖

仅构建工具链，无任何第三方系统库：

- `build-essential` / `cmake` / `pkg-config` - 构建工具

yaml-cpp / leveldb / marisa / opencc 都从 `deps/librime/deps/` 内置源码静态编译，**无需安装**它们的 `-dev` 包。Boost 已彻底剥离（librime 的 boost::algorithm/signals2/interprocess/crc/uuid 改用 `<rime/*.hpp>` 极简实现，regex 改用 `std::regex`；唯一保留的 `boost/sml.hpp` 来自内置 `deps/sml` 子模块，不依赖系统 Boost）。

### 随仓库内置的依赖（均从源码编译为静态库）
- FTXUI - 终端 UI 组件
- spdlog - 日志库
- nlohmann_json - JSON 解析
- googletest - 单元测试框架
- librime - 输入法引擎（其嵌套依赖 yaml-cpp/leveldb/marisa/opencc 亦从源码静态编译）
- libuv - 异步事件循环
- sml / utf8proc - 状态机 / UTF-8 处理

## 构建

```bash
# 安装构建依赖（仅需工具链，无第三方库）
sudo apt-get install -y build-essential cmake pkg-config

# 克隆主仓库（依赖源码已内置，不需要子模块）
git clone https://github.com/fwz233-RE/C1auncher.git
cd C1auncher/term-ime

# 构建（产出完全静态链接的二进制）
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

或使用 Makefile:

```bash
make build
```

## 运行

```bash
./build/term-ime
```

**注意**: 需要在真实 TTY 或支持 alternate screen 的终端中运行。

## 使用方法

### 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+A` `Space` | 切换中英文模式 |
| `Ctrl+A` `S` | 打开/关闭设置面板 |
| `1-9` | 选择候选词 |
| `Space` | 选择第一个候选词（候选状态时） |
| `Esc` | 取消输入 |
| `,` `.` | 候选词上一组 / 下一组（屏幕外的候选按可见数量成组翻页） |
| `←` `↑` `PgUp` | 候选词上一组（同 `,`） |
| `→` `↓` `PgDn` | 候选词下一组（同 `.`） |
| `exit` | 退出 shell |

### 操作流程

单独 `Esc` 使用约 50 ms 的等待窗口，避免把分批到达的方向键误当取消。
组词时 `Enter`、`Tab`、`Home`、`End`、`Delete` 和常用 Ctrl 编辑键交给 Rime；
`Ctrl+C` 取消当前拼音并传给终端程序。中文标点的提交结果会立即输出。

带括号粘贴标记（`ESC[200~` / `ESC[201~`）的文本在中英文模式下均逐字节透传，
不会触发拼音、数字选词或 `Ctrl+A` 快捷键；开始粘贴时取消尚未提交的拼音。
设置面板忽略这类粘贴，不执行其中的按键。没有粘贴标记的输入仍按普通键盘输入处理。

1. 启动后进入英文模式，状态栏显示 `[EN]`
2. 按 `Ctrl+A` 然后按 `Space` 切换到中文模式
3. 输入拼音（如 `nihao`），显示候选词
4. 按 `1-9` 选择候选词，或按 `Space` 选择第一个
5. 输入 `exit` 退出程序

## 配置

配置文件默认位于 `~/.config/term-ime/config.json`（遵循 `XDG_CONFIG_HOME`）。
也可运行 `./build/term-ime /path/to/config.json`，设置会原路保存到该文件，采用临时文件加原子替换，避免写坏原配置。

有效配置项：
- `shell`：明确指定的可执行文件路径优先；缺失或为空时使用非空 `$SHELL`，最后回退 `/bin/bash`。启动路径无效或不可执行时显示具体原因并返回非零退出码。
- `rime_shared_data_dir` / `rime_user_data_dir`：分别指定 Rime 共享词库和用户数据目录。
- `ui_language`、`fuzzy_pinyin`、`max_candidates`：界面语言、模糊音和候选上限。
- `show_mode_indicator`：是否显示模式标签。
- `log_level`：`debug` / `info` / `warn` / `error` / `off`；`log_file` 指定追加写入的日志文件，空字符串表示不写文件日志。
- `candidate_bar_position` 目前仅支持 `bottom`，其他值会明确降级为底栏。
- 旧 `dict_path` / `extra_dicts` 已废弃；额外词库请在 Rime 方案中配置，不再将这些无效字段写回配置。

Rime 初始化或方案选择失败时，状态栏显示 `[EN!]`，继续英文直通；切换快捷键不会进入吞字的中文状态。

配置示例：

```json
{
  "languages": [
    {"id": "zh-Hans", "name": "简体中文", "schema": "luna_pinyin_simp", "enabled": true}
  ],
  "active_language": "zh-Hans",
  "log_level": "warn"
}
```

### 候选词显示

候选栏按终端宽度自适应：优先显示能完整放下的候选词；长拼音会省略显示，给候选保留空间。
单个候选过长时用省略号截断显示，选词仍提交完整文字。按 `,` / `.`（或 `<` / `>`）成组翻页，
因此窄终端下屏幕外的候选不会被跳过。

`max_candidates`（1-9，默认 `9`）是每页候选词的数量上限；调大它可以让宽终端一次显示更多。
旧配置里的 `page_size` 仍然兼容读取。这里限制的是界面可见数量，不限制 Rime 方案的 `menu/page_size`；
方案一页超过 9 个候选时，仍可翻组后用界面上的 `1-9` 选择，不依赖方案自己的选择键配置。
设置面板里也有「候选词数量」（1-9）一项，按 `↑`/`↓` 移动、`←`/`→` 或 `Enter` 改值。

### 模糊音

设置面板里的「模糊音」开关控制这组易混读音：`n/l`、`zh/z`（zh/ch/sh ↔ z/c/s）、`r/l`、`r/y`、
`hu/f`、`en/eng`（含 `in/ing`）、`an/ang`（同一条规则覆盖 `ian/iang`、`uan/uang`、`üan/üang`）。
默认**开启**（与历史行为一致），关闭后按精确拼音匹配。

例：开关打开时输入 `la` 会出现「那/拿」，`fan` 会出现「方」，`qian` 会出现「枪」；
关闭后只剩精确读音（`啦/拉`、`饭/反`、`前/钱`）。

配置文件键为 `fuzzy_pinyin`（`true`/`false`）。两份 schema 各自带一份编译好的规则，
所以开关即时生效，不需要重新部署。

## 架构

```
src/
├── core/
│   ├── app.hpp/cpp        # 应用主逻辑
│   ├── config.hpp/cpp     # 配置管理
│   └── event_loop.hpp/cpp # libuv 事件循环
├── ime/
│   ├── engine.hpp         # IME 抽象接口
│   ├── rime_engine.hpp/cpp # librime 封装
│   ├── language.hpp/cpp   # 语言管理器
│   └── kaomoji.hpp/cpp     # 颜文字
├── terminal/
│   ├── pty.hpp/cpp        # PTY 管理
│   ├── screen.hpp/cpp     # 屏幕缓冲
│   └── parser.hpp/cpp     # 转义序列解析(CSI 光标/SGR 颜色/ED/EL 擦除)
├── ui/
│   └── renderer.hpp/cpp   # FTXUI 终端渲染
└── util/
    └── utf8.hpp/cpp       # UTF-8 工具

tests/
├── test_main.cpp          # 测试入口
├── test_utf8.cpp          # UTF-8 编解码测试
├── test_config.cpp        # 配置测试
└── test_ime_state.cpp     # IME 状态测试
```

## 开发

### 代码格式化

```bash
make format
```

需要安装 `clang-format`。

### 运行测试

```bash
make build
make test
```

或使用 CTest:

```bash
cd build && ctest --output-on-failure
```

### 测试覆盖

- **UTF-8 测试**: ASCII/中文/Emoji 编解码
- **配置测试**: 默认配置、JSON 序列化
- **IME 状态测试**: 候选词结构、状态枚举

## 自动化验证

在 `term-ime/` 中运行 `make build` 和 `make test` 完成构建与测试。原独立仓库的 GitHub Actions 工作流未随此次源码导入启用；主仓库如需持续集成或输入法 Release，应另行配置面向本子目录的工作流。

## 功能列表

- [x] PTY 创建与子进程管理
- [x] 屏幕缓冲
- [x] VT100 转义序列解析
- [x] UTF-8 输入输出
- [x] librime 多语言输入法
- [x] 终端大小变化处理
- [x] libuv 异步事件循环
- [x] 可配置多语言支持
- [x] FTXUI 候选词渲染
- [x] Ctrl+A+Space 模式切换
- [x] 空格选择第一个候选词
- [x] 进入/退出时清屏
- [x] 单元测试框架
- [x] CI/CD 自动构建
- [x] SGR 16 色支持(前景/背景/加亮/反显)
- [x] ED/EL 擦除(清屏/清行)
- [ ] 更多转义序列支持(如 SGR 自查询、OSC 透传)
- [ ] 主题切换

## 许可证

本目录保留原项目的 MIT License 声明；依赖库与词库分别遵循自身许可。来源与许可文件位置见 [第三方说明](THIRD_PARTY_NOTICES.md)。