using System.Diagnostics;
using System.Text;

namespace C1SlimInstaller;

public sealed record CommandResult(int ExitCode, string Output);
public interface ICommandRunner
{
    Task<CommandResult> RunAsync(string file, IEnumerable<string> args, TimeSpan timeout, CancellationToken cancellation = default);
}
public sealed class CommandRunner : ICommandRunner
{
    public async Task<CommandResult> RunAsync(string file, IEnumerable<string> args, TimeSpan timeout, CancellationToken cancellation = default)
    {
        var info = new ProcessStartInfo(file) { UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8 };
        foreach (var argument in args) info.ArgumentList.Add(argument);
        using var process = new Process { StartInfo = info };
        process.Start();
        using var bounded = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        bounded.CancelAfter(timeout);
        var stdout = process.StandardOutput.ReadToEndAsync(bounded.Token);
        var stderr = process.StandardError.ReadToEndAsync(bounded.Token);
        // Bound both process exit and pipe reads: inherited/open pipes must not
        // leave the form waiting forever after the local command has exited.
        var completion = Task.WhenAll(process.WaitForExitAsync(bounded.Token), stdout, stderr);
        _ = completion.ContinueWith(task => { _ = task.Exception; },
            CancellationToken.None, TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        try { await completion.WaitAsync(bounded.Token); }
        catch (OperationCanceledException)
        {
            // Stop only this local client, never the shared ADB server or a device task.
            string cleanup = "";
            try
            {
                process.Kill(entireProcessTree: true);
                await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(3));
            }
            catch (InvalidOperationException) { }
            catch (Exception e) when (e is System.ComponentModel.Win32Exception or TimeoutException)
            { cleanup = " 本地进程清理未确认：" + e.Message; }
            throw new IOException("本地命令等待超时或已停止；设备端任务可能仍在运行，请重新检查，勿立即拔线。" + cleanup);
        }
        return new CommandResult(process.ExitCode, (await stdout) + (await stderr));
    }
}

public sealed record Device(string Serial, string State)
{
    public override string ToString() => Serial + "  [" + State + "]";
}
public sealed class AdbClient(string executable, ICommandRunner runner)
{
    public async Task<IReadOnlyList<Device>> DevicesAsync()
    {
        var result = await runner.RunAsync(executable, ["devices"], TimeSpan.FromSeconds(15));
        if (result.ExitCode != 0) throw new IOException("ADB 设备检测失败：" + result.Output);
        return ParseDevices(result.Output);
    }
    public static IReadOnlyList<Device> ParseDevices(string text) => text.Split('\n')
        .Select(line => line.Trim().Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries))
        .Where(fields => fields.Length >= 2 && fields[0] != "List" && fields[0] != "*" &&
            fields[1] is "device" or "offline" or "unauthorized" or "recovery" or "sideload")
        .Select(fields => new Device(fields[0], fields[1])).ToArray();
    public Task<CommandResult> RunAsync(string serial, IEnumerable<string> args, int seconds = 30)
    {
        if (string.IsNullOrWhiteSpace(serial) || serial.StartsWith('-') || serial.Any(char.IsControl))
            throw new IOException("设备序列号无效。");
        return runner.RunAsync(executable, new[] { "-s", serial }.Concat(args), TimeSpan.FromSeconds(seconds));
    }
    public async Task<string> ShellAsync(string serial, string command, int seconds = 30)
    {
        string marker = "__C1_SETUP_EXIT_" + Guid.NewGuid().ToString("N") + "=";
        var result = await RunAsync(serial, ["shell", "( " + command + " ); rc=$?; printf '\\n" + marker + "%s\\n' \"$rc\""], seconds);
        // Legacy device adbd plus Windows ADB can return CRCRLF, not just CRLF.
        // Normalize transport line endings before matching the per-command exit marker.
        string output = result.Output.Replace("\r\r\n", "\n").Replace("\r\n", "\n");
        var match = System.Text.RegularExpressions.Regex.Match(output,
            "(?m)^" + marker + "([0-9]+)\\r?$");
        if (result.ExitCode != 0 || !match.Success || match.Groups[1].Value != "0")
            throw new IOException("设备命令未成功（可能仍在后台运行）：\n" + result.Output);
        return output[..match.Index].Trim();
    }
    public async Task EnsureRootAsync(string serial, Action<string> log)
    {
        if (await ShellAsync(serial, "id -u") == "0") return;
        log("向所选设备请求 adb root，等待同一台设备重新连接。");
        var request = await RunAsync(serial, ["root"]);
        if (request.ExitCode != 0 || request.Output.Contains("cannot run as root", StringComparison.OrdinalIgnoreCase))
            throw new IOException("设备未开放 root ADB，请先完成设备授权 / 开发模式开启：" + request.Output);
        var deadline = DateTime.UtcNow.AddSeconds(45);
        while (DateTime.UtcNow < deadline)
        {
            await Task.Delay(1500);
            try { if (await ShellAsync(serial, "id -u", 5) == "0") return; }
            catch (IOException) { }
        }
        throw new IOException("同一设备未恢复 root ADB，安装停止；不会向其他设备发送命令。");
    }
    public async Task PushAsync(string serial, string local, string remote, string digest)
    {
        var result = await RunAsync(serial, ["push", local, remote], 180);
        if (result.ExitCode != 0) throw new IOException("上传失败：" + result.Output);
        var hash = await ShellAsync(serial, "sha256sum " + Quote(remote));
        if (!hash.StartsWith(digest + " ", StringComparison.OrdinalIgnoreCase)) throw new IOException("上传后校验不一致：" + remote);
    }
    public static string Quote(string text) => "'" + text.Replace("'", "'\"'\"'") + "'";
}
