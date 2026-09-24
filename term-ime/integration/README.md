# 公共 Rime 服务与 C17 SDK

`c1-ime-service` 在独立进程中复用一个 `RimeIme`，供 C 桌面、终端和第三方客户端访问。服务不读 evdev、不操作屏幕、不自动 daemonize；只有一个事件循环串行调用 Rime。初始化和词典部署阻塞的是服务进程，客户端 `connect/send/receive` 均不等待部署。

约 50 MiB 内存预算的设备应使用 [MIPS 离线运行载荷准备器](runtime-payload.md)：`prepare-runtime.py` 在 QEMU 24Kf 上运行真正的 MIPS 服务生成词典，再输出 `payload/bin/c1-ime-service` 和目标预编译数据。设备启动必须传 `--prebuilt-only`，缺少资源时失败而不重编词典；打包入口为 `bin/c1-ime-service`。默认不带该参数的原有部署行为保留。许可全文和未完成对应源码义务随载荷保留；准备成功不表示法律审核或设备验收通过。

## 构建与测试

`term-ime/CMakeLists.txt` 已增加默认 ON 的 `TERM_IME_BUILD_INTEGRATION`，在文件末尾挂接 `add_subdirectory(integration)`。原终端构建可通过 `-DTERM_IME_BUILD_INTEGRATION=OFF` 排除该目录。

从 term-ime 目录执行：

```sh
cmake -S . -B build -DTERM_IME_BUILD_INTEGRATION=ON
cmake --build build --target c1-ime-service c1-ime-sdk-test c1-ime-client-demo -j2
ctest --test-dir build -R '^c1-ime-' --output-on-failure
```

也可独立使用已构建的**主机**静态库（不能把 x86_64 库用作 MIPS 库）：

```sh
cmake -S integration -B build-integration \
  -DC1_IME_EXISTING_BUILD=/absolute/path/to/term-ime/build
cmake --build build-integration -j2
ctest --test-dir build-integration --output-on-failure
```

`C1_IME_STATIC_SERVICE` 默认 ON；服务静态链接 C++/Rime 运行库。`c1-ime-client` 是纯 C 静态库，不依赖 Rime、C++、Xlib；可直接把 `c1_ime_client.c` 加入其他 C 项目的编译，保留同目录的两个头文件即可。`C1_IME_BUILD_EXAMPLES`、`C1_IME_BUILD_TESTS` 均可单独关闭。

测试仅创建临时 socket/user-data，启动前台子进程，结束后清理。真实服务测试验证“nihao → 你好”、五候选、选择和翻页、空格/回车提交、退格/Escape/取消、中文转英文取消、中文开启时密码绕过及用户词典文件不变、断连清空和初始英文状态、独占连接、第二实例不覆盖 socket、非法版本/操作/序号/修饰符和超过 8192 字节的消息。C SDK 测试还用模拟对端检查错误响应、非法 UTF-8、过多候选、原键和修饰符恢复、FD close-on-exec、非阻塞、在途请求限制和断连不重放。C 测试在 Release/NDEBUG 构建中也执行全部断言。

## 启动与运行时接入

服务命令：

```sh
./build/integration/c1-ime-service \
  --socket /absolute/private-runtime/socket \
  --shared-data /absolute/path/to/term-ime/data/rime-data \
  --user-data /absolute/private-user-data
```

默认 socket 是 `/run/c1-ime/socket`，其父目录必须属于运行服务的用户且权限恰为 0700；socket 权限为 0600。普通用户通常不能创建 `/run/c1-ime`，调用方可使用自己拥有的运行时目录。已有目录权限不正确会直接失败，不擅自 chmod 别人的目录。shared-data 为可信的随程序分发词典；user-data 必须是此服务专用的 0700 目录，且不要与原 term-ime 用户目录共用。

服务会在专用 user-data 写入受控 `default.custom.yaml`，强制每页五候选并关闭方案切换快捷键；已有不同内容的同名文件将导致启动失败，避免覆盖用户设置。user-data 锁阻止不同 socket 的服务同时使用同一词典。服务绝不在 bind 前删除现有 socket，双实例会失败。正常退出只删除自己创建且 inode/device 未改变的 socket；SIGKILL 留下的旧 socket 需确认无活跃服务后人工处理。

桌面应在另一个进程启动服务（或由外部进程管理器启动），不要在 UI 主线程等待子进程退出/词典部署。公开接口是 `c1_ime_client.h`：

