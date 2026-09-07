using System.Diagnostics;

namespace C1SlimInstaller;

public sealed partial class MainForm : Form
{
    readonly TextBox folder = new() { Dock = DockStyle.Fill, Name = "PayloadFolder" };
    readonly TextBox logBox = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, WordWrap = true, Dock = DockStyle.Fill, BorderStyle = BorderStyle.None, Name = "InstallLog" };
    readonly ComboBox devices = new() { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill, Name = "DeviceSelector", IntegralHeight = false, DropDownHeight = 240 };
    readonly Button scan = new() { Text = "刷新 USB 设备", AutoSize = true };
    readonly Button install = new() { Text = "安装到所选设备", AutoSize = true };
    readonly Button root = new() { Text = "检查 / 请求 root ADB", AutoSize = true };
    readonly Button browse = new() { Text = "选择组件文件夹", AutoSize = true };
    readonly Button validate = new() { Text = "重新校验组件", AutoSize = true };
    readonly CheckBox consent = new() { Text = "设备属于我或已获授权；同意不备份直接删除原厂学习软件，并保留开机 root ADB", AutoSize = true, Name = "DeviceConsent" };
    readonly CheckBox reboot = new() { Text = "安装后重启并验证", Checked = true, AutoSize = true };
    readonly Label status = new WrapLabel { Text = "请选择 EXE 同级的 payload 文件夹。", Name = "PayloadStatus" };
    readonly Label counts = new() { Text = "本次完成 0 台 / 失败 0 台", AutoSize = true };
    readonly Button logs = new() { Text = "打开安装日志", AutoSize = true };
    readonly ProgressBar progress = new() { Dock = DockStyle.Fill, Height = 6, Style = ProgressBarStyle.Blocks };
    readonly System.Windows.Forms.Timer timer = new() { Interval = 3000 };
    readonly HashSet<string> completed = new();
    readonly string session = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "C1SlimInstaller", "runs", DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N")[..8]);
    Payload? payload;
    AdbClient? adb;
    bool busy, scanning;
    int success, failures;

    public MainForm(bool offlinePreview = false)
    {
        // Establish the design DPI before assigning sizes or adding controls.
        // Font-based autoscaling during construction previously shrank fixed widths.
        SuspendLayout();
        AutoScaleDimensions = new SizeF(96F, 96F);
        AutoScaleMode = AutoScaleMode.Dpi;
        Font = Control.DefaultFont;
        Text = "C1-Slim 批量安装器 · 外置组件版";
        ClientSize = new Size(1080, 780); MinimumSize = SizeFromClientSize(new Size(900, 740));
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.FromArgb(243, 246, 250);
        ForeColor = Color.FromArgb(31, 41, 55);
        folder.Text = Path.Combine(AppContext.BaseDirectory, "payload");
        BuildLayout();
        ActiveControl = browse;
        folder.SelectionStart = 0; folder.SelectionLength = 0;
        ResumeLayout(true);
        scan.Click += async (_, _) => await RefreshDevices();
        timer.Tick += async (_, _) => { if (!busy) await RefreshDevices(); };
        root.Click += async (_, _) => await Root();
        admit.Click += (_, _) => LaunchAdbAdmit();
        install.Click += async (_, _) => await Install();
        validate.Click += async (_, _) => await ValidateFolder();
        folder.TextChanged += (_, _) => { payload = null; adb = null; EnableControls(); };
        browse.Click += async (_, _) =>
        {
            using var dialog = new FolderBrowserDialog { Description = "选择包含 enrollment、tools 等目录的 payload 文件夹", SelectedPath = folder.Text };
            if (dialog.ShowDialog() == DialogResult.OK) { folder.Text = dialog.SelectedPath; await ValidateFolder(); }
        };
        logs.Click += (_, _) => { Directory.CreateDirectory(session); Process.Start(new ProcessStartInfo("explorer.exe") { UseShellExecute = true, ArgumentList = { session } }); };
        if (!offlinePreview) Shown += async (_, _) => { await ValidateFolder(); timer.Start(); };
        else
        {
            devices.Items.Add(new Device("MagicPen-USB-preview", "device")); devices.SelectedIndex = 0;
            status.Text = "组件尚未就绪，安装已禁用：缺少完整签名核心安装包。补齐 payload/enrollment 并重新组装后，再校验组件。";
            logBox.Text = "[离线界面预览] 未启动 ADB，不连接设备。\r\n等待补齐签名核心安装包；安装按钮保持禁用。";
        }
        FormClosing += (_, e) => { if (busy) { e.Cancel = true; MessageBox.Show("正在检查或安装，请等待结束；不要拔线。", "操作进行中"); } else timer.Stop(); };
        EnableControls();
    }
    void Write(string text)
    {
        if (IsDisposed || Disposing) return;
        if (InvokeRequired)
        {
            try { BeginInvoke(() => Write(text)); }
            catch (InvalidOperationException) when (IsDisposed || Disposing || !IsHandleCreated) { }
            return;
        }
        string line = $"[{DateTime.Now:HH:mm:ss}] {text}\r\n";
        if (logBox.TextLength > 250000) logBox.Clear();
        logBox.AppendText(line);
        try { Directory.CreateDirectory(session); File.AppendAllText(Path.Combine(session, "installer.log"), line); }
        catch (IOException e) { logBox.AppendText("日志写入失败：" + e.Message + "\r\n"); }
        catch (UnauthorizedAccessException e) { logBox.AppendText("日志目录不可写：" + e.Message + "\r\n"); }
    }
    void EnableControls()
    {
        if (IsDisposed || Disposing) return;
        admit.Enabled = !busy;
        scan.Enabled = root.Enabled = adb != null && !busy;
        install.Enabled = payload != null && !busy;
        install.BackColor = install.Enabled ? Color.FromArgb(37, 99, 205) : Color.FromArgb(237, 240, 245);
        install.ForeColor = install.Enabled ? Color.White : Color.FromArgb(139, 148, 162);
        folder.Enabled = browse.Enabled = validate.Enabled = devices.Enabled = consent.Enabled = reboot.Enabled = !busy;
        progress.Style = busy ? ProgressBarStyle.Marquee : ProgressBarStyle.Blocks;
    }
    async Task ValidateFolder()
    {
        if (busy) return;
        busy = true; payload = null; adb = null; EnableControls();
        try
        {
            var candidate = new Payload(folder.Text);
            // Templates can list devices without pretending that signed core files exist.
            if (File.Exists(candidate.FileAt("tools/adb.exe"))) adb = new AdbClient(candidate.FileAt("tools/adb.exe"), new CommandRunner());
            await Task.Run(candidate.Validate); payload = candidate;
            status.Text = $"外置核心 {candidate.Version} / 序列 {candidate.Sequence}：完整性和签名通过。首次使用请先验收一台。";
            Write(status.Text);
        }
        catch (Exception e) { status.Text = "组件尚未就绪，安装已禁用：" + e.Message; Write(status.Text); }
        finally { busy = false; EnableControls(); await RefreshDevices(); }
    }
    async Task RefreshDevices()
    {
        if (adb == null || scanning || busy || devices.DroppedDown) return;
        scanning = true;
        try
        {
            var items = await adb.DevicesAsync();
            if (IsDisposed || Disposing || busy || devices.DroppedDown || devices.Items.Cast<Device>().SequenceEqual(items)) return;
            string? selected = (devices.SelectedItem as Device)?.Serial;
            devices.BeginUpdate();
            try
            {
                devices.Items.Clear(); devices.Items.AddRange(items.Cast<object>().ToArray());
                if (selected != null) devices.SelectedItem = items.FirstOrDefault(x => x.Serial == selected);
                if (devices.SelectedIndex < 0 && devices.Items.Count == 1) devices.SelectedIndex = 0;
            }
            finally { devices.EndUpdate(); }
        }
        catch (Exception e) { Write("USB 检测失败：" + e.Message); }
        finally { scanning = false; }
    }
    Device? Selected()
    {
        if (devices.SelectedItem is Device device && device.State == "device") return device;
        MessageBox.Show("请选择状态为 device 的 USB 设备。offline / unauthorized 需先解决连接或在设备上授权。"); return null;
    }
    async Task Root()
    {
        if (adb == null || busy || Selected() is not Device selected) return;
        if (!consent.Checked) { MessageBox.Show("请先勾选下方“设备属于我或已获授权”操作确认框；这与设备端 ADB 授权不同。"); return; }
        busy = true; EnableControls();
        try { await adb.EnsureRootAsync(selected.Serial, Write); Write("所选设备 root ADB 检查通过：" + selected.Serial); }
        catch (Exception e) { Write(e.Message); }
        finally { busy = false; EnableControls(); }
    }
    async Task Install()
    {
        if (payload == null || busy || Selected() is not Device selected) return;
        if (!consent.Checked) { MessageBox.Show("请先勾选下方操作确认框，同意不备份直接删除原厂学习软件。"); return; }
        string identity;
        try { identity = selected.Serial + ":" + Payload.Hash(payload.FileAt("enrollment/release/manifest.v1")); }
        catch (Exception e) { payload = null; status.Text = "组件已变化，请重新校验：" + e.Message; EnableControls(); return; }
        if (completed.Contains(identity)) { MessageBox.Show("本次已向这台设备安装此版本，请换下一台。"); return; }
        // Freeze inputs and block re-entry before showing a modal confirmation.
        var selectedPayload = payload;
        bool verifyReboot = reboot.Checked;
        busy = true; timer.Stop(); EnableControls();
        string run = Path.Combine(session, "device-" + Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(selected.Serial)))[..12] + "-" + Guid.NewGuid().ToString("N")[..8]);
        try
        {
            if (MessageBox.Show(this, $"目标设备：{selected.Serial}\n核心版本：{selectedPayload.Version}\n不备份原厂学习软件。新核心运行验证通过后，直接删除 /usr/bin/d261 原厂运行目录。\n本次不生成原厂恢复备份，请确认接受删除。\n配套核心核验通过后会开启深度休眠（覆盖已有禁用设置），最终开关检查通过才报告成功。\nWindows / Linux 发布工具和说明会复制到设备存储。\n{(verifyReboot ? "完成后重启并验证同一台设备。" : "本次不重启，重启验收待完成。 ")}\n确认开始？", "确认目标与安装", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
            Directory.CreateDirectory(run);
            var snapshot = await Task.Run(() => selectedPayload.CreateSnapshot(Path.Combine(run, "payload")));
            if (identity != selected.Serial + ":" + Payload.Hash(snapshot.FileAt("enrollment/release/manifest.v1")))
                throw new IOException("确认安装后核心版本发生变化；本次未向设备写入，请重新校验并确认。");
            var engine = new InstallerEngine(snapshot, new AdbClient(snapshot.FileAt("tools/adb.exe"), new CommandRunner()), Write, run);
            await engine.InstallAsync(selected.Serial, verifyReboot);
            completed.Add(identity); success++; status.Text = verifyReboot ? "本台安装和重启检查通过，深度休眠已确认开启。" : "本台已安装，深度休眠已确认开启；重启尚未验证，请保留记录。";
        }
        catch (Exception e)
        {
            failures++; status.Text = "本台安装已停止，未标记成功。请查看日志，恢复同一设备连接后手动重试。";
            Write("安装已停止：" + e.Message);
            Write("详细错误记录：" + Path.Combine(run, "failure.txt"));
            try { Directory.CreateDirectory(run); File.WriteAllText(Path.Combine(run, "failure.txt"), e.ToString()); }
            catch (Exception saveError) { Write("无法保存独立失败记录：" + saveError.Message); }
        }
        finally { counts.Text = $"本次完成 {success} 台 / 失败 {failures} 台"; busy = false; EnableControls(); timer.Start(); }
    }
}

internal static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        if (args.Length >= 1 && args[0] == "--validate-payload")
        {
            try { new Payload(args.Length > 1 ? args[1] : Path.Combine(AppContext.BaseDirectory, "payload")).Validate(); return 0; }
            catch (Exception e) { if (args.Length > 2) File.WriteAllText(args[2], e.ToString()); return 1; }
        }
        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.SetDefaultFont(new Font("Microsoft YaHei UI", 10.5F));
        Application.Run(new MainForm()); return 0;
    }
}
