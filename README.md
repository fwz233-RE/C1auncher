# C1ancher

`C1ancher` 是 C1-Slim / MP-D261 的自研用户空间应用。当前设备已将它设为唯一的默认开机界面，同时启用开放 root ADB。原厂英语学习软件 `/usr/bin/d261` 已从设备删除，完整系统备份仍保留在主机上用于整机恢复。

## 当前安全边界

- 目标 ABI：MIPS32r2、小端、o32、hard-float double，静态链接。
- 生产二进制只接受 `app` 启动模式，不再包含早期硬件清单、按键采集、LED 识别、测试图或限时 UI 预览入口。
- 应用只保留当前首页、Wi-Fi、SSH、持久终端和 `DEVICE` 的 Neofetch 快捷入口所需的硬件访问。
- 根文件系统必须保持只读；持久配置和应用文件仅写入 `/usr/data/c1/`。

## 开机开放 root ADB

设备已将原厂 `S90usb` 的现有 ADB FunctionFS 启动项启用，开机同时提供 ADB 与 MTP。未配置 `/adb_keys` 或 `/data/misc/adb/adb_keys`，因此任意连接电脑都可直接获得 root shell；这是明确选择的开放开发模式，不具备 USB 访问控制。

安装、验证和卸载统一使用：

```powershell
.\C1ancher\scripts\install-open-adb.ps1 -Action Install -Reboot
.\C1ancher\scripts\install-open-adb.ps1 -Action Verify -Reboot
.\C1ancher\scripts\install-open-adb.ps1 -Action Uninstall -Reboot
```

安装器只接受已确认的原始或已安装哈希，上传后在设备端执行语法与 SHA-256 校验，失败时自动恢复原文件并把根分区重新挂载为只读。原始 `S90usb` 保存在 `/etc/init.d/S90usb.c1-original`、`/usr/data/c1/recovery/open-adb/` 和 `/storage/c1/recovery/open-adb/`；完整原始 system 镜像仍位于主机备份目录。最终冷启动验证确认 root ADB、`app_daemon` 和 `C1ancher` 均正常恢复。

## 默认自研应用

`/etc/app_daemon` 现在是最小分发 shim，只执行 `/usr/data/c1/bin/app_daemon` supervisor；supervisor 始终启动 `/usr/data/c1/bin/C1ancher app`。`C1ancher` 无论连续快速退出多少次都会在一秒后重新启动，不再存在失败计数、崩溃阈值、Home+Back 逃生键、`force-original` 状态或启动原厂程序的路径。若 `C1ancher` 二进制暂时缺失，supervisor 会继续等待并重试同一路径。

安装、重复冷启动验证和删除原厂软件使用以下统一命令；它会同时管理 C1ancher、`app_daemon` supervisor 和随附的 Neofetch：

```powershell
.\C1ancher\scripts\install-default-app.ps1 -Action Install -Reboot
.\C1ancher\scripts\install-default-app.ps1 -Action Verify -Reboot
.\C1ancher\scripts\install-default-app.ps1 -Action RemoveOriginal -Reboot
```

原始 `app_daemon` 副本仍保存在 `/etc/app_daemon.c1-original`、`/usr/data/c1/recovery/default-app/` 和 `/storage/c1/recovery/default-app/`，但安装器不再提供直接恢复原厂启动器的操作，因为设备上的 `/usr/bin/d261` 已删除。恢复原厂系统必须使用主机上的完整系统备份同时恢复启动器和 `/usr/bin/d261`。设备测试会连续五次强制终止 `C1ancher`，并验证每次都生成新的 `C1ancher` 进程且 `app_daemon` 始终为单实例。

## Wi-Fi 与 SSH

首页改为覆盖整个 296×152 屏幕的 3×3 九宫格，不再显示独立状态栏、`DESKTOP` 标题或屏幕外边框，只保留 2 像素内部井字分隔线。中心黑底区域使用一个白色原点表示默认锁定位置；按方向键会立即进入对应界面，不经过“移动选择后再按 Enter”的步骤：上方 `WI-FI` 进入无线网络页并开始扫描，左侧 `SSH` 进入 SSH 页，右侧 `TERMINAL` 进入终端，下方 `DEVICE` 进入同一个持久终端并自动执行 `neofetch`。四角使用单行居中大字和黑底白字反色样式，分别显示 `WIFI ON/OFF`、`SSH ON/OFF`、电量百分比和时间。Home 从终端返回九宫格首页。`DEVICE` 不拥有独立详情页。仅在九宫格首页按实体 OK 会进入锁屏，再按一次 OK 返回首页；锁屏显示由用户图片居中裁剪、缩放并抖动为 296×152 的原生 1-bit 壁纸，锁屏期间不进行周期刷新。Wi-Fi、SSH、终端和密码输入页面的 OK/Enter 行为保持原功能，不会触发锁屏。

