using System.ComponentModel;
using System.Diagnostics;

namespace C1SlimInstaller;

internal sealed class AdbAdmitDialog : Form
{
    readonly TextBox script = new() { Dock = DockStyle.Fill, Name = "AdbAdmitScript" };
    readonly TextBox deviceIp = new() { Dock = DockStyle.Fill, Name = "AdbAdmitDeviceIp", PlaceholderText = "可留空；其他设备请填写热点列表中的当前 IPv4" };
    readonly Panel body = new() { Dock = DockStyle.Fill, AutoScroll = true, Name = "AdbAdmitBody" };
    readonly TableLayoutPanel layout = new() { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Padding = new Padding(20, 20, 20, 0), ColumnCount = 1, RowCount = 7 };
    readonly FlowLayoutPanel buttons = new() { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Dock = DockStyle.Bottom, FlowDirection = FlowDirection.RightToLeft, Padding = new Padding(20, 0, 20, 16), Name = "AdbAdmitActions" };
    bool fitting;
    int fittedWidth;
    internal string ScriptPath => script.Text.Trim();
    internal string DeviceIp => deviceIp.Text.Trim();

    internal AdbAdmitDialog(string initialScript)
    {
        Text = "开启 ADB · 先连接电脑热点";
        AutoScaleDimensions = new SizeF(96, 96); AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(760, 530); MinimumSize = SizeFromClientSize(new Size(680, 240));
        StartPosition = FormStartPosition.CenterParent; MinimizeBox = false; MaximizeBox = false;
        script.Text = initialScript;
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (int i = 0; i < 7; i++) layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.Controls.Add(new WrapLabel { Text = "1. 开启 Windows 移动热点，频带设为 2.4 GHz。\n2. 让 C1-Slim 连接这个电脑热点，并用数据线连接 USB。\n3. 准备好后点“确定，启动脚本”，允许管理员权限弹窗。\n4. 在新窗口看到 READY 后，打开设备“关于设备”的版本信息项目，5 秒内完整按下并松开 Enter 10 次。\n5. 等待 APPROVED 和清理完成，再回安装器查看 / 刷新 USB 设备。", Margin = new Padding(0, 0, 0, 12) }, 0, 0);
        var hotspot = new Button { Text = "打开 Windows 移动热点设置", AutoSize = true, Margin = new Padding(0, 0, 0, 12) };
        hotspot.Click += (_, _) => {
            try { Process.Start(new ProcessStartInfo("ms-settings:network-mobilehotspot") { UseShellExecute = true }); }
            catch (Exception e) { MessageBox.Show(this, "无法打开设置，请手动打开“设置 → 网络和 Internet → 移动热点”。\n" + e.Message); }
        };
        layout.Controls.Add(hotspot, 0, 1);
        layout.Controls.Add(new WrapLabel { Text = "可信的本地脚本（将以管理员权限运行；需要 Python 3.12、cryptography 和 pydivert 3.1.3）：" }, 0, 2);
        var row = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 2, Margin = new Padding(0, 4, 0, 10) };
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); row.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        var browse = new Button { Text = "选择脚本…", AutoSize = true };
        browse.Click += (_, _) => {
            using var picker = new OpenFileDialog { Title = "选择可信的 adb_admit_local.py", Filter = "Python 脚本 (*.py)|*.py", CheckFileExists = true, FileName = "adb_admit_local.py" };
            if (picker.ShowDialog(this) == DialogResult.OK) script.Text = picker.FileName;
        };
        row.Controls.Add(script, 0, 0); row.Controls.Add(browse, 1, 0); layout.Controls.Add(row, 0, 3);
        layout.Controls.Add(new WrapLabel { Text = "设备热点 IPv4（可选）：留空沿用脚本默认 MAC 58-C5-87-15-F1-49；换设备时请填当前 IP。" }, 0, 4);
        deviceIp.Margin = new Padding(0, 4, 0, 10); layout.Controls.Add(deviceIp, 0, 5);
        layout.Controls.Add(new WrapLabel { Text = "仅用于本人拥有或已获授权的设备。此功能只启动 ADB 放行脚本，不执行安装、删除或重启，不自动安装 Python 依赖。脚本可能记录含账号 / 设备标识的日志。停止时按 Ctrl+C，等待清理完成后再关闭终端。", Margin = new Padding(0, 0, 0, 12) }, 0, 6);
        var cancel = new Button { Text = "取消", DialogResult = DialogResult.Cancel, AutoSize = true };
        var ok = new Button { Text = "确定，启动脚本", AutoSize = true, Name = "LaunchAdbAdmit" };
        ok.Click += (_, _) => {
            try {
                // Validate before dismissing; this only constructs text, never executes the script.
                AdbAdmitLauncher.Command(ScriptPath, DeviceIp, Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData));
                DialogResult = DialogResult.OK;
            } catch (Exception e) { MessageBox.Show(this, e.Message, "请检查脚本和设备 IP", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
        };
        buttons.Controls.Add(cancel); buttons.Controls.Add(ok);
        body.Controls.Add(layout);
        Controls.Add(body); Controls.Add(buttons); CancelButton = cancel;
        Shown += (_, _) => FitContent();
        ClientSizeChanged += (_, _) => { if (Visible && ClientSize.Width != fittedWidth) FitContent(); };
        DpiChanged += (_, _) => { if (Visible) BeginInvoke(() => FitContent()); };
        // No default accept button: pressing Enter while entering an IP must not launch an elevated script.
    }

    internal void FitContent(int? maximumClientHeight = null)
    {
        if (fitting || IsDisposed) return;
        fitting = true;
        try
        {
            fittedWidth = ClientSize.Width;
            int maximum = maximumClientHeight ?? Math.Max(1, Screen.FromControl(this).WorkingArea.Height - (Height - ClientSize.Height) - (int)(24 * DeviceDpi / 96F));
            // Measure at the full width first so a previous scrollbar cannot keep the dialog unnecessarily tall.
            body.AutoScrollPosition = Point.Empty;
            int width = Math.Max(1, ClientSize.Width);
            layout.Width = width;
            int contentHeight = layout.GetPreferredSize(new Size(width, 0)).Height;
            int actionsHeight = buttons.GetPreferredSize(new Size(width, 0)).Height;
            ClientSize = new Size(width, Math.Min(contentHeight + actionsHeight, maximum));
            PerformLayout();
        }
        finally { fitting = false; }
    }
}

