# C1ancher

C1-Slim / MP-D261 的自研用户空间界面，面向 296×152 黑白电子纸、实体键盘和 MIPS Buildroot 系统。

- 当前源码版本：**2.0.0**。本目录属于设备端与应用开发总仓库，首次阅读请先看 [仓库首页](../README.md) 和 [普通应用开发入门](../docs/app-development.md)。
- 历史 GitHub 发行版：[Releases](https://github.com/fwz233-RE/C1ancher/releases)。源码版本、GitHub 附件和设备服务器渠道是不同的发布状态，请以各版本说明为准。
- 目标 ABI：ELF32、MIPS32r2、小端、o32、hard-float double、完全静态链接

本地新增的事务恢复、运行模式、主页心跳与核心可信恢复说明见 [生命周期修复](docs/lifecycle-fixes.md)。部署和物理断电/重启验收仍是独立步骤。

## 功能

- 九宫格首页：放大居中的 Wi-Fi、APP、终端和设备信息入口，以及电量和时间。
- Wi-Fi 扫描后优先自动连接已保存网络；手动选择已保存网络也直接连接，不再重复输入密码。只有未保存的加密网络才进入实体键盘密码输入页，密码默认显示，可用 Tab 隐藏。
- 支持 `wpa_supplicant` 转义的 UTF-8 SSID，可连接中文热点。
- `APP` 默认启动 296×152 中文图形包管理器（`c1pkg gui`），进入“已安装”。连续输入应用名称或 ID 前缀定位，回车/确定直接打开；左右切换可下载/已安装，安装、更新及默认取消的卸载确认保留在应用详情。`c1pkg tui` 保留为旧终端兼容入口。
- 软件仓库索引使用内置 Ed25519 验签。图形界面按空格只刷新列表，更新由应用详情单独确认；缓存立即展示，安装前重新验证离线列表。首次使用没有应用目录时按空库处理，存储故障也不会隐藏服务器列表。支持中文署名、下载等待倒计时和取消，软件包仍使用 SHA-256 校验、受限路径解包及原子版本切换。
- 运行期间驱动右上角四灯循环跑马灯，退出或休眠时恢复原有 LED 状态。
- 49×19 持久 PTY root 终端，支持 ANSI/xterm、滚动历史和常用全屏程序。
- `DEVICE` 复用终端并自动运行随附的 Neofetch 7.1.0。
- 壁纸锁屏、事件驱动主循环、相同帧去重和按变化刷新。
- 可恢复 Wi-Fi、USB/ADB 与终端状态的 suspend/resume 后端。
- 安装时同步系统时间和 UTC 硬件时钟。

## 操作

| 场景 | 按键 | 行为 |
| --- | --- | --- |
| 首页 | 上 / 左 / 右 / 下 | Wi-Fi / APP / 终端 / DEVICE |
| 首页或锁屏 | OK 或电源键 | 锁屏或解锁 |
| 任意页面 | 电源键长按 3 秒 | 请求系统关机 |
| 普通页面 | Home | 返回首页 |
| Wi-Fi | 左右、上下、Enter | 选择操作或网络并确认 |
| 密码输入 | Shift | 循环切换 `abc`、`ABC`、符号层 |
| 密码输入 | Space / Delete / Enter | 空格 / 删除 / 提交 |
| APP | 音量加 / 音量减 | 切换到已安装 / 可下载列表 |
| APP | 左右 | 在可下载（左）和已安装（右）之间切换；详情页中选择操作 |
| APP | 上下 / Enter 或 OK | 选择应用 / 打开详情；详情页确认安装、更新、打开或卸载 |
| APP | `r` / `q` 或 Back | 仅刷新软件列表 / 返回；下载等待中取消当前操作 |
| 终端 | OK+字母 | Ctrl+A–Z；OK 单击不触发 APP 列表切换 |
| 终端 | Back / Shift+Delete | Esc / Delete |
| 普通终端 | 音量键 | 翻阅滚动历史 |
| 终端 | Wakeup | 打开扩展符号面板 |

首页及其他页面连续 5 分钟无输入会进入壁纸锁屏；锁屏与外部电源离线均持续 20 秒后，才请求 Linux `mem` 休眠。**首次安装仅在受支持的 `ingenic,halley6_v20` 硬件上默认启用自动深度休眠**，重装保留已有明确禁用设置，不受支持硬件保持禁用。可用 `-EnableAutoSuspend` 或 `-DisableAutoSuspend` 明确设置偏好。USB 单独恢复探测已通过，真实 `mem` 休眠与唤醒后 USB/ADB 自动恢复仍待验收；默认策略不代表物理验证通过。详细步骤见 [`docs/power-validation.md`](docs/power-validation.md)。

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
# C1ancher 2.0.0
```

## 安装

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

普通应用通过独立发布工具上传到现有服务器，开发者不需要服务端源码或服务器密码，见 [打包与发布](../docs/publishing.md)。服务端实现与生产部署记录不包含在本公开仓库中。

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