`WI-FI` 页面会真实加载无线驱动、启动受控 `wpa_supplicant` 并扫描附近网络。页面顶部固定一行两个操作按钮，左侧为 `SCAN WI-FI`，右侧为 `TURN WI-FI OFF`，下方仅显示扫描到的网络；左右键切换操作按钮，上下键在操作栏和网络列表之间移动；触发扫描后会先清空旧列表并显示大号 `SCANNING WI-FI`、`PLEASE WAIT` 和加载指示，再执行同步扫描。选择 SSID 后进入实体键盘优先的密码输入页：初始为 `abc` 层，实体 A–Z 键按当前 `abc`、`ABC` 或符号层输入；单独按下并松开实体 Shift 会循环切换三层，按住 Shift 再按字母键则临时输入该实体键帽上印刷的数字或符号，松开后保留原输入层。Wi-Fi 密码输入框以 2× 字号直接显示最近 34 个字符，超长输入自动向左滚动，并显示当前可见范围和总长度；屏幕不再重复显示字母软键盘，`123#+` 层只显示实体键帽无法输入的 `! + = [ ] { } < > \ | _`，使用方向键选择并按实体 OK 输入。Space 输入空格，Delete 删除，Enter 直接提交。提交后立即离开明文输入页，连接成功后通过 `udhcpc` 获取 IPv4。受控配置保存在 `/usr/data/c1/wifi/wpa_supplicant.conf`，权限为 `0600`；应用不会将 PSK 写入诊断记录或日志。2026-08-15 已从首页 Enter 事件验证驱动、控制 socket 和真实扫描列表，返回 8 个脱敏网络；由于验收过程没有取得用户 Wi-Fi 密码，尚未替用户连接其中的加密网络。2026-08-17 扫描加载画面、顶部操作栏、大字号明文 Wi-Fi 输入框、紧凑密码页和 Shift 交互已通过主机状态机测试和 MIPS32r2 交叉构建，仍需在真机逐项复验界面与键帽符号映射。

`SSH` 页面不再显示密码输入界面。服务关闭时按 Enter 会立即启用免密码 root SSH，服务启用时按 Enter 会将其关闭。设备 host identity 是安装时生成的独立 Ed25519 key，权限为 `0600`，保存在 `/usr/data/c1/ssh/`。启用期间，应用只在 `/run/c1/ssh/` 生成临时空密码 shadow，并以 bind mount 临时覆盖 `/etc/shadow`；永久 shadow 保持不变。关闭 SSH 会终止本应用启动的 `sshd`、卸载临时 shadow 并清理运行时文件；重启同样恢复锁定状态，SSH 默认关闭。该模式允许同一网络中的客户端无需凭据直接取得 root shell，只适用于明确接受该风险的开发环境。

2026-08-15 的旧密码版已完成真实 `sshd` 监听、root 登录、停止服务、22 端口关闭和 shadow 卸载验收。2026-08-23 已将会话密码输入 UI、密码哈希生成和密码参数传递代码删除，改为临时空密码登录模式。

`TERMINAL` 页面现在是持久的本地 root 交互终端，不再使用单次 `/bin/sh -c` 命令和 5 秒超时：应用通过 Linux PTY 启动 `/bin/bash -i`，失败时回退 `/bin/sh -i`，并使用固定的 `libtsm` v4.7.1 解析 VT100–VT520/xterm 控制序列、备用屏幕和滚动历史，因此可以运行 `vi`、`top`、`less` 和交互式 `ssh`。首页下方 `DEVICE` 区域复用这个终端，每次向下进入都会先清屏并自动输入一次 `neofetch`，不再包含独立设备详情 UI 或对应页面代码。终端使用 49×19 的全屏 5×7 等宽网格，不再保留底部按键提示栏；首版字体覆盖可打印 ASCII 和常见方框线，其他 Unicode 字符显示为明确的替代方框。实体字母、Space、Enter 和方向键直接输入；单独按下并松开 Shift 会在 `abc`、`ABC` 和实体键帽符号层之间切换，按住 Shift 再按字母会临时输入对应键帽符号。Wakeup 打开 `! + = [ ] { } < > \ | _` 扩展符号面板，方向键选择、Enter 插入，Wakeup 或 Back 关闭。OK 按住再按字母发送 Ctrl+A–Z，OK 单击发送 Tab，Back 发送 Esc，Delete 发送退格，Shift+Delete 发送向前 Delete，音量键翻阅历史。Home 返回首页但保留会话，Shift+Home 向终端发送 Home；再次进入恢复原来的 Shell 或前台程序。方向、Delete 和滚动键具有应用层长按重复。终端输出按约 150 ms 合并电子纸快刷，高速输出会显示最新完整画面而不会逐字刷新。Shell 执行 `exit` 后页面显示结束状态，按 Enter 可重新启动；应用退出或重启会清理终端会话。

