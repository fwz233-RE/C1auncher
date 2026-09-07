using System.Security.Cryptography;
using System.Text;

namespace C1SlimInstaller;

public sealed class InstallerEngine(Payload payload, AdbClient adb, Action<string> log, string evidence)
{
    // Production uses real time; offline tests can advance polling without weakening any gate.
    internal TimeProvider Clock { get; init; } = TimeProvider.System;
    static Guid BootIdentity(string text)
    {
        if (!System.Text.RegularExpressions.Regex.IsMatch(text, @"\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\z") ||
            !Guid.TryParseExact(text, "D", out var identity) || identity == Guid.Empty)
            throw new IOException("设备启动编号无效，重启验收已停止：" + text);
        return identity;
    }
    static readonly string[] EnrollmentDirectories = ["enrollment", "enrollment/release", "enrollment/release/artifacts"];
    static readonly string[] EnrollmentFiles = [
        "enrollment/bootstrap.v1", "enrollment/bootstrap.v1.sig", "enrollment/core.ed25519.pem", "enrollment/core.ed25519.pub",
        "enrollment/app-daemon-bootstrap.sh", "enrollment/device-core-enroll.sh", "enrollment/enroll.sh",
        "enrollment/release/manifest.v1", "enrollment/release/manifest.v1.sig", "enrollment/release/artifacts/C1ancher",
        "enrollment/release/artifacts/c1pkg", "enrollment/release/artifacts/C1ancher-launcher", "enrollment/release/artifacts/c1updater"
    ];
    // Only the current generated staging tree is eligible; never accept a UI or stored enrollment path.
    internal static string EnrollmentMetadataCommand(string remote)
    {
        if (remote == null || !System.Text.RegularExpressions.Regex.IsMatch(remote, @"\A/storage/c1-installer-[a-f0-9]{32}\z"))
            throw new ArgumentException("Invalid generated installer staging path.", nameof(remote));
        // Fixed lists and short loops keep the shell OPEN request within old adbd's 4 KiB budget.
        // Every loop body explicitly exits on failure; no command relies on shell errexit.
        string command = ": c1-enrollment-metadata-v1 && c1_root=" + AdbClient.Quote(remote)
            + " && c1_dirs=" + AdbClient.Quote(string.Join(" ", EnrollmentDirectories))
            + " && c1_files=" + AdbClient.Quote(string.Join(" ", EnrollmentFiles))
            + " && for c1_p in $c1_dirs; do test ! -L \"$c1_root/$c1_p\" && test -d \"$c1_root/$c1_p\" || exit 1; done"
            + " && for c1_p in $c1_files; do test ! -L \"$c1_root/$c1_p\" && test -f \"$c1_root/$c1_p\" && c1_meta=$(stat -c '%h' \"$c1_root/$c1_p\") && test \"$c1_meta\" = '1' || exit 1; done"
            + " && for c1_p in $c1_dirs; do chown 0:0 \"$c1_root/$c1_p\" && chmod 700 \"$c1_root/$c1_p\" || exit 1; done"
            + " && for c1_p in $c1_files; do chown 0:0 \"$c1_root/$c1_p\" && chmod 600 \"$c1_root/$c1_p\" || exit 1; done"
            + " && for c1_p in $c1_dirs; do c1_meta=$(stat -c '%a:%u:%g' \"$c1_root/$c1_p\") && test \"$c1_meta\" = '700:0:0' || exit 1; done"
            + " && for c1_p in $c1_files; do c1_meta=$(stat -c '%a:%u:%g' \"$c1_root/$c1_p\") && test \"$c1_meta\" = '600:0:0' || exit 1; done";
        if (Encoding.UTF8.GetByteCount(command) > 3500) throw new IOException("核心权限检查命令超过旧版 ADB 安全长度。");
        return command;
    }
    public async Task<string> ProbeAsync(string serial)
    {
        await adb.EnsureRootAsync(serial, log);
        string product = await adb.ShellAsync(serial, "cat /sys/kernel/config/usb_gadget/demo/strings/0x409/product; uname -m");
        if (!product.Split('\n').Any(x => x.Trim().Equals("mp-d261", StringComparison.OrdinalIgnoreCase)) || !product.Contains("mips", StringComparison.OrdinalIgnoreCase))
            throw new IOException("设备型号不匹配：仅支持已知 MP-D261 / C1-Slim MIPS 固件。");
        string mounts = await adb.ShellAsync(serial, "cat /proc/mounts");
        if (!mounts.Split('\n').Any(line => { var f = line.Split(' '); return f.Length > 3 && f[1] == "/" && f[3].Split(',').Contains("ro"); }))
            throw new IOException("设备根文件系统必须以只读状态开始。");
        string usbHash = FirstWord(await adb.ShellAsync(serial, "sha256sum /etc/init.d/S90usb"));
        if (usbHash != Payload.OriginalUsbHash && usbHash != Payload.Hash(payload.FileAt("usb/S90usb.open"))) throw new IOException("未知 USB 启动脚本，拒绝覆盖。");
        string daemon = FirstWord(await adb.ShellAsync(serial, "sha256sum /etc/app_daemon"));
        if (daemon != Payload.OriginalDaemonHash && daemon != payload.BootstrapHash) throw new IOException("未知主页启动脚本，拒绝覆盖。已有旧版系统请使用专门维护流程。");
        return product.Trim();
    }
    static string FirstWord(string text) => text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries)[0];
    async Task UploadAsync(string serial, string relative, string remote)
    {
        string local = payload.FileAt(relative);
        long size = new FileInfo(local).Length;
        log($"上传并校验：{relative}（{size:N0} 字节）。");
        var watch = System.Diagnostics.Stopwatch.StartNew();
        var transfer = adb.PushAsync(serial, local, remote, Payload.Hash(local));
        while (await Task.WhenAny(transfer, Task.Delay(TimeSpan.FromSeconds(5))) != transfer)
            log($"等待 ADB 上传/校验：{relative}，已等待 {watch.Elapsed.TotalSeconds:F0} 秒；尚未收到完成确认。");
        try { await transfer; }
        catch (IOException e)
        {
            throw new IOException($"文件传输或校验失败：{relative}（{size:N0} 字节，{watch.Elapsed.TotalSeconds:F0} 秒）。本次已停止，不会自动重试。请恢复同一设备的 USB / ADB 连接后查看日志。\n" + e.Message, e);
        }
        log($"上传校验完成：{relative}（{watch.Elapsed.TotalSeconds:F1} 秒）。");
    }
    async Task StepAsync(string serial, string command, int timeout = 60)
    {
        string result = await adb.ShellAsync(serial, command, timeout);
        if (result.Length > 0) log(result);
    }
    public async Task InstallAsync(string serial, bool reboot)
    {
        string locks = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "C1SlimInstaller", "locks");
        Directory.CreateDirectory(locks);
        string lockName = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(serial)));
        using var lease = new FileStream(Path.Combine(locks, lockName + ".lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        // Fail before any remote modification if another installer in this account owns the device.
        payload.Validate();
        log("1/8 检查所选设备和安装包。");
        string baseline = await ProbeAsync(serial);
        Directory.CreateDirectory(evidence);
        await File.WriteAllTextAsync(Path.Combine(evidence, "baseline.txt"), baseline);
        string remote = "/storage/c1-installer-" + Guid.NewGuid().ToString("N");
        string setup = AdbClient.Quote(remote + "/device-setup.sh");
        string invoke = "sh " + setup + " ";
        string suspendIdentity = " " + Payload.Hash(payload.FileAt("enrollment/release/manifest.v1"))
            + " " + Payload.Hash(payload.FileAt("enrollment/release/artifacts/c1pkg"));
        async Task Helper(string command, int timeout = 60)
        {
            string result = await adb.ShellAsync(serial, invoke + command, timeout);
            string expected = "C1SETUP_OK " + command.Split(' ')[0];
            if (result.Split('\n').Count(line => line.TrimEnd('\r') == expected) != 1)
                throw new IOException("设备辅助步骤没有返回唯一成功标记：" + command + "\n" + result);
            log(result);
        }
        // All remote staging paths are generated, never derived from UI strings.
        await StepAsync(serial, "test ! -L /storage && mkdir -m 700 " + AdbClient.Quote(remote));
        try
        {
            log("2/8 上传并逐文件校验核心、仓库配置及设备辅助脚本（不包含普通应用）。");
            var remoteFiles = payload.Files.Where(p => p.StartsWith("enrollment/") || p.StartsWith("profile/") || p.StartsWith("accessories/") || p == "device-setup.sh" || p.StartsWith("usb/")).ToArray();
            int fileNumber = 0;
            foreach (string relative in remoteFiles)
            {
                log($"2/8 文件 {++fileNumber}/{remoteFiles.Length}：{relative}");
                string target = remote + "/" + relative;
                await StepAsync(serial, "mkdir -p " + AdbClient.Quote(target[..target.LastIndexOf('/')]));
                await UploadAsync(serial, relative, target);
            }
            log("检查全部已上传安装脚本语法；通过后才开始设备配置。");
            foreach (string relative in remoteFiles.Where(name => name.EndsWith(".sh", StringComparison.Ordinal) || name == "usb/S90usb.open"))
            {
                log("设备 shell 语法检查：" + relative);
                await StepAsync(serial, "sh -n " + AdbClient.Quote(remote + "/" + relative));
            }
            log("准备本次核心 enrollment 安全权限：3 个目录 700、13 个文件 600，属主和组均为 root；先检查全部类型与链接。");
            await StepAsync(serial, EnrollmentMetadataCommand(remote));
            log("核心 enrollment 权限准备及逐项 stat 检查成功；重新校验全部 13 个文件 SHA-256。");
            foreach (string relative in EnrollmentFiles)
                if (FirstWord(await adb.ShellAsync(serial, "sha256sum " + AdbClient.Quote(remote + "/" + relative))) != Payload.Hash(payload.FileAt(relative)))
                    throw new IOException("核心 enrollment 权限准备后文件校验不一致：" + relative);
            log("核心 enrollment 安全权限准备成功，13 个文件 SHA-256 均与安装包一致。");
            await Helper("preflight");
            await Helper("prepare");
            string existing = await adb.ShellAsync(serial, "if [ -e /usr/data/c1/update/enrolled.v1 ]; then echo enrolled; else echo factory; fi");
            if (existing == "enrolled")
            {
                string digest = FirstWord(await adb.ShellAsync(serial, "sha256sum /usr/data/c1/core/current/manifest.v1"));
                if (digest != Payload.Hash(payload.FileAt("enrollment/release/manifest.v1")))
                    throw new IOException("设备已有其他核心版本。安装器不会重置更新历史，请从设备首页更新或使用专门维护流程。");
                string key = FirstWord(await adb.ShellAsync(serial, "sha256sum /etc/c1updater/core.ed25519.pub"));
                if (key != payload.CoreKeyHash) throw new IOException("设备核心公钥与安装包不同，拒绝更换信任密钥。");
            }
            log("3/8 安装开机 ADB 支持；不修改设备认证密钥。");
            await UploadAsync(serial, "usb/S90usb.open", "/dev/shm/c1-S90usb.open-root-adb");
            await StepAsync(serial, "sh " + AdbClient.Quote(remote + "/usb/device-open-adb.sh") + " install " + Payload.OriginalUsbHash + " " + Payload.Hash(payload.FileAt("usb/S90usb.open")));
            log("4/8 安装默认壁纸与终端信息工具；已有壁纸保留。");
            await Helper("accessories " + AdbClient.Quote(remote + "/accessories"), 90);
            log("5/8 安装签名核心、启动保活和可信更新恢复组件。");
            if (existing != "enrolled")
            {
                string bundle = remote + "/enrollment";
                await StepAsync(serial, "chmod 700 " + AdbClient.Quote(bundle + "/device-core-enroll.sh") + " " + AdbClient.Quote(bundle + "/enroll.sh") + " && " + AdbClient.Quote(bundle + "/device-core-enroll.sh") + " install " + AdbClient.Quote(bundle), 90);
                var deadline = Clock.GetUtcNow().AddMinutes(6);
                bool completed = false;
                while (Clock.GetUtcNow() < deadline)
                {
                    await Task.Delay(TimeSpan.FromMilliseconds(2500), Clock);
                    string progress = await adb.ShellAsync(serial, "if [ -e /storage/c1/update/enrollment/apply.pid ]; then echo running; elif [ -f /usr/data/c1/update/enrolled.v1 ] && grep -q 'core enrollment completed' /storage/c1/update/enrollment/apply.log; then echo complete; else echo failed; fi");
                    if (progress == "complete") { completed = true; break; }
                    if (progress == "failed") throw new IOException("核心安装后台任务失败。请保留设备和日志，勿重复强制刷写。");
                }
                if (!completed) throw new IOException("核心安装等待超时，任务可能仍在后台运行。请检查日志再继续。");
            }
            // Verify with the authenticated script uploaded for this attempt; historical staging may be absent.
            await StepAsync(serial, "sh " + AdbClient.Quote(remote + "/enrollment/device-core-enroll.sh") + " verify", 90);
            await Helper("start-core", 90);
            // Verify the actual running image, not only a marker or an executable file.
            await VerifyRunningAsync(serial, "before-removal");
            log("配套签名核心和运行状态已核验；开启深度休眠并检查开关。已有禁用设置将被改为启用，失败即停止安装。");
            await Helper("enable-suspend" + suspendIdentity);
            await Helper("verify-suspend" + suspendIdentity);
            log("6/8 配置普通应用仓库和核心更新地址；不下载或安装普通应用。");
            await StepAsync(serial, "sh " + AdbClient.Quote(remote + "/profile/device-repository-config.sh") + " " + AdbClient.Quote(remote + "/profile"));
            log("复制 Windows / Linux 发布工具、说明和公开仓库配置到设备存储。");
            string developerPath = await InstallDeveloperToolsAsync(serial);
            log("开发工具目录：" + developerPath);
            log("7/8 新核心已通过检查，直接删除原厂学习软件运行目录；本次不创建原厂备份。");
            await Helper("remove-factory", 180);
            await Helper("verify", 120);
            await VerifyRunningAsync(serial, "after-removal");
            await VerifyRepositoriesAsync(serial);
            if (reboot)
            {
                log("8/8 重启所选设备并检查同一序列号、启动编号与核心状态。");
                Guid boot = BootIdentity(await adb.ShellAsync(serial, "cat /proc/sys/kernel/random/boot_id"));
                var rebootResult = await adb.RunAsync(serial, ["reboot"]);
                if (rebootResult.ExitCode != 0)
                    throw new IOException("ADB 重启命令失败（退出码 " + rebootResult.ExitCode + "），不会继续等待重启：\n" + rebootResult.Output);
                var deadline = Clock.GetUtcNow().AddMinutes(5);
                bool returned = false;
                while (Clock.GetUtcNow() < deadline)
                {
                    await Task.Delay(TimeSpan.FromSeconds(3), Clock);
                    string next;
                    try { next = await adb.ShellAsync(serial, "cat /proc/sys/kernel/random/boot_id", 10); }
                    catch (IOException) { continue; }
                    // A transport failure is retryable; a successful but malformed identity is not.
                    if (BootIdentity(next) != boot) { returned = true; break; }
                }
                if (!returned) throw new IOException("重启后设备未在时限内恢复。安装尚未通过重启验收，勿标记成功。");
                await Helper("verify", 120);
                await VerifyRunningAsync(serial, "after-reboot");
                await VerifyRepositoriesAsync(serial);
                await StepAsync(serial, "sh " + AdbClient.Quote(remote + "/usb/device-open-adb.sh") + " verify " + Payload.OriginalUsbHash + " " + Payload.Hash(payload.FileAt("usb/S90usb.open")));
            }
            log("最终检查深度休眠开关；检查通过后才记录安装成功。");
            await Helper("verify-suspend" + suspendIdentity);
            await StepAsync(serial, "rm -rf " + AdbClient.Quote(remote));
            await File.WriteAllTextAsync(Path.Combine(evidence, "result.txt"), "serial=" + serial + "\nversion=" + payload.Version + "\nsequence=" + payload.Sequence + "\nfactory_backup_created=false\nfactory_removal=direct\nautomatic_suspend=enabled\nautomatic_suspend_verified=true\nreboot_verified=" + reboot + "\n");
            log(reboot
                ? "安装完成：同一台设备已重启、核心运行检查通过，深度休眠开关已确认开启。请人工检查屏幕和实体按键。"
                : "安装已写入并验证运行状态，深度休眠开关已确认开启；未重启，重启仍待验收。");
        }
        catch (Exception failure)
        {
            log("电脑端安装流程已停止，不会继续执行后续步骤：" + failure.Message);
            log("保留设备临时安装目录供恢复：" + remote);
            try
            {
                var detail = await adb.ShellAsync(serial, "if [ -f /storage/c1/update/enrollment/apply.log ]; then tail -c 16384 /storage/c1/update/enrollment/apply.log; fi", 5);
                await File.WriteAllTextAsync(Path.Combine(evidence, "device-apply.log"), detail);
                log(detail);
            }
            catch (Exception error) { log("未能读取设备日志：" + error.Message); }
            throw;
        }
    }
    public async Task<string> InstallDeveloperToolsAsync(string serial)
    {
        var files = payload.Files.Where(name => name.StartsWith("developer/", StringComparison.Ordinal)).Order().ToArray();
        string identity = string.Join("\n", files.Select(name => name + ":" + Payload.Hash(payload.FileAt(name))));
        string digest = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(identity))).ToLowerInvariant();
        const string root = "/storage/mtp/C1DeveloperTools";
        foreach (string parent in new[] { "/storage", "/storage/mtp", root })
            await StepAsync(serial, "test ! -L " + AdbClient.Quote(parent) + " && { test ! -e " + AdbClient.Quote(parent) + " || test -d " + AdbClient.Quote(parent) + "; } && mkdir -p " + AdbClient.Quote(parent));
        string destination = root + "/" + digest;
        string existing = await adb.ShellAsync(serial, "test ! -L " + AdbClient.Quote(destination) + " && if [ -d " + AdbClient.Quote(destination) + " ]; then echo existing; else test ! -e " + AdbClient.Quote(destination) + " && echo new; fi");
        if (existing == "new")
        {
            string draft = root + "/.incoming-" + Guid.NewGuid().ToString("N");
            await StepAsync(serial, "mkdir " + AdbClient.Quote(draft));
            foreach (string relative in files)
            {
                string target = draft + "/" + relative["developer/".Length..];
                await StepAsync(serial, "mkdir -p " + AdbClient.Quote(target[..target.LastIndexOf('/')]));
                await UploadAsync(serial, relative, target);
            }
            await StepAsync(serial, "sync && test ! -e " + AdbClient.Quote(destination) + " && test ! -L " + AdbClient.Quote(destination) + " && mv " + AdbClient.Quote(draft) + " " + AdbClient.Quote(destination));
        }
        else if (existing != "existing") throw new IOException("开发工具目录检查返回无效状态。");
        foreach (string relative in files)
        {
            string target = destination + "/" + relative["developer/".Length..];
            await StepAsync(serial, "test ! -L " + AdbClient.Quote(target) + " && test -f " + AdbClient.Quote(target));
            if (FirstWord(await adb.ShellAsync(serial, "sha256sum " + AdbClient.Quote(target))) != Payload.Hash(payload.FileAt(relative)))
                throw new IOException("设备上的开发工具校验失败，保留已有文件：" + target);
        }
        return destination;
    }
    async Task VerifyRepositoriesAsync(string serial)
    {
        foreach (var item in new[] { ("profile/repository.ed25519.pub", "/usr/data/c1/pkg/repository.ed25519.pub"), ("profile/repository.url", "/usr/data/c1/pkg/repository.url"), ("profile/core-repository.url", "/usr/data/c1/update/repository.url") })
            if (FirstWord(await adb.ShellAsync(serial, "sha256sum " + AdbClient.Quote(item.Item2))) != Payload.Hash(payload.FileAt(item.Item1))) throw new IOException("仓库配置检查失败。");
    }
    async Task VerifyRunningAsync(string serial, string stage)
    {
        string expected = Payload.Hash(payload.FileAt("enrollment/release/artifacts/C1ancher"));
        string launcher = Payload.Hash(payload.FileAt("enrollment/release/artifacts/C1ancher-launcher"));
        var deadline = Clock.GetUtcNow().AddSeconds(60);
        string previous = "", snapshot = "", reason = "尚无运行快照";
        log("核验实际主页及启动器进程关系（" + stage + "）；允许已核验主页的同镜像派生进程。 ");
        while (Clock.GetUtcNow() < deadline)
        {
            snapshot = await adb.ShellAsync(serial, RunningProcessProbe.Command);
            var observed = RunningProcessProbe.Analyze(snapshot, expected, launcher);
            reason = observed.Reason;
            if (observed.Valid && observed.Identity == previous)
            {
                log("运行镜像和进程关系连续两次核验通过：" + reason);
                return;
            }
            if (observed.Valid) reason = "主页身份尚未连续两次保持一致：" + observed.Reason;
            previous = observed.Valid ? observed.Identity : "";
            await Task.Delay(TimeSpan.FromSeconds(2), Clock);
        }
        string detail = "阶段=" + stage + "\n原因=" + reason + "\n主页预期 SHA256=" + expected
            + "\n启动器预期 SHA256=" + launcher + "\n" + snapshot;
        log("运行核验失败：" + reason + "\n最后一次只读进程快照：\n" + snapshot);
        try { await File.WriteAllTextAsync(Path.Combine(evidence, "running-process-" + stage + ".txt"), detail); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { log("进程诊断记录写入失败：" + e.Message); }
        throw new IOException("没有检测到唯一且匹配安装包的主页主进程及可信启动器（" + stage + "）：" + reason + "。后续步骤已停止。");
    }
}