public sealed partial class MainForm
{
    readonly Button admit = new() { Text = "开启 ADB（热点辅助）", AutoSize = true, Name = "AdbAdmitButton" };
    Process? admitWindow;
    string admitScript = AdbAdmitLauncher.DefaultScript(AppContext.BaseDirectory);

    void LaunchAdbAdmit()
    {
        if (busy) return;
        busy = true; EnableControls();
        try
        {
            if (admitWindow != null && !admitWindow.HasExited)
            {
                MessageBox.Show(this, "ADB 辅助终端仍然打开。请在那个窗口查看进度；若要重新启动，请先按 Ctrl+C 并等待清理完成，再关闭旧终端。", "避免重复启动");
                return;
            }
            admitWindow?.Dispose(); admitWindow = null;
            using var dialog = new AdbAdmitDialog(admitScript);
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            admitScript = dialog.ScriptPath;
            var start = AdbAdmitLauncher.StartInfo(admitScript, dialog.DeviceIp,
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), Environment.SystemDirectory);
            admitWindow = Process.Start(start) ?? throw new IOException("未能创建管理员终端。 ");
            Write("已打开 ADB 热点辅助终端，脚本：" + admitScript);
            Write("请在终端等待 READY 后触发设备入口；启动终端不代表 ADB 已放行。此操作不会开始安装。 ");
        }
        catch (Win32Exception e) when (e.NativeErrorCode == 1223) { Write("已取消管理员授权，未启动 ADB 辅助脚本。 "); }
        catch (Exception e) { Write("ADB 辅助启动失败：" + e.Message); MessageBox.Show(this, e.Message, "ADB 辅助启动失败"); }
        finally { busy = false; EnableControls(); }
    }
}
