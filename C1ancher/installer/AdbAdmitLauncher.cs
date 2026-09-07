using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace C1SlimInstaller;

// Builds a visible, explicitly elevated launcher; never starts it during discovery or validation.
internal static class AdbAdmitLauncher
{
    internal static string DefaultScript(string baseDirectory) =>
        File.Exists(Path.Combine(baseDirectory, "adb_admit_local.py"))
            ? Path.Combine(baseDirectory, "adb_admit_local.py")
            : @"D:\c1slim\adb_admit_local.py";

    internal static string Literal(string value) => "'" + value.Replace("'", "''") + "'";

    internal static string NormalizeDeviceIp(string value)
    {
        value = value.Trim();
        if (value.Length == 0) return "";
        if (!IPAddress.TryParse(value, out var address) || address.AddressFamily != AddressFamily.InterNetwork
            || address.ToString() != value || IPAddress.IsLoopback(address)
            || address.GetAddressBytes()[0] is 0 or >= 224)
            throw new ArgumentException("设备 IP 必须是热点客户端列表中的完整 IPv4 地址，例如 192.168.137.231；也可以留空。 ");
        return value;
    }

    internal static string Command(string scriptPath, string deviceIp, string localAppData)
    {
        string script = Path.GetFullPath(scriptPath);
        if (!File.Exists(script) || !string.Equals(Path.GetExtension(script), ".py", StringComparison.OrdinalIgnoreCase))
            throw new IOException("请选择存在的、可信的 adb_admit_local.py 脚本。 ");
        string ip = NormalizeDeviceIp(deviceIp);
        string python = Path.Combine(localAppData, "Programs", "Python", "Python312", "python.exe");
        return """
            $ErrorActionPreference = 'Stop'
            [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
            $env:PYTHONIOENCODING = 'utf-8'
            $Host.UI.RawUI.WindowTitle = 'C1-Slim ADB hotspot helper'
            Write-Host '等待 READY 后，在设备的“关于设备”版本信息项目上，5 秒内完整按下并松开 Enter 10 次。'
            Write-Host '停止时按 Ctrl+C 并等待清理完成；请勿直接强制结束进程。'
            Write-Host '本窗口独立运行；启动脚本不代表 ADB 已开启，也不会开始安装。'
            try {
            """ + "\n$script = " + Literal(script) + "\n$python = " + Literal(python) + "\n" + """
                if (!(Test-Path -LiteralPath $script -PathType Leaf)) { throw 'ADB 脚本已移动或删除，请重新选择。' }
                Set-Location -LiteralPath (Split-Path -LiteralPath $script)
                $pythonArgs = @()
                if (!(Test-Path -LiteralPath $python -PathType Leaf)) {
                    $launcher = Get-Command py.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
                    if (!$launcher) { throw '未找到 Python 3.12。请安装 Python 3.12 和依赖后重试。' }
                    $python = $launcher.Source
                    $pythonArgs = @('-3.12')
                }
            """ + "\n$scriptArgs = @($script)\n" + (ip.Length == 0 ? "" : "$scriptArgs += @('--device-ip', " + Literal(ip) + ")\n") + """
                & $python @pythonArgs @scriptArgs
                Write-Host ('脚本已返回，退出码：' + $LASTEXITCODE + '。请检查 APPROVED 和 cleanup_complete=true，再回安装器刷新 USB 设备。')
            } catch {
                Write-Host ('启动失败：' + $_.Exception.Message) -ForegroundColor Red
            }
            Write-Host '依赖安装命令（仅在缺少依赖时手动执行）：py -3.12 -m pip install cryptography pydivert==3.1.3'
            Write-Host '日志位于脚本同目录的 adb-admit-plaintext.log，可能含账号和设备标识，请勿直接分享。'
            Write-Host '确认脚本清理结束后，可以关闭此窗口。'
            """;
    }

    internal static ProcessStartInfo StartInfo(string scriptPath, string deviceIp, string localAppData, string systemDirectory)
    {
        string encoded = Convert.ToBase64String(Encoding.Unicode.GetBytes(Command(scriptPath, deviceIp, localAppData)));
        return new ProcessStartInfo(Path.Combine(systemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe"))
        {
            UseShellExecute = true,
            Verb = "runas",
            // Only a base64-encoded command is placed in native arguments. User paths stay PowerShell literals.
            Arguments = "-NoLogo -NoProfile -NoExit -EncodedCommand " + encoded,
            WindowStyle = ProcessWindowStyle.Normal,
            WorkingDirectory = Path.GetDirectoryName(Path.GetFullPath(scriptPath))!
        };
    }
}
