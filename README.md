# C1ancher

C1-Slim / MP-D261 的自研用户空间界面，面向 296×152 黑白电子纸、实体键盘和 MIPS Buildroot 系统。

- 当前版本：**1.3.0**
- 上一个已发布版本：[GitHub Release v1.2.0](https://github.com/fwz233-RE/C1ancher/releases/tag/v1.2.0)
- 目标 ABI：ELF32、MIPS32r2、小端、o32、hard-float double、完全静态链接

## 功能

- 九宫格首页：放大居中的 Wi-Fi、APP、终端和设备信息入口，以及电量和时间。
- 开放 Wi-Fi 点击后直接连接；加密 Wi-Fi 使用实体键盘输入密码。
- 支持 `wpa_supplicant` 转义的 UTF-8 SSID，可连接中文热点。
- `APP` 复用终端并启动 49×19 的 `c1pkg` TUI，合并已安装与服务器应用，支持启动、安装、版本感知更新和带确认的卸载管理。
- 软件仓库索引使用内置 Ed25519 验签，支持验签缓存回退；软件包使用 SHA-256 校验、受限路径解包、原子版本切换和回滚保护。
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
| 普通页面 | Home | 返回首页 |
| Wi-Fi | 左右、上下、Enter | 选择操作或网络并确认 |
| 密码输入 | Shift | 循环切换 `abc`、`ABC`、符号层 |
| 密码输入 | Space / Delete / Enter | 空格 / 删除 / 提交 |
| APP | 音量加 / 音量减 | 切换到 `DOWNLOAD`（可下载）/ `MY APPS`（已安装） |
| APP | 左右 | 在 `MY APPS`（已安装）和 `DOWNLOAD`（可下载）之间切换 |
| APP | 上下 / Enter 或 OK | 选择应用；已安装列表中启动，下载列表中检查、安装或更新 |
| APP | `r` / `q` | 刷新已签名仓库 / 退出到终端 |
| 终端 | OK+字母 | Ctrl+A–Z；OK 单击不触发 APP 列表切换 |
| 终端 | Back / Shift+Delete | Esc / Delete |
| 普通终端 | 音量键 | 翻阅滚动历史 |
| 终端 | Wakeup | 打开扩展符号面板 |

首页及其他页面连续 5 分钟无输入会进入壁纸锁屏，锁屏 30 秒后请求 Linux `mem` 休眠。**自动深度休眠默认关闭**；必须先完成物理唤醒和 ADB 重连探测，再使用 `-EnableAutoSuspend` 显式启用。详细步骤见 [`docs/power-validation.md`](docs/power-validation.md)。

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
# C1ancher 1.3.0
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

服务器初始化、打包和发布命令：

```powershell
# 首次在服务器执行 scripts/setup-app-repo.sh，建立只读 Caddy 仓库。
.\scripts\build-app-package.ps1 -CatalogPath .\config\app-repo\catalog.json -Sequence 1 -InitializeSigningKey
.\scripts\publish-app-repo.ps1 -ReleaseDirectory .\build\app-repo\1
```

设备上的 APP 管理器和独立应用启动命令：

```sh
clear; /usr/data/c1/bin/c1pkg tui
/usr/data/c1/bin/c1pkg launch hello
```

`/usr/data/c1/bin` 已加入 C1ancher 终端的 `PATH`，所以也可以简写成 `c1pkg tui` 和 `c1pkg launch hello`。从管理界面启动应用时，管理器会先清屏、显示对应的 `c1pkg launch <应用ID>` 命令，然后退出并把终端交给应用；应用结束后返回普通终端，不会自动重新进入管理界面。

仓库默认地址为 `http://120.26.180.48/c1/v1`。HTTP 只负责传输公开内容；设备只接受由本机私钥签名、且序列号不低于历史记录的索引。私钥默认保存在项目目录之外的 `%LOCALAPPDATA%\C1ancher\secrets\`，不会上传服务器或设备。

应用目录由 JSON catalog 描述，格式见 [`config/app-repo/catalog.example.json`](config/app-repo/catalog.example.json)。设备端状态位于 `/usr/data/c1/pkg/`，已安装应用位于 `/usr/data/c1/apps/`。

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
- 仓库签名私钥只能保存在受保护的本机目录；服务器和设备都不持有私钥。
- Wi-Fi 密码保存在权限为 `0600` 的 `/usr/data/c1/wifi/wpa_supplicant.conf`，不会写入诊断日志。
- 当前目标设备上的原厂 `/usr/bin/d261` 已删除；恢复原厂系统必须使用完整系统备份，不能只恢复启动脚本。
- 未完成真机 suspend 循环和长期功耗验收前，不应默认启用自动深度休眠。

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