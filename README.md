# C1ancher

C1-Slim / MP-D261 的自研用户空间界面，面向 296×152 黑白电子纸、实体键盘和 MIPS Buildroot 系统。

- 当前版本：**1.0.0**
- [GitHub Release v1.0.0](https://github.com/fwz233-RE/C1ancher/releases/tag/v1.0.0)
- 目标 ABI：ELF32、MIPS32r2、小端、o32、hard-float double、完全静态链接

## 功能

- 九宫格首页：Wi-Fi、SSH、终端、设备信息、电量和时间。
- 开放 Wi-Fi 点击后直接连接；加密 Wi-Fi 使用实体键盘输入密码。
- 支持 `wpa_supplicant` 转义的 UTF-8 SSID，可连接中文热点。
- 免密码 root SSH，使用设备独立 Ed25519 host key。
- 49×19 持久 PTY root 终端，支持 ANSI/xterm、滚动历史和常用全屏程序。
- `DEVICE` 复用终端并自动运行随附的 Neofetch 7.1.0。
- 壁纸锁屏、事件驱动主循环、相同帧去重和按变化刷新。
- 可恢复 Wi-Fi、USB/ADB、SSH 与终端状态的 suspend/resume 后端。
- 安装时同步系统时间和 UTC 硬件时钟。

## 操作

| 场景 | 按键 | 行为 |
| --- | --- | --- |
| 首页 | 上 / 左 / 右 / 下 | Wi-Fi / SSH / 终端 / DEVICE |
| 首页或锁屏 | OK 或电源键 | 锁屏或解锁 |
| 普通页面 | Home | 返回首页 |
| Wi-Fi | 左右、上下、Enter | 选择操作或网络并确认 |
| 密码输入 | Shift | 循环切换 `abc`、`ABC`、符号层 |
| 密码输入 | Space / Delete / Enter | 空格 / 删除 / 提交 |
| 终端 | OK+字母 / OK 单击 | Ctrl+A–Z / Tab |
| 终端 | Back / Shift+Delete | Esc / Delete |
| 终端 | 音量键 | 翻阅滚动历史 |
| 终端 | Wakeup | 打开扩展符号面板 |

任意非首页页面连续 5 分钟无输入会返回首页并锁屏，锁屏 30 秒后请求 Linux `mem` 休眠。**自动深度休眠默认关闭**；必须先完成物理唤醒和 ADB 重连探测，再使用 `-EnableAutoSuspend` 显式启用。详细步骤见 [`docs/power-validation.md`](docs/power-validation.md)。

## 构建

需要 Windows、WSL Ubuntu、`make`、`gcc`、`gcc-mipsel-linux-gnu` 和 `binutils-mipsel-linux-gnu`。

```powershell
.\scripts\build.ps1
```

构建流程会执行主机测试、键盘配置验证、MIPS 交叉构建和 ABI 检查，输出：

```text
build/C1ancher
build/C1ancher-launcher
build/build-info.txt
```

在目标设备查询编译版本：

```sh
./build/C1ancher --version
# C1ancher 1.0.0
```

## 安装

安装器要求只连接一台受支持设备，并具有 root ADB。它会校验已知哈希、部署 C1ancher、launcher、Neofetch 和 SSH host key，同时保持根文件系统只读。

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

## Release 文件

| 文件 | 内容 |
| --- | --- |
| `C1ancher-v1.0.0-c1-slim-mipsel-static.tar.gz` | 完整应用固件部署包 |
| `C1ancher-v1.0.0-mipsel-static` | 静态主程序 |
| `C1ancher-launcher-v1.0.0-mipsel-static` | 静态 supervisor |
| `SHA256SUMS.txt` | SHA-256 校验值 |

这些文件是 **C1ancher 应用固件和安装工具**，不是包含分区表、内核和根文件系统的整机刷机镜像。

## 安全说明

- 设备启用了开放 root ADB，任意连接的电脑都可以获得 root shell。
- SSH 启用后允许同一网络中的客户端免密码登录 root。
- Wi-Fi 密码保存在权限为 `0600` 的 `/usr/data/c1/wifi/wpa_supplicant.conf`，不会写入诊断日志。
- 当前目标设备上的原厂 `/usr/bin/d261` 已删除；恢复原厂系统必须使用完整系统备份，不能只恢复启动脚本。
- 未完成真机 suspend 循环和长期功耗验收前，不应默认启用自动深度休眠。

## 项目结构

```text
src/          应用、UI、服务和 Linux 后端
scripts/      构建、安装、验证和功耗测试
tests/        主机自动测试
third_party/  libtsm 与 Neofetch
config/       C1-Slim 硬件配置
docs/         真机验收说明
```

第三方许可证见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)，项目许可证见 [`LICENSE`](LICENSE)。