# C1-Slim / MP-D261 平台规格速查（2026-08 三方交叉验证）

> 来源：C1ancher 源码逐文件核实 + `config/c1-slim/{display,keyboard}.csv` 真机验证 + `docs/{设备,驱动,固件更新}.md` + 备份镜像 debugfs 浏览。持续复用：开发 C1-Slim 应用前先读本文档。

## 1. 硬件

| 项目 | 规格 |
|---|---|
| SoC | 君正 Ingenic X1600（halley6_v20），单核 XBurst MIPS32r2 小端，~1.1GHz，FPU，无多核无 GPU |
| 内存 | 内核 `mem=64M`，可用 ~27MiB，**无 swap** |
| 存储 | 64GB eMMC：system 400M(ext2 只读)、userdata 100M、storage ~57GiB、可写区 `/usr/data`(~83MiB 可用)/`/usr/resource`(~123MiB)/`/storage` |
| 屏幕 | 2.66" 电子纸 **296×152 1bit**，SPI 驱动 `spilcd,e0266a128` |
| 键盘 | 40 键：event0 矩阵 30 键(QWERTY+SPACE+LSHIFT+HOME+VOL±) + event1 GPIO 10 键，**硬件无按键重复** |
| 音频 | ALSA ES8326 codec |
| 电池 | `/sys/class/power_supply/{battery,ac,usb}` |
| USB | MTP+ADB configfs gadget（VID/PID 18D1:D002） |

## 2. 显示（关键：无 /dev/fb0，无 DRM）

- 帧设备：`/dev/epaper_lcd`（字符设备，无 ioctl），`open(O_WRONLY|O_NONBLOCK|O_CLOEXEC)` 后 **一次 `write(fd, frame, 5624)`**
- 全刷：再向 `/sys/devices/platform/e0266a128/epaper/refresh` 写 `"1"`（驱动忙等 ~1.5s；`refresh_max=30/分钟` 上限）
- 快刷：只 write 帧、不写 refresh 节点 → 内核自动局部波形更新
- 帧格式（`offset=(y>>3)*296+x`，`mask=0x80>>(y&7)`，**MSB 在顶，黑=1 白=0**）：
  ```c
  #define W 296U  H 152U  STRIP_H 8U  STRIP_COUNT 19U  FRAME_BYTES 5624U
  ```
- 帧去重模式（C1ancher 已验证）：静态 `cached_frame[5624]` memcmp 相同跳过；唤醒后必须清缓存强制全刷
- 写帧失败处理：write 返回 != 5624 重试一次，仍失败忽略并计数

## 3. 输入（标准 evdev）

- `/dev/input/event0`：matrix_keypad@0 — HOME(102)、VOLUMEDOWN(114)、VOLUMEUP(115)、A-Z、LEFTSHIFT(42)、SPACE(57)
- `/dev/input/event1`：gpio_keys — UP(103)、LEFT(105)、RIGHT(106)、DOWN(108)、DELETE(111)、WAKEUP(143,电源键)、BACK(158)、P(25)、ENTER(28)、**OK(352)**
- 读取：两个 fd `open(O_RDONLY|O_NONBLOCK|O_CLOEXEC)` 入 pollfds，`read()` 出 `struct input_event`，过滤 `EV_KEY`；无硬件重复 → 软件合成（C1ancher 用 450ms 首延 + 90ms 间隔）
- OK/ENTER 合并为确认键；HOME 不进应用语义层（系统杀会话）

## 4. 运行时环境（c1pkg launch）

- 49×19 PTY 中 exec（bash），cwd=`/usr/data`；`PATH=/usr/data/c1/bin:/sbin:/usr/sbin:/bin:/usr/bin`；`TERM=xterm-256color`；`C1_C1ANCHER_TERMINAL=1`
- **应用可直接 open `/dev/epaper_lcd` 和 `/dev/input/*` 绕过终端**（C1ancher 自身即如此）
- HOME 键 → C1ancher 杀终端会话 → 应用收 SIGHUP（`PR_SET_PDEATHSIG=SIGHUP`）
- 休眠：前台进程组 SIGSTOP/SIGCONT（不可捕获），计时必须 `CLOCK_MONOTONIC`；唤醒后 C1ancher 全屏重绘
- 自动休眠默认关闭（`/usr/data/c1/disable-auto-suspend` 存在即禁用）

## 5. 电源

- `/sys/power/state` 可写含 `mem` 且 disable-auto-suspend 不存在时，C1ancher：5min 无输入锁屏 → 锁屏 30s → suspend
- 空闲等待建议 `read()`/`poll()` 阻塞，不要忙轮询（电子纸静止不耗电，CPU 空转耗电）

