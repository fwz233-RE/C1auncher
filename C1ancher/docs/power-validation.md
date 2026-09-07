# 功耗与休眠验证

C1ancher 首次安装仅在受支持的 `ingenic,halley6_v20` 硬件上默认启用自动深度休眠；需要 `mpenbatt` 平台设备、已启用的 `gpio_keys` 唤醒节点，以及可读写且提供 `mem` 的 `/sys/power/state`。Windows EXE 安装助手与 `install-default-app.ps1` 使用相同条件。重装或升级保留已有的明确禁用设置，包括空文件与旧安装器写入的标记；不受支持的硬件始终写入或保留禁用标记。

禁用开关是 `/usr/data/c1/disable-auto-suspend`；文件存在时，应用仍使用低唤醒事件循环和按变化显示，但不会写 `/sys/power/state`。

## USB 使用约定（2026-09-07）

深度休眠以省电为目标，不要求唤醒后电脑自动重连。设备只恢复休眠前启用的 ADB/MTP 服务，并补回暂停 ADB 时丢失的 USB 控制器绑定；已有绑定不主动断开重连。不添加打开 Wi-Fi 时重启 USB 的联动，也不以电脑未确认作为整套 USB 服务重启的触发条件。设备侧服务就绪不等于电脑已识别，用户可在短按电源键唤醒后重新拔插 USB。

简化版生产 USB 处理已于 2026-09-07 通过一次实机探测：用户确认短按唤醒并拔插，内核休眠成功计数从 1 增至 2，启动编号不变，ADB 命令可执行，测试未配置备用 USB 重启。证据位于 `artifacts/manual-usb-suspend-20260907-a/`。该结果是单次 USB 生命周期探测，不是完整签名核心的长期运行、自动休眠或续航验收；安装包交付仍需匹配同一实现和完整核心测试。

## 1. 建立基线

设备通过 root ADB 连接后运行：

```powershell
.\scripts\test-power-device.ps1 -DurationSeconds 3600 -IntervalSeconds 10
```

结果保存在 `artifacts/power-<timestamp>/`。至少记录锁屏静置、首页 Wi-Fi 关闭、首页 Wi-Fi 连接和终端静置四种场景。可靠电流节点或外置电流计下，每个场景预热 10 分钟、采样 60 分钟并重复三组；只有电量百分比时，单组至少持续 8 小时或下降 5%。

## 2. 验证物理唤醒

当前手动重连验收使用 `tests/device_usb_suspend_probe.c` 链接生产 `usb_power.c` 与 `liveness.c` 构建 MIPS 静态探测程序，再由 `scripts/test-manual-usb-suspend.py` 执行。`--start --confirm-suspend --binary <探测程序>` 要求人在设备旁；同时提供 `--adb`、`--serial` 和不存在的 `--record` 目录。探测不改永久设置，也没有自动 USB 服务重启或主机确认超时回退。短按电源键、等约 5 秒并重新拔插后，以同一记录目录运行 `--collect`。收集会验证启动编号未变、成功计数恰好增加 1、探测程序成功退出及永久设置不变；物理拔插由操作员单独确认。收集失败可待连接后重试，不能据此伪造通过记录。

以下旧 `test-power-device.ps1` 是历史自动重连探测，不代表上述简化版生产路径，也不能用于证明新版免拔插恢复；其证明文件不作为安装启用的门禁。

单次探测会写 `mem`，ADB 可能立即断开。操作前保证物理电源键可用，并准备设备恢复手段：

```powershell
.\scripts\test-power-device.ps1 -SuspendProbe -ConfirmSuspend
```

按物理电源键唤醒设备，确认 ADB 在超时内恢复。探测会在休眠前主动停止 ADB，并在唤醒后重新启动 ADB；只有完整恢复成功才会写入 `/usr/data/c1/suspend-probe-passed` 证明文件。单次成功后执行至少 20 次初步循环；正式交付前执行 100 次：

```powershell
.\scripts\test-power-device.ps1 -SuspendCycles 20 -ConfirmSuspend
.\scripts\test-power-device.ps1 -SuspendCycles 100 -ConfirmSuspend -ReconnectTimeoutSeconds 300
```

任一循环失败都会先清除证明文件、停止启用流程，并保留对应 `artifacts/power-<timestamp>/failure.txt` 和逐轮记录。重新成功完成整组循环后才会重建证明文件。

## 3. 启用或回退

首次安装无需额外参数即可按硬件能力选择默认值。已有禁用设置需要用户明确要求才会清除：

```powershell
.\scripts\install-default-app.ps1 -Action Install -EnableAutoSuspend -Reboot
.\scripts\install-default-app.ps1 -Action Verify -EnableAutoSuspend
```

安装器与设备助手检查上述精确硬件条件；不受支持的硬件使用 `-EnableAutoSuspend` 会失败。安装器不再要求 `/usr/data/c1/suspend-probe-passed`，也不会创建物理验证成功证明。`Verify` 只检查当前设置，不改变偏好。默认重装保留明确禁用，只有 `-EnableAutoSuspend` 才覆盖它；`-EnableAutoSuspend` 与 `-DisableAutoSuspend` 不能同时使用。

立即回退为“仅低唤醒、不自动休眠”可执行：

```powershell
.\scripts\install-default-app.ps1 -Action Install -DisableAutoSuspend -Reboot
```

也可从 root shell 创建 `/usr/data/c1/disable-auto-suspend`，然后重启 C1ancher。

## 4. 运行时验收

- 首页 `OK` 或物理电源键进入锁屏；其他页面的电源键保留原用途。
- 非首页连续 5 分钟无输入后回首页并锁屏；只有 AC 与 USB 状态均可读取且持续离线满 20 秒、同时锁屏也已满 20 秒后，才请求 `mem`。
- AC 或 USB 任一在线时禁止自动休眠；任一电源状态无法读取时按未知处理并禁止休眠。重新拔掉外部电源后必须重新计满 20 秒。
- 唤醒后仍显示锁屏页，首次唤醒键按下和释放不会穿透为解锁。
- Wi-Fi、ADB 和终端按休眠前启用状态恢复；终端内运行的 APP TUI 或已启动应用随前台进程一起暂停和恢复。
- 不变画面不写入电子纸；结构化日志中的 `display.frame_write` 记录包含 `unchanged_skipped`。
- 休眠失败后保持锁屏，并至少等待 60 秒再重试，避免错误循环。
- 24 小时运行期间应用不重启、内存不持续增长、根文件系统保持只读。

验收目标是锁屏无周期显示写、静置平均功耗至少下降 10%，且任一代表场景不得劣化超过 5%。