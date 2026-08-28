# 功耗与休眠验证

C1ancher 的自动休眠默认保持禁用，直到设备完成物理唤醒验证。禁用开关是 `/usr/data/c1/disable-auto-suspend`；文件存在时，应用仍使用低唤醒事件循环和按变化显示，但不会写 `/sys/power/state`。

## 1. 建立基线

设备通过 root ADB 连接后运行：

```powershell
.\scripts\test-power-device.ps1 -DurationSeconds 3600 -IntervalSeconds 10
```

结果保存在 `artifacts/power-<timestamp>/`。至少记录锁屏静置、首页 Wi-Fi 关闭、首页 Wi-Fi 连接和终端静置四种场景。可靠电流节点或外置电流计下，每个场景预热 10 分钟、采样 60 分钟并重复三组；只有电量百分比时，单组至少持续 8 小时或下降 5%。

## 2. 验证物理唤醒

单次探测会写 `mem`，ADB 可能立即断开。操作前保证物理电源键可用，并准备设备恢复手段：

```powershell
.\scripts\test-power-device.ps1 -SuspendProbe -ConfirmSuspend
```

按物理电源键唤醒设备，确认 ADB 在超时内恢复。探测会在休眠前主动停止 ADB，并在唤醒后重新启动 ADB；只有完整恢复成功才会写入 `/usr/data/c1/suspend-probe-passed` 证明文件。单次成功后执行至少 20 次初步循环；正式启用前执行 100 次：

```powershell
.\scripts\test-power-device.ps1 -SuspendCycles 20 -ConfirmSuspend
.\scripts\test-power-device.ps1 -SuspendCycles 100 -ConfirmSuspend -ReconnectTimeoutSeconds 300
```

任一循环失败都会先清除证明文件、停止启用流程，并保留对应 `artifacts/power-<timestamp>/failure.txt` 和逐轮记录。重新成功完成整组循环后才会重建证明文件。

## 3. 启用或回退

完成唤醒验证后，安装时显式启用：

```powershell
.\scripts\install-default-app.ps1 -Action Install -EnableAutoSuspend -Reboot
.\scripts\install-default-app.ps1 -Action Verify -EnableAutoSuspend
```

安装器会强制检查 `/usr/data/c1/suspend-probe-passed` 中的成功结果，同时检查 `/sys/power/state` 提供 `mem`、节点可写且内核至少存在一个已启用的输入唤醒源。缺少成功的物理按键休眠与 ADB 重连探测时，`-EnableAutoSuspend` 会直接失败。

立即回退为“仅低唤醒、不自动休眠”可执行：

```powershell
.\scripts\install-default-app.ps1 -Action Install -Reboot
```

也可从 root shell 创建 `/usr/data/c1/disable-auto-suspend`，然后重启 C1ancher。

## 4. 运行时验收

- 首页 `OK` 或物理电源键进入锁屏；其他页面的电源键保留原用途。
- 非首页连续 5 分钟无输入后回首页并锁屏；锁屏 30 秒后请求 `mem`。
- 唤醒后仍显示锁屏页，首次唤醒键按下和释放不会穿透为解锁。
- Wi-Fi、ADB、SSH 和终端按休眠前启用状态恢复。
- 不变画面不写入电子纸；结构化日志中的 `display.frame_write` 记录包含 `unchanged_skipped`。
- 休眠失败后保持锁屏，并至少等待 60 秒再重试，避免错误循环。
- 24 小时运行期间应用不重启、内存不持续增长、根文件系统保持只读。

验收目标是锁屏无周期显示写、静置平均功耗至少下降 10%，且任一代表场景不得劣化超过 5%。