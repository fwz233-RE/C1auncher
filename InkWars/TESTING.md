# 验证记录与复现方法

## 0.1.1 本轮回归

新增 `test_session` 多进程独占锁／exec 继承／PTY 回显恢复测试，以及 `test_device_runtime`：编译真实设备分支，用普通文件替代面板、FIFO 传入 Linux EV_KEY，验证首帧无输入显示、确认后仍持锁、恢复重绘和预算不变。`test_terrain` 验证十一种像素地形、连通与裁剪，另生成两张图鉴／地图预览。启动器的 `platform/app_lease.c` 原样复用，测试使用它的共享写屏锁作为真实竞争方。

本轮 ADB 查询无设备；以下真机记录属于 0.1.0，不能用来宣称 0.1.1 的用户手按故障已经面板验收。旧脚本曾暂停启动器，漏测了争用覆盖；新版 `device-smoke.sh` **保持启动器运行**。设备日志改写 `/usr/data/inkwars/last-run.log`，脚本在程序结束后读取，不向游戏运行中的 PTY 输出。


## 主机与交叉编译

环境：Windows + WSL Ubuntu 22.04，GCC 与 mipsel-linux-gnu 工具链；主流程实际安装了 Python Pillow、QEMU user 和 clang-format（后者不是构建依赖）。

- `make test`：运行时回归、19,063 项规则／存档断言、UI 操作闭环。
- UI 在未经改写的青岚河谷初始地图上，经相同按键处理函数完成：新建热座局→生产步兵→移动→两次占领中立城→两方坦克靠近→预测与交战／反击→保存→保存退出→主菜单重载。共 9 次换边计数，15 张预览。
- 额外显式边界场景测试能力按钮、两次总部占领和胜利返回，以及 AI 回合不泄露对方迷雾视野。这些边界布置没有被冒充自然开局截图。
- ASan／UBSan 运行规则和 UI 闭环，无报错。
- `make target verify mips-test`：ELF32、小端、MIPS32r2、o32、硬浮点 FP32、无动态解释器／动态段。
- QEMU 运行静态 MIPS 规则与 UI 测试通过。指定 WSL 的 `/mnt/d` 为存档目录时，额外三次原子覆盖保存／加载通过，总断言 19,073；这修复了 MIPS32 普通 stat 在大 inode 上的 EOVERFLOW。
- `tools/verify_source.py` 将随附源码包解压到独立临时目录，重新运行主机测试、交叉构建和 ABI 检查，重建的 MIPS 可执行文件与发布文件逐字节相同。
- 预览生成脚本断言每张帧尺寸 296×152、模式为 1bit、像素只有黑白；PNG 无平滑缩放。实际查看过主菜单、地图、指挥官选择、生产、预测、结算和迷雾预览，并修复过指挥官名中间点缺字。

常用命令：

```sh
make test target verify previews mips-test
qemu-mipsel build/test_ui-mips build/mips-previews
INKWARS_TEST_SAVE_DIR="$PWD/build" qemu-mipsel build/test_game-mips
```

## 0.1.0 历史真机记录（非本轮验收）

已通过 ADB 在当前连接的 C1-Slim 上实际运行，无重启、无计数清零、无系统安装替换。测试目录为 `/usr/data/inkwars-test`；测试写屏期间暂时 SIGSTOP 已有 C1ancher 进程，脚本 trap 在结束时 SIGCONT 恢复它。单独的正常游戏状态／刷新预算目录为 `/usr/data/inkwars`。

`tools/device_test.py` 会执行：

1. 确认只有一台授权设备；推送静态游戏和测试二进制，脚本规范化 LF。
2. 在设备 CPU 上运行 19,063 项规则／存档断言和九回合 UI 闭环。
3. 使用真实 `/dev/epaper_lcd` 写屏层回放新建、生产、保存；启动第二个游戏进程读取存档。
4. 检查前后刷新计数和 fast-only；拉回 15 张设备生成 PBM，与主机逐字节比较。

已取得结果：规则及闭环通过；15 张主机／设备 PBM 全部相同；计数 **30→30**，`fast_refresh_only=1`；正常烟雾测试存档为 **2627 字节**。本轮带资源记录的写屏操作测试耗时 13.397 秒，内核报告峰值 RSS **808 KiB**，用户 CPU 55,355 微秒、系统 CPU 0 微秒。重载进程峰值 RSS 也是 808 KiB。最终运行以 `build/device-test.log` 为准。

这里的真机 UI 测试和显示测试使用程序提供的逻辑方向／确认／返回键回放，**不是物理手按测试，也不是面板照片验收**。预算已耗尽，只验证了这种状态下禁止额外全刷、快刷可用和进程重启不恢复预算。

Windows 复现（需设备已经授权 ADB）：

```powershell
py -3 tools/device_test.py --adb "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
```

当前脚本只部署自己的测试目录，并保持 C1ancher 运行；它不配置 ADB、不修改设备时钟、不改只读系统、不替换包签名信任根。通过之后测试程序退出并释放独占锁，**不会把测试进程留在前台**。请先正常退出其他直接显示应用，以免其硬件锁阻止测试启动。

## 已修复的集成问题

- 原地确认被规则移动 API 拒绝，导致连续占领及原地射击无法进入指令菜单：UI 现将原格确认作为不移动的指令入口。
- MIPS32 大 inode 文件系统重复保存失败：启用 64 位文件接口并增加跨文件系统回归。
- Windows 脚本 CRLF 导致 BusyBox `set -eu` 失败：部署／打包脚本统一换成 LF。
- 全刷后写屏截止时间变化可能导致延后帧被误认为已显示：保留 dirty 状态并使用运行层的下一安全写帧时间重试。
- SIGCONT 仅使运行层缓存失效却未通知 UI：增加明确重绘事件，不重置预算。
- 单人敌方回合错误地可能显示敌方视野：渲染固定使用甲方视野；热座另有交接页。
- 后勤指挥官生产价格显示与实际折扣不一致：生产菜单按实际九折显示。

完整未验证项目与功能缺项在 `KNOWN_ISSUES.md`。运行时模拟测试故意注入 SYN_DROPPED，因此日志中出现一次 “evdev queue overflow; waiting for SYN_REPORT” 是该测试的预期诊断，不是设备实测溢出。