2026-08-23 已完成完整符号集、PTY 三路 TTY、49×19 窗口、ANSI 光标移动、反色、Ctrl-C 和会话退出检测的主机自动测试，并完成 MIPS32r2 完全静态构建和真机安装。设备端已确认 `xterm-256color` terminfo 以及 `bash`、`vi`、`top`、`less`、`ssh` 均存在；安装后二进制哈希与构建产物一致，`app_daemon` 与 `C1ancher` 各保持单实例，根文件系统仍为只读。实体按键组合、全屏程序显示效果、电子纸输入延迟和长时间前台会话仍需操作者在设备上进行最终人工验收。

`third_party/neofetch/` 随项目固定保留 Neofetch 7.1.0 原始脚本和 MIT 许可证，并包含适配 49×19 黑白电子纸终端的 C1-Slim 启动脚本、设备信息配置和自定义 Logo。每次运行固定显示 C1-Slim 自定义 Logo。统一安装器会把 Neofetch 与 C1ancher 主程序、`app_daemon` supervisor 一起部署到 `/usr/data/c1/`；终端 PATH 包含 `/usr/data/c1/bin`，执行 `neofetch` 会紧凑显示设备、SoC、CPU、Buildroot、内核、运行时间、内存、电池与电压、Wi-Fi、IP 和存储，不再需要下载或单独安装外部 Neofetch 文件夹。

## 结构

- `src/core/`：状态码和运行时结构化记录。
- `src/hal/linux/`：当前应用实际使用的电子纸、状态读取和 UI 运行时。
- `src/services/`：Wi-Fi、会话级 SSH 与持久 PTY root Shell 的真实系统服务控制。
- `third_party/libtsm/`：固定版本的 VT/xterm 终端状态机源码及许可证。
- `third_party/neofetch/`：Neofetch 7.1.0 原始脚本、MIT 许可证及 C1-Slim 定制启动脚本和配置；具体分发说明见 `THIRD_PARTY_NOTICES.md`。
- `src/platform/`：信号与进程生命周期适配。
- `src/launcher/`：子进程生命周期监管和 `C1ancher` 固定重启策略。
- `scripts/`：构建、真机临时运行、持久安装和恢复控制。
- `tests/`：不依赖真机的主机测试。

## 构建

需要 WSL 中的 `make`、`gcc`、`gcc-mipsel-linux-gnu` 和 `binutils-mipsel-linux-gnu`。

```powershell
.\C1ancher\scripts\build.ps1
```

构建脚本会先执行应用与 launcher 主机测试，再生成 `build/C1ancher` 和 `build/C1ancher-launcher`，并分别验证 ELF32、MIPS32r2、小端、o32、hard-float double 和完全静态链接。WSL 的详细标准输出和警告会暂存到系统临时文件，成功时只显示摘要，失败时回放完整日志，避免大量跨 WSL 输出导致 PowerShell 包装进程不能正常返回。

## 安装与验证

```powershell
.\C1ancher\scripts\install-default-app.ps1 -Action Install
.\C1ancher\scripts\install-default-app.ps1 -Action Verify
```

安装器会在一次操作中部署并核对 C1ancher、`app_daemon` supervisor、Neofetch 7.1.0、设备 Neofetch 配置和 SSH host key 的 SHA-256，同时验证默认应用单实例、Neofetch 实际输出及只读根文件系统。项目不依赖工作区之外的固件解包或备份目录；安装器会在目标设备上按固定原厂哈希验证并保存原始 `/etc/app_daemon`，以便卸载回退。早期用于硬件摸底的清单、按键采集、LED、音频、测试图和 UI 预览脚本已经移除；屏幕刷新速度等硬件分析应通过 ADB 只读状态、外部高帧率录像或专门的临时工具完成，不再编入默认应用。