1. 用 `C1_IME_CLIENT_INIT` 初始化可栈分配状态，调用 `c1_ime_connect`。成功只代表建立传输，发送 STATUS 并等 `C1_IME_READY` 才代表初始化完成。
2. 将 `c1_ime_client_fd` 注册到已有事件循环。`send` 返回 1 表示已发送，0 表示 EAGAIN（可等 POLLOUT 后重试该尚未发送的请求），-1 检查 errno。每连接只允许一个在途请求，EBUSY 不是传输断连。
3. 可读时调用 `receive`；只有返回 1 时才使用响应。状态/模式/用途响应不能被当作按键转发。
4. 收到 KEY 响应时，先交付非空 `commit`，再根据 `C1_IME_CONSUMED` 决定是否转发 `response.request.keysym/modifiers`。两者可以同时存在，例如 Rime 提交已有组合但不消费后续键。保留自己的原始终端字节与请求对应关系，keysym 不是终端转义序列。
5. 焦点丢失立即 close，并清空调用方预编辑 UI。重连从英文、普通用途和空组合开始。服务只允许一个焦点客户端；其他客户端会被关闭，SDK 不承诺独立多会话。
6. 断连、协议错误或等待超时后，已经 send 成功但没有响应的键可能已被提交。关闭连接、丢弃组合，**不得重放/再转发这些键**；将来的新键由调用方提供 ASCII 英文降级。服务不会自动重连或重放。
7. 密码字段建议完全断开并绕过本服务。若使用 PURPOSE_PASSWORD，先等确认再发密码键；即使逻辑中文标志开启，也不把密码键送入 Rime。切换用途会取消已有组合，选择/翻页在密码模式无作用。

`examples/client_demo.c` 是可编译运行的纯 C 示例，演示 STATUS、启用中文、nihao、提交和未消费键处理。其有限次数的同步 poll 仅用于命令行演示，**不要把示例的等待循环搬到桌面 UI 线程**。执行 `build/integration/c1-ime-client-demo /absolute/private-runtime/socket` 即可访问已经运行的测试服务。

进程隔离和权限检查是同一 Unix 用户的本地服务边界，不是针对同 UID 恶意进程的沙箱。同用户普通输入的 Rime 学习词库是有意共享的；跨焦点清理的是未提交组合、待取提交、模式和方案处理器状态，不承诺删除正常输入历史。保证 socket 上级路径和词典目录可信，避免由其他用户控制其祖先路径。

## 桌面与包管理器的有序候选交互

桌面终端、锁屏文字编辑和 `c1pkg gui` 搜索共用 `C1ancher/src/ui/input_method.c` 的 64 项异步队列：音量减上一页、音量加下一页；摇杆左右移动当前页高亮（到边缘停止）；确认/Enter/空格提交高亮项，Shift+数字键帽 1–5 仍可直选。编辑组合或收到翻页响应后，高亮回到该页第一项；短页不允许越界，空候选时不提交隐藏项。组合时上下键不改变候选，未组合时方向键仍交给原界面导航。未组合且队列空闲时音量直接走原路径；前面仍有输入时音量按原顺序等待，再决定翻页或执行原行为。

候选索引只在请求到达队首、前面所有响应已经交付后解析，不能使用物理按键到达时的旧页面。左右高亮是本地操作，不消耗 wire 序号；本地响应不携带已交付的 commit。提交和翻页转换成现有 v1 `SELECT`/`PAGE`，响应交付时恢复原按键供调用方识别；断连、队列满和超时继续丢弃不明确的输入，不重放。密码字段仍断开并绕过输入法，`PASSWORD` 响应也不会启用本地候选操作。

`c1_ime_response.highlighted_candidate` 是仅本地使用的尾部字段，原始 SDK 解码初始化为零。更新此 C 结构后需要重新编译消费者；**线协议、保留字节和旧服务均不变**，无需升级服务或扩展数据包。新客户端可连接旧服务，旧客户端的选择协议保持兼容。

包管理器保留 PageUp/PageDown 与左右方向键的区别；终端应用的音量键分别发送上一页/下一页。GUI 的 40 毫秒转义序列超时先探测已到达的输入，避免慢重绘把已排队的 CSI/SS3 后缀丢弃或误当成数字命令。`test_input_method.c`、`test_pkg_ime.c` 和 `test_desktop_input_runtime.c` 覆盖异步连按、延迟页响应、短页边界、密码绕过、旧协议、已到达后缀超时故障注入以及真实 Rime 候选提交。

## Wire v1

使用 Unix SOCK_SEQPACKET，一条消息一个数据包；最大 8192 字节，严格检查 MSG_TRUNC/MSG_CTRUNC；原生结构体仅存在于本地 SDK，不传输内存布局。所有多字节整数采用大端序。请求固定 32 字节：

- 0:u32 magic `0x4331494d`；4:u16 version=1；6:u16 operation。
- 8:u32 sequence（从 1 开始严格递增，回绕跳过 0）；12:u32 packet_length=32。
- 16:u32 X11 keysym；20:u32 modifiers；24:u32 value；28:u32 reserved=0。

响应头固定 40 字节：

