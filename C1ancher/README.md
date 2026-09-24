# C1ancher

C1-Slim / MP-D261 的自研用户空间界面，面向 296×152 黑白电子纸、实体键盘和 MIPS Buildroot 系统。

- 当前源码版本：**2.9.9**。本目录属于设备端与应用开发总仓库，首次阅读请先看 [仓库首页](../README.md) 和 [普通应用开发入门](../docs/app-development.md)。
- 2.9.9 的功能、更新后自动清理范围和输入法独立发布安排见 [版本说明](docs/release-2.9.9.md)。
- 历史 GitHub 发行版：[Releases](https://github.com/fwz233-RE/C1auncher/releases)。源码版本、GitHub 附件和设备服务器渠道是不同的发布状态，请以各版本说明为准。
- 目标 ABI：ELF32、MIPS32r2、小端、o32、hard-float double、完全静态链接

本地新增的事务恢复、运行模式、主页心跳与核心可信恢复说明见 [生命周期修复](docs/lifecycle-fixes.md)。部署和物理断电/重启验收仍是独立步骤。

## 功能

- 中英双语五入口桌面：应用、终端、Wi-Fi、进程、设置；方向键选择，确认进入，主页表情键不执行操作。共用顶栏只显示当前应用名、时间和电量。
- Wi-Fi、设置、锁屏和图形包管理器采用同一套 16 像素中英文字体；中文网络名和应用信息直接显示。
- Wi-Fi 扫描后优先自动连接已保存网络；手动选择已保存网络也直接连接，不再重复输入密码。只有未保存的加密网络才进入实体键盘密码输入页，密码默认显示，可用表情键隐藏。
- 支持 `wpa_supplicant` 转义的 UTF-8 SSID，可连接中文热点。新增密码按 WPA 标准派生为十六进制 PSK，避免引号、反斜杠与注释字符在控制协议或保存文件时改变密码；已保存加密网络连接失败后允许重新输入，不泄露旧凭据，成功前保留原配置。
- 应用入口启动 296×152 双语图形包管理器（`c1pkg gui`），先显示本地库；左右切换白底黑字的本地/商店标签，细下划线标记当前页，验证/刷新/任务状态集中到页脚；上下选择，字母定位。本地确认打开、商店确认管理，单按 Shift 进入详情选择安装、更新或卸载；不再显示“表情搜索”。后台刷新不阻塞导航，兼容 UTF-8 搜索保留。`c1pkg tui` 只保留为旧终端兼容入口。
- 软件仓库索引使用内置 Ed25519 验签。图形界面按空格只刷新列表，更新由应用详情单独确认；缓存立即展示，安装前重新验证离线列表。首次使用没有应用目录时按空库处理，存储故障也不会隐藏服务器列表。支持中文署名、下载等待倒计时和取消，软件包仍使用 SHA-256 校验、受限路径解包及原子版本切换。
- 运行期间驱动右上角四灯循环跑马灯，退出或休眠时恢复原有 LED 状态。
- 持久 PTY root 终端：固定 37×8 中英文全屏内容区，保留一个顶栏；中文候选启用时为 37×6。支持 ANSI/xterm、滚动历史与窗口尺寸通知，不提供缩放操作。
- 公共 Rime 输入服务和纯 C 客户端接口：桌面锁屏文字、终端和包管理中文搜索共用；候选区预留空间，终端同步缩小 PTY，服务异常不重放可能已提交的按键。全静态 MIPS 服务已交叉构建并通过 QEMU 模拟中的真实中文输入，仍待真机资源与键盘验收，详见 [输入法接口](../term-ime/integration/README.md)。
- 进程页只管理终端和 Wi-Fi：关闭运行中的终端需要二次确认，联网校时仅保留在设置，后台检查不在进程页提供开关。关于页显示当前系统版本。
- 锁屏可选壁纸、保留当前画面、中英文文字或月历；文字按完整内容自动换行并尽量放大铺满屏幕。电源只保留省电、标准、性能三个模式，插电不触发自动锁屏、休眠或关机。
- 联网校时优先只读观察系统 ntpd 的内核同步证据；无外部服务时才探测 BusyBox NTP 后备能力，不重复启动校时守护进程。时区单独配置；事件驱动、相同帧去重和按变化刷新。
- 可恢复 Wi-Fi、USB/ADB 与终端状态的 suspend/resume 后端。
- 安装时同步系统时间和 UTC 硬件时钟。

## 操作

- 桌面：方向键只移动五个入口的选择，主页表情键无动作，`Enter` / `OK` 确认进入。返回桌面保留先前选择。设置包含系统更新和关于两项；联网时后台只检查系统元数据和轻量桌面摘要，进入设置确认系统更新后才下载/准备。
- 桌面管理的页面：电源键短按锁屏，锁屏中有效按键或电源键短按释放可解锁（本地新修正）；长按电源键 3 秒仍请求系统关机。`Home` 返回桌面。第三方独占应用的限制见下文。
- Wi-Fi：方向键选择，`Enter` 确认。密码页 `Shift` 切换大小写/键帽符号层，表情键显示/隐藏，`Delete` 删除，`Enter` 连接；密码不经过 Rime。
- 应用：左右切换本地/商店，上下选择，字母定位。`Enter` / `OK` 在本地直接打开、在商店进入管理；单按 Shift（向上箭头键帽）进入管理选择安装/更新/卸载。兼容搜索通过 `/` 或 Shift+Space 进入，确认先应用筛选，再次确认执行对应页面操作。进入应用管理后记录已查看的应用集合，主页只提示新增 ID 数量；应用内仍按原流程下载并验签完整索引。
- 终端：固定全屏，无放大缩小。表情键在英文终端发送原有补全字节；`OK+字母` 为 Ctrl 组合，`Back` 为 Esc，`Shift+Delete` 为 Delete，音量键翻阅历史，`Shift+电源短按` 打开扩展符号。
- 中文输入：在终端、包管理列表或锁屏文字框按 `Shift+Space` 切换中英文；音量减/加翻上一页/下一页，摇杆左右移动当前页高亮候选，确认、Enter 或空格提交高亮候选，Shift+数字键帽 1–5 直选。
- 设置：上下选项，左右修改并保存；锁屏文字使用编辑草稿，`Enter` 保存，`Back` / `Home` 取消，删除按完整 UTF-8 字符处理。
- 进程：只显示终端和 Wi-Fi 两个开关；不重复提供联网校时/后台检查，不展示 Linux 进程列表或任意 PID 结束操作。

默认**标准模式：无操作 3 分钟锁屏，进入锁屏后再计 5 分钟请求关机**；省电为无操作 1 分钟锁屏、进入锁屏后再计 2 分钟请求关机；性能为无操作 5 分钟锁屏并尝试休眠、不自动关机。三种模式在插电时都不自动锁屏、休眠或关机，检测到拔电后重新计时；仍允许手动锁屏。手动锁屏也会启动独立的关机倒计时；锁屏后按键唤醒会取消关机倒计时，恢复锁屏前页面并重新开始无操作计时。旧配置迁移为标准模式，保留语言、文字等非电源设置。独立计时、唤醒和关机监督握手已随序列23刷入 `MagicPen-967fa9`；真实拔电和实体按键仍需人工验收。详见 [实现与设备边界](docs/desktop-services.md)。

三模式设置、浅色应用页标签与自适应文字锁屏最初于2026-09-20刷入序列18；其当时的总无操作关机计时不等同于上述最新独立计时。实际电源动作受设备后端、外部电源检测及任务保护约束；运行会话阻止自动关机，网络任务及不安全/未知的更新状态会推迟自动休眠和关机。省电、标准不提前进入 `mem` 休眠，以免清醒计时停止后错过关机期限；真实休眠、唤醒与 USB/ADB 恢复仍须单独验收。

传统第三方程序可能独占输入或显示，桌面不能可靠替它锁屏/冻结，也不会通过删除锁文件或随意 SIGSTOP 绕过。此时暂停桌面的自动电源策略；`Home` 保留原来的结束应用会话行为，并非最小化。墨水屏不刷新不代表整机零耗电。更多边界和验证要求见 [桌面整合说明](docs/desktop-services.md) 与 [电源验收](docs/power-validation.md)。

## 媒体目录

主页启动时自动创建 `/storage/mtp/Pic`、`/storage/mtp/Music`、`/storage/mtp/Book`，首字母大写，`Book` 为单数。图片、音乐、阅读器默认读取对应目录，无需先启动应用创建文件夹。重复启动保留已有内容；旧目录中的文件不会被删除或自动搬移。

锁屏固定读取 `/storage/mtp/Pic/wallpaper.raw`，缺失时自动补入原来的默认壁纸；仅在该文件缺失时兼容迁移旧的 `/storage/mtp/pic/wallpaper.raw`。浏览其他图片不会更换锁屏壁纸。

## 构建

需要 Windows、WSL Ubuntu、`make`、`gcc`、`gcc-mipsel-linux-gnu` 和 `binutils-mipsel-linux-gnu`。

```powershell
.\scripts\build.ps1
```

构建流程会执行主机测试、键盘配置验证、MIPS 交叉构建和 ABI 检查，输出：

```text
build/C1ancher
build/C1ancher-launcher
build/c1pkg
build/build-info.txt
```

在目标设备查询编译版本：

```sh
./build/C1ancher --version
# C1ancher 2.0.2
```

## 安装

**已注册签名核心的设备安装本次统一桌面，请使用 [本地构建与自行安装](docs/desktop-install.md)。** 新的 `build-desktop-bundle.ps1` 将核心与预编译 MIPS 输入法分别签名；`prepare-desktop-install.py` 默认只离线验签，显式 `--install` 后才先安装输入法、再准备核心，始终不重启。以下历史安装入口不负责新输入法依赖，不能用覆盖二进制的方式替代签名更新。

安装器要求只连接一台受支持设备，并具有 root ADB。它会校验已知哈希，部署 C1ancher、launcher、`c1pkg`、仓库公钥和 Neofetch，清理旧版 C1ancher SSH 运行状态，同时保持根文件系统只读。

```powershell
.\scripts\install-default-app.ps1 -Action Install -Reboot
.\scripts\install-default-app.ps1 -Action Verify -Reboot
```

开放 root ADB 的安装和验证：

```powershell
.\scripts\install-open-adb.ps1 -Action Install -Reboot
.\scripts\install-open-adb.ps1 -Action Verify -Reboot
```

应用和持久配置安装到 `/usr/data/c1/`。安装证据保存在本机 `artifacts/`，设备恢复副本保存在 `/usr/data/c1/recovery/` 和 `/storage/c1/recovery/`。

## APP 软件仓库

普通应用通过发布工具打包上传，操作步骤见 [打包与发布](../docs/publishing.md)。

旧 `build-app-package.ps1` / `publish-app-repo.ps1` 保留用于历史仓库维护和迁移，不用于覆盖新服务数据；旧 `setup-app-repo.sh` 不能用来覆盖新服务的代理路由。

设备上的 APP 管理器和独立应用启动命令：

```sh
clear; /usr/data/c1/bin/c1pkg gui
/usr/data/c1/bin/c1pkg launch hello
```

`/usr/data/c1/bin` 已加入 C1ancher 终端的 `PATH`，所以也可以简写成 `c1pkg gui` 和 `c1pkg launch hello`。图形管理器使用独立屏幕绘制，PTY 仅转发按键，普通终端任务仍保留。详情页选择“打开”时释放界面占用并把终端交给应用。`c1pkg tui` 作为旧 ANSI 界面兼容入口保留（旧界面的 R 仍执行刷新并批量更新）。新界面与首次使用修复说明见 `docs/pkg-gui.md`。

仓库默认使用 `http://www.fwz233.com`，官方域名访问失败时固定回退到 `http://123.56.214.77`，保留路径且不要求 HTTPS。普通应用读取 `/usr/data/c1/pkg/repository.url`，核心更新读取 `/usr/data/c1/update/repository.url`；显式自定义仓库优先，不会被替换为官方 IP。刷机时部署相同的公开配置和可信公钥，每台独立保存自己的版本及防降级历史。普通应用开放发布不需要令牌；HTTP 不加密上传内容，须用 `-allow-insecure-http` 显式接受风险。维护者更新受保护应用时使用的令牌同样不受 HTTP 加密保护；核心私钥仍离线保存。公开开发流程见 [发布说明](../docs/publishing.md)。

设备端状态位于 `/usr/data/c1/pkg/`，已安装应用位于 `/storage/c1/apps/`。作者来自服务端授权配置；旧格式索引没有作者时显示 `Unknown`，不会猜测或冒认署名。

## 上一已发布版本文件

| 文件 | 内容 |
| --- | --- |
| `C1ancher-v1.2.0-c1-slim-mipsel-static.tar.gz` | 完整应用固件部署包 |
| `C1ancher-v1.2.0-mipsel-static` | 静态主程序 |
| `C1ancher-launcher-v1.2.0-mipsel-static` | 静态 supervisor |
| `c1pkg-v1.2.0-mipsel-static` | 静态 APP 包管理器 |
| `SHA256SUMS.txt` | SHA-256 校验值 |

这些文件是 **C1ancher 应用固件和安装工具**，不是包含分区表、内核和根文件系统的整机刷机镜像。

## 安全说明

- 设备启用了开放 root ADB，任意连接的电脑都可以获得 root shell。
- APP 仓库使用公网 HTTP，因此应用名称、版本和下载流量不保密；Ed25519 签名、SHA-256 和单调序列号负责真实性、完整性和防降级。
- 核心签名私钥保持离线；新发布服务需要在线普通应用签名能力，只有维护者明确批准后才配置。设备只持有公钥，开发者只持有各自可撤销的上传令牌。
- Wi-Fi 密码保存在权限为 `0600` 的 `/usr/data/c1/wifi/wpa_supplicant.conf`，不会写入诊断日志。
- 当前目标设备上的原厂 `/usr/bin/d261` 已删除；恢复原厂系统必须使用完整系统备份，不能只恢复启动脚本。
- 支持硬件的默认启用策略仍需真实 `mem` 休眠、唤醒后 USB/ADB 自动恢复循环和长期功耗验收；发布安装包前验证其实际附带的核心版本。

## 项目结构

```text
src/          应用、UI、服务、包管理器和 Linux 后端
scripts/      构建、安装、仓库发布、验证和功耗测试
tests/        主机自动测试
third_party/  libtsm、Ed25519 与 Neofetch
config/       C1-Slim 硬件和 APP 仓库配置
docs/         真机验收说明
```

第三方许可证见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)，项目许可证见 [`LICENSE`](LICENSE)。