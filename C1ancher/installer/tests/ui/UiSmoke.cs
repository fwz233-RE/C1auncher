namespace C1SlimInstaller;

internal static class UiSmoke
{
    [STAThread]
    static int Main(string[] args)
    {
        Application.SetHighDpiMode(args.Contains("--dpi-unaware") ? HighDpiMode.DpiUnaware : HighDpiMode.PerMonitorV2);
        Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
        Application.SetDefaultFont(new Font("Microsoft YaHei UI", 10.5F));
        string output = Path.GetFullPath(args.Length > 0 ? args[0] : "ui-smoke");
        Directory.CreateDirectory(output);
        using var form = new MainForm(offlinePreview: true);
        form.StartPosition = FormStartPosition.Manual; form.Location = new Point(-20000, -20000); form.ShowInTaskbar = false;
        form.Show(); Application.DoEvents();
        float scale = form.DeviceDpi / 96F;
        Console.WriteLine($"DPI={form.DeviceDpi}; Font={form.Font.Name} {form.Font.SizeInPoints}pt; initial={form.ClientSize}");
        foreach (var size in new[] { new Size(1080, 780), new Size(900, 740), new Size(1280, 900) })
        {
            form.ClientSize = new Size((int)(size.Width * scale), (int)(size.Height * scale));
            form.PerformLayout(); Application.DoEvents();
            Console.WriteLine($"Viewport {form.ClientSize}");
            foreach (Control c in Descendants(form).Where(c => c.Name.Length > 0 || c is Button || c is WrapLabel))
            {
                Rectangle rect = form.RectangleToClient(c.RectangleToScreen(c.ClientRectangle));
                Console.WriteLine($"  {c.Name} {c.Text.Replace('\r', ' ').Replace('\n', ' ')}: {rect}; font={c.Font.SizeInPoints}");
                if (rect.Bottom > form.ClientSize.Height + 1 || rect.Right > form.ClientSize.Width + 1 || rect.Left < 0 || rect.Top < 0)
                    throw new Exception("Control outside viewport: " + c.Name + " " + c.Text);
                if (c.Name == "DeviceSelector" && c.Width < 250 * scale) throw new Exception("Device selector clipped");
                if (c is WrapLabel && c.Height < c.GetPreferredSize(new Size(c.Width, 0)).Height) throw new Exception("Wrapped label clipped: " + c.Text);
                if (c is Button && c.Text == "安装到所选设备" && c.Enabled) throw new Exception("Template install must remain disabled");
                if (c is Button b && (b.Width < b.GetPreferredSize(Size.Empty).Width || b.Height < b.GetPreferredSize(Size.Empty).Height))
                    throw new Exception("Button text clipped: " + b.Text);
            }
            using var image = new Bitmap(form.Width, form.Height);
            form.DrawToBitmap(image, new Rectangle(Point.Empty, form.Size));
            image.Save(Path.Combine(output, $"installer-{size.Width}x{size.Height}.png"));
        }
        var admitButton = Descendants(form).OfType<Button>().Single(c => c.Name == "AdbAdmitButton");
        if (!admitButton.Enabled) throw new Exception("ADB helper must be available without payload or connected devices");
        using (var dialog = new AdbAdmitDialog(@"D:\c1slim\adb_admit_local.py")) {
            dialog.Location = new Point(-20000, -20000); dialog.StartPosition = FormStartPosition.Manual; dialog.ShowInTaskbar = false;
            dialog.Show(); Application.DoEvents();
            foreach (var size in new[] { new Size(760, 530), new Size(680, 530) }) {
                dialog.ClientSize = new Size((int)(size.Width * scale), (int)(size.Height * scale)); dialog.FitContent(); dialog.PerformLayout(); Application.DoEvents();
                var actions = Descendants(dialog).Single(c => c.Name == "AdbAdmitActions");
                var launch = Descendants(dialog).Single(c => c.Name == "LaunchAdbAdmit");
                var launchRect = dialog.RectangleToClient(launch.RectangleToScreen(launch.ClientRectangle));
                int bottomGap = dialog.ClientSize.Height - launchRect.Bottom;
                if (bottomGap < 0 || bottomGap > 24 * scale) throw new Exception("Excess blank space below dialog buttons: " + bottomGap);
                if (actions.Bottom != dialog.ClientSize.Height) throw new Exception("Dialog actions must stay at bottom");
                foreach (var c in Descendants(dialog).Where(c => c is Button || c is WrapLabel || c is TextBox)) {
                    var rect = dialog.RectangleToClient(c.RectangleToScreen(c.ClientRectangle));
                    if (rect.Bottom > dialog.ClientSize.Height + 1 || rect.Right > dialog.ClientSize.Width + 1 || rect.Top < 0 || rect.Left < 0)
                        throw new Exception("ADB dialog control outside viewport: " + c.Text + " " + rect);
                    if (c is WrapLabel && c.Height < c.GetPreferredSize(new Size(c.Width, 0)).Height) throw new Exception("ADB instructions clipped");
                    if (c is Button b && b.Width < b.GetPreferredSize(Size.Empty).Width) throw new Exception("ADB button clipped");
                }
                using var image = new Bitmap(dialog.Width, dialog.Height); dialog.DrawToBitmap(image, new Rectangle(Point.Empty, dialog.Size));
                image.Save(Path.Combine(output, $"adb-hotspot-{size.Width}x{size.Height}.png"));
            }
            dialog.FitContent((int)(300 * scale)); Application.DoEvents();
            var scrollBody = (Panel)Descendants(dialog).Single(c => c.Name == "AdbAdmitBody");
            var footer = Descendants(dialog).Single(c => c.Name == "AdbAdmitActions");
            if (!scrollBody.VerticalScroll.Visible || footer.Bottom != dialog.ClientSize.Height || scrollBody.Bottom > footer.Top)
                throw new Exception("Short viewport must scroll only the body and retain the footer");
            scrollBody.AutoScrollPosition = new Point(0, scrollBody.VerticalScroll.Maximum); Application.DoEvents();
            var fixedButton = Descendants(dialog).Single(c => c.Name == "LaunchAdbAdmit");
            var fixedRect = dialog.RectangleToClient(fixedButton.RectangleToScreen(fixedButton.ClientRectangle));
            if (dialog.ClientSize.Height - fixedRect.Bottom > 24 * scale || fixedRect.Top < footer.Top)
                throw new Exception("Scrolling moved the action buttons");
            using (var image = new Bitmap(dialog.Width, dialog.Height)) {
                dialog.DrawToBitmap(image, new Rectangle(Point.Empty, dialog.Size)); image.Save(Path.Combine(output, "adb-hotspot-short-scrolled.png"));
            }
            Console.WriteLine("PASS: dialog fits content, bottom gap <=24 logical pixels, short viewport scrolls body only.");
            if (dialog.AcceptButton != null) throw new Exception("Enter must not implicitly launch the script");
            dialog.Close();
        }
        using (var cancelTimer = new System.Windows.Forms.Timer { Interval = 50 }) {
            bool cancelled = false;
            cancelTimer.Tick += (_, _) => {
                var prompt = Application.OpenForms.OfType<AdbAdmitDialog>().FirstOrDefault();
                if (prompt != null) { cancelled = true; prompt.DialogResult = DialogResult.Cancel; cancelTimer.Stop(); }
            };
            cancelTimer.Start(); admitButton.PerformClick();
            if (!cancelled || !admitButton.Enabled) throw new Exception("ADB prompt cancellation did not restore controls");
            if (typeof(MainForm).GetField("admitWindow", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.GetValue(form) != null)
                throw new Exception("Cancel must not start an elevated process");
        }
        Console.WriteLine("PASS: hotspot instructions fit, helper works without payload, and cancelling launches no process.");
        // Simulate an asynchronous disconnect without starting ADB or touching a device.
        var adbField = typeof(MainForm).GetField("adb", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!;
        adbField.SetValue(form, new AdbClient("OFFLINE-NOT-AN-EXECUTABLE", new DisconnectRunner()));
        var refresh = typeof(MainForm).GetMethod("RefreshDevices", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!;
        Pump((Task)refresh.Invoke(form, null)!);
        Console.WriteLine("Disconnect diagnostic: disposed=" + form.IsDisposed + "; " + string.Join(" | ", Descendants(form).Where(c => c.Name == "InstallLog").Select(c => c.Text)));
        if (form.IsDisposed || !Descendants(form).Any(c => c.Name == "InstallLog" && c.Text.Contains("OFFLINE simulated disconnect")))
            throw new Exception("Disconnect did not leave the form open with an error log");
        Console.WriteLine("PASS: asynchronous disconnect keeps the window open and logs failure.");
        var pending = (Task)refresh.Invoke(form, null)!;
        form.Close();
        Pump(pending);
        Console.WriteLine("PASS: late disconnect callback after window close does not crash.");
        Console.WriteLine("PASS: layout fits all tested window sizes; offline preview never starts ADB.");
        return 0;
    }
    sealed class DisconnectRunner : ICommandRunner
    {
        public async Task<CommandResult> RunAsync(string file, IEnumerable<string> args, TimeSpan timeout, CancellationToken cancellation = default)
        {
            if (file != "OFFLINE-NOT-AN-EXECUTABLE" || !args.SequenceEqual(new[] { "devices" }))
                throw new Exception("Unexpected simulated command");
            await Task.Delay(50);
            throw new IOException("OFFLINE simulated disconnect");
        }
    }
    static void Pump(Task task)
    {
        var watch = System.Diagnostics.Stopwatch.StartNew();
        while (!task.IsCompleted && watch.Elapsed < TimeSpan.FromSeconds(5))
        { Application.DoEvents(); System.Threading.Thread.Sleep(5); }
        if (!task.IsCompleted) throw new Exception("UI callback hung");
        task.GetAwaiter().GetResult();
        Application.DoEvents();
    }
    static IEnumerable<Control> Descendants(Control parent)
    {
        foreach (Control child in parent.Controls) {
            yield return child;
            foreach (var nested in Descendants(child)) yield return nested;
        }
    }
}