- 0..15 与请求同布局，operation 增加 `0x8000`，length 为整个响应长度。
- 16:u32 原 keysym；20:u32 response flags；24:u32 status；28:u32 reserved=0。
- 32:u16 preedit 字节数；34:u16 commit 字节数；36:u16 候选数；38:u16 reserved=0。
- 后接 preedit、commit、每个候选的 u16 字节长度与 UTF-8 文本；不传输字符串终止 NUL。

SDK 文本容量为 1024，单文本最长 1023 字节，最多五候选。预编辑/候选截断到完整 UTF-8 字符并标记 TRUNCATED；commit 不允许静默截断，超限关闭连接且不可重放。请求保留字段、操作参数、UTF-8、序号及精确长度均验证。过长组合限制为 128 次普通字符处理，触限键标记 consumed 和 STATUS_LIMIT，仍可取消或退格。

## MIPS 独立交叉构建与预检

`integration/build-mips.py` 是 Linux/WSL 下的服务专用构建入口，使用 `integration/mips/toolchain.cmake` 和 `integration/mips/CMakeLists.txt`，不经过终端根 CMake，不构建 FTXUI、终端、libuv、JSON 或 UI 测试。默认 ABI 固定为 `-EL -march=mips32r2 -mabi=32 -mhard-float -mfp32`，所有可执行文件使用 `-static`。默认并行数为 2，不下载依赖、不访问或安装设备。

前提是完整的现有依赖源码、CMake 3.22+、Python 3.8+、MIPS C/C++ 工具链和静态 C/C++ 运行库；主机需要 X11 协议常量头文件 `keysym.h` / `keysymdef.h`，无需链接 Xlib。Ubuntu 22.04 的相关软件包为 `g++-mipsel-linux-gnu`、`binutils-mipsel-linux-gnu`、`x11proto-dev`；模拟测试另需 `qemu-user`。软件包安装不是脚本自动执行的动作。

从 term-ime 目录执行：

```sh
python3 integration/mips-toolchain-check.py --qemu qemu-mipsel
python3 integration/build-mips.py --build-dir /mnt/d/c1slim/.c1-ime-mips-build --jobs 2
python3 integration/tests/test_mips.py \
  --package /mnt/d/c1slim/.c1-ime-mips-build/package \
  --report-dir /mnt/d/c1slim/.c1-ime-mips-build/qemu-test
```

构建支持 `--prefix`、`--sysroot` 和 `--x11-headers`，详见 `--help`。更换编译器、sysroot 或 ABI 时应使用新的空构建目录，不能沿用 host 构建目录。工具链预检另外支持 `MIPS_CXX_PREFIX`、`MIPS_CC`、`MIPS_CXX`、`MIPS_READELF`、`MIPS_SYSROOT` 和 `MIPS_CFLAGS`；`--sdk-only` 仅检查纯 C SDK。

### 依赖和数据隔离

- yaml-cpp、LevelDB、marisa、OpenCC、librime、spdlog、utf8proc 和 `RimeIme` 包装层全部由同一 MIPS 工具链从源码编译。每个依赖子 CMake 都显式传入 toolchain/sysroot，不使用 `C1_IME_EXISTING_BUILD` 或任何 host `.a`。构建结束逐成员检查生成的归档为 MIPS32r2、little-endian、o32，并检查最终 ELF 的 FP32 与静态属性。
- OpenCC 使用已交叉构建的同一个 marisa；由于当前 marisa 头文件使用 `std::string_view`，OpenCC 配置显式启用 C++17。只构建 `libopencc`，随后通过 `CMAKE_INSTALL_LOCAL_ONLY=ON` 安装 `src/` 的库和头文件，避开 `src/tools` 和 `data` 的安装、数据生成命令。没有修改依赖源码，也没有假设不存在的 `OPENCC_BUILD_TOOLS` 选项能禁用数据生成。
- 运行包仅复制可信的 `data/rime-data/*.yaml`、`essay.txt` 和 `opencc/` 数据；不复制 host 的 Rime `build/*.bin`。Rime 的词典由 MIPS 服务首次启动时生成。随项目已有的 OpenCC `.ocd2` 数据是否可用，由模拟测试中的真实简体输出检查，而不是仅靠编译成功判定。
- Ubuntu GCC 10 的 libstdc++/gthread 对部分 `pthread_*` 函数使用弱引用；与 glibc 2.34+ 合并后的静态 libc 链接时，单独添加 `-pthread` / `-lpthread` 仍可能不提取实现。实测曾先后在 `std::thread::join` 和 LevelDB 条件变量析构时跳转到地址 0。独立 CMake 依据 `mips/pthread-symbols.txt` 显式提取对应 libc 对象，构建后检查所有列出的函数都有非零定义。预检增加线程、条件变量的可选 QEMU 执行，以防只验证链接而漏报此类问题。
- X11 只复制两个与 CPU 无关的按键常量头文件到目标暂存目录，不把主机 `/usr/include` 加入交叉头文件搜索，也不链接 host X11 库。