## 6. 编译目标（C1ancher 已验证配置）

```
mipsel-linux-gnu-gcc -std=gnu99/-std=c11 -Os -Wall -Wextra \
  -march=mips32r2 -mabi=32 -mhard-float -mfp32 -fstack-protector-strong \
  -ffunction-sections -fdata-sections \
  -static -Wl,--gc-sections,-z,noexecstack,-z,relro,-z,now
```
- 全静态、无共享库依赖；C1ancher 二进制 765KB（strip 后），运行时驻留 ~1.3MB
- 内存纪律：全静态/栈分配，零 malloc（C1ancher 主工程仅 1 处 malloc）

## 7. 系统软件栈

- Buildroot 2023.08.3 / Linux 5.10.186 PREEMPT / BusyBox 1.36.1 / glibc 2.38
- 可用命令：curl、wget、sqlite3、ffmpeg、tar/gzip、sha256sum、evtest、amixer/aplay、wpa_supplicant、adbd、bash
- 图形库存在但应用不应依赖：LVGL、SDL2、freetype（在只读 /usr/lib）
- **不要**在 /storage/lib 放库（主程序 RPATH 劫持风险）

## 8. c1pkg 打包格式（发布应用必须遵守）

- **catalog.json 每包恰好 5 字段**：`id`(≤32, `[A-Za-z0-9._-]`) / `version`(semver) / `displayName`(1-40 可打印 ASCII，**不能中文**) / `entry`(payload 内相对路径 ≤192 无空格，必须可执行) / `payloadDirectory`
- 包结构：GNU tar.gz，顶层恰好 `manifest.v1` + `payload/`；manifest 内容精确为：
  ```
  C1PKG-PACKAGE 1
  id\t<id>
  version\t<version>
  entry\t<entry>
  ```
- index.v1 格式（LF 结尾、制表符分隔、每行 ≤1024B、全文 ≤256KiB）：
  ```
  C1PKG-INDEX 1
  S\t<sequence 单调递增防回滚>
  P\t<id>\t<version>\t<displayName>\tpackages/<id>/<version>.tar.gz\t<sha256>\t<size>\t<entry>
  ```
- 签名：Ed25519 原始签名 64 字节存 `index.v1.sig`；设备信任根 `/usr/data/c1/pkg/repository.ed25519.pub`（32 字节原始公钥）
- 限制：包 ≤32MB、单文件 ≤16MB、解包 ≤64MB、≤128 包；tar 内禁 symlink/特殊文件/硬链接；路径禁 `..`/`\`/`:`/`*`/`?`/`[`
- 安装布局：`/usr/data/c1/apps/<id>/{current,previous}` symlink → `versions/<ver>/`（密封只读 0555/0400，`.c1pkg-entry` 记 entry）；状态 `/usr/data/c1/pkg/`（cache/staging/lock/highest-sequence）
- launch：`execv(current/<entry>)` 直接替换进程
- 设备运行时依赖：curl、sha256sum、GNU tar；验签进程内 ed25519

## 8.5 e-ink 驱动刷新语义（2026-08-29 真机实测，血泪教训）

- **`refresh_cnt` 是每次开机的全刷预算（0-30），不重置（仅重启清零）**：每次刷新消耗预算（快刷+1、手动全刷约+8）；预算耗尽后手动全刷变**静默**（不闪但仍执行全波形，dmesg "Busy wait 2070ms" 可证），ghost 清除仍有效
- **`refresh_max=30`**：预算上限，非速率限制
- **`fast_refresh_only=1`**：快刷永不触发驱动自动全刷；同时屏蔽手动全刷（refresh 写变 no-op）；**全刷前写 0、全刷后写 1 可恢复手动全刷**（预算内会闪）
- **驱动自动全刷**：flag=0 且计数跨过 30 时触发（闪烁波形），触发后计数清零
- **写帧是异步的**（write 返回 ~60us，面板处理 ~700ms/帧）；连续写超过面板处理速率会被**丢帧/合并** → 表现为跳格、头身分离观感
- **游戏 tick 必须 ≥700ms**（本面板实测 700ms 平滑、500ms 跳格、900ms 反而异常）
- 面板正常使用模式：快刷为主、场景切换显式全刷（flag 切换）、接受死亡全刷静默

## 9. 开发环境拓扑

- Linux 构建主机：安装 MIPS32r2 交叉工具链，通过受控凭据访问部署中继
- 部署中继：`<RELAY_USER>@<RELAY_HOST>`；凭据由密钥管理器提供，ADB 与工作区路径由环境变量配置
- 设备标识：由部署环境注入，不写入源码或文档
- 快速回路：构建 → 上传到部署中继 → ADB 推送到设备临时目录 → 设备验证