### 输出与测试边界

构建目录包含 `package/bin/c1-ime-service`、`c1-ime-sdk-test`、`c1-ime-client-demo` 及 `package/share/rime-data/`。这是本地验证产物，尚不是已完成许可整理和设备验收的发布包。未 strip 的服务可执行文件保留在 `service/integration/` 和 `package/bin/`；纯 C 静态 SDK 在 `service/integration/libc1-ime-client.a`。

证据文件为 `build.log`、`elf-report.txt`、`archive-audit.json`、`pthread-link-audit.json`、`hardening-audit.json`、`package-sha256.json`，模拟测试输出 `qemu-test/report.json`（包括实际测试的三个二进制 SHA-256）和服务 stdout/stderr 日志。默认使用 `qemu-mipsel -cpu 24Kf`，服务、C SDK 测试和 C 示例均执行真实 MIPS 二进制。测试使用 Linux `/tmp` 下全新的私有运行目录、用户词库和共享数据副本，允许最多 900 秒首次部署；测试只结束自己启动的前台服务，不接触其他进程或系统 socket。

模拟检查包括冷部署、READY、`nihao → 你好`、`zhongguo → 中国`（验证 OpenCC 数据）、五候选、翻页、选择、回车、退格、Escape、取消、模式/密码绕过，以及 SIGTERM 退出和自有 socket 清理。它不等同于全部 host 协议负向测试，更不等同于设备测试。

设备仍需单独核对静态 glibc 与设备内核的兼容性、实际 CPU/FPU、首次词典部署耗时、内存与存储预算、服务生命周期和桌面事件循环接入。发布前须补齐并核对 librime/OpenCC 等依赖许可声明和对应源码义务。本任务没有设备部署、守护进程安装、提交或推送。

### 2026-09-18 实测状态

Ubuntu 22.04、GCC/G++ 10.3、QEMU 6.2.0（`-cpu 24Kf`）下已完成：

- 全部服务依赖和三个可执行程序交叉编译成功；13 份构建/暂存归档共 311 个成员经检查均为目标 MIPS32r2 little-endian o32，无 host 档案。服务 ELF32、`0x70001007`、hard-float、CPR1=32，无 `INTERP`、无动态节；未 strip 约 4.36 MiB。ELF GNU ABI 标记要求 Linux 3.2.0，不能据此推定设备内核已经兼容。
- `mips-toolchain-check.py --qemu qemu-mipsel` 的 C SDK 和 C++17 filesystem/thread/条件变量执行通过；交叉构建树中的 `ctest -R '^c1-ime-sdk$'` 通过。
- `test_mips.py` 使用真正的 MIPS 服务、MIPS SDK 测试程序和 MIPS 示例，全部模拟检查通过，服务退出码为 0。全新词库首次 READY 为 **40.758 秒**（仅此主机模拟测量）。`nihao` 首候选/提交为“你好”，`zhongguo` 首候选/提交为“中国”，证实复用 OpenCC 数据在该目标组合上可用。
- 服务目标额外使用 `-Wl,-z,noexecstack,-z,relro,-z,now`，构建脚本强制检查其 `GNU_STACK` 为 RW 且存在 `GNU_RELRO`，结果写入 `hardening-audit.json`。2026-09-18 加固后完整 QEMU 回归通过，新的全新词库 READY 为 **18.568 秒**；与此前 40.758 秒均为不同运行条件下的主机模拟测量，不能归因为加固带来的性能提升。服务仍是无动态节的静态 ELF，没有动态 `BIND_NOW` 标记；这里只确认链接选项、ELF 段属性和模拟功能，不宣称动态链接意义的 full RELRO 或真实设备上的内存保护已验收。本次加固仅限服务，SDK 测试和 C 示例的栈仍为 RWE。同一构建树再次执行标准构建后服务 SHA-256 保持 `76573481965122fd28e441d2fb5d90e0b00251090ea6bb774081c26503b0b8a6`，与 QEMU 测试报告中的服务哈希一致；这验证当前环境的增量重复构建，不承诺跨工具链的逐字节可复现。
- 现有 vendored yaml-cpp / RapidJSON 编译仍有 `-Weffc++` / `-Wclass-memaccess` 警告，未修改依赖源码、未隐藏警告。WSL 的 `/mnt/d` 构建还观察到时钟偏差提示，最终链接、归档审计和运行测试均通过。

真实设备仍未连接或执行，因此结论是“全静态 MIPS 交叉构建与 QEMU 真输入通过”，不是“设备移植验收完成”。此前缺少 MIPS g++、静态 pthread 弱符号空调用两类阻塞均已解决；不需要修改 term-ime 顶层源码或依赖源码。
