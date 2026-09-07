namespace C1SlimInstaller;

// Measure against the current column width, not a construction-time MaximumSize.
// This keeps Chinese text readable when the window or monitor DPI changes.
internal sealed class WrapLabel : Label
{
    public WrapLabel() { AutoSize = true; Dock = DockStyle.Top; }
    public override Size GetPreferredSize(Size proposedSize)
    {
        int available = Parent == null ? 800 : Math.Max(1, Parent.ClientSize.Width - Parent.Padding.Horizontal - Margin.Horizontal);
        int width = proposedSize.Width > 1 && proposedSize.Width < int.MaxValue ? Math.Min(proposedSize.Width, available) : available;
        return base.GetPreferredSize(new Size(width, 0));
    }
}

public sealed partial class MainForm
{
    static TableLayoutPanel Stack(int rows, Padding padding = default) {
        var table = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 1, RowCount = rows, Padding = padding, Margin = Padding.Empty };
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (int i = 0; i < rows; i++) table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        return table;
    }
    Label Heading(string text) => new() { Text = text, AutoSize = true, Dock = DockStyle.Top,
        Font = new Font(Font, FontStyle.Bold), Margin = new Padding(0, 0, 0, 8) };
    static TableLayoutPanel InputRow(params Control[] controls) {
        var row = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = controls.Length,
            RowCount = 1, Margin = new Padding(0, 0, 0, 8) };
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        row.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        for (int i = 1; i < controls.Length; i++) row.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        for (int i = 0; i < controls.Length; i++) {
            controls[i].Anchor = AnchorStyles.Left | AnchorStyles.Right;
            controls[i].Margin = new Padding(i == 0 ? 0 : 10, 0, 0, 0);
            row.Controls.Add(controls[i], i, 0);
        }
        return row;
    }
    void BuildLayout()
    {
        foreach (var button in new[] { browse, validate, scan, root, install, logs, admit }) {
            button.AutoSize = true; button.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            button.Padding = new Padding(12, 5, 12, 5);
            button.MinimumSize = new Size(0, 38);
            button.FlatStyle = FlatStyle.Flat;
            button.FlatAppearance.BorderColor = Color.FromArgb(207, 215, 226);
            button.BackColor = Color.White; button.UseVisualStyleBackColor = false;
            button.Cursor = Cursors.Hand;
        }
        install.BackColor = Color.FromArgb(225, 236, 255);
        install.ForeColor = Color.FromArgb(27, 79, 156);
        install.Font = new Font(Font, FontStyle.Bold);
        folder.Margin = Padding.Empty;
        devices.MinimumSize = new Size(260, 0);
        devices.DropDown += (_, _) => {
            int width = devices.Width;
            foreach (var item in devices.Items) width = Math.Max(width, TextRenderer.MeasureText(item.ToString(), devices.Font).Width + SystemInformation.VerticalScrollBarWidth + 24);
            devices.DropDownWidth = width;
        };
        var layout = Stack(7, new Padding(20));
        layout.Name = "MainLayout";
        layout.Dock = DockStyle.Fill; layout.AutoSize = false; layout.AutoScroll = true;
        layout.RowStyles[6] = new RowStyle(SizeType.Percent, 100);

        var header = Stack(2);
        header.Margin = new Padding(0, 0, 0, 18);
        header.Controls.Add(new Label { Text = "C1-Slim 设备安装器", AutoSize = true, Font = new Font(Font.FontFamily, 20, FontStyle.Bold), Margin = new Padding(0, 0, 0, 6) });
        header.Controls.Add(new WrapLabel { Text = "逐台安装核心系统，不预装普通应用。组件放在外置文件夹中，换版本无需更换 EXE。", ForeColor = Color.FromArgb(91, 105, 125), Margin = Padding.Empty });
        layout.Controls.Add(header, 0, 0);

        var payloadCard = Stack(3, new Padding(18));
        payloadCard.BackColor = Color.White; payloadCard.Margin = new Padding(0, 0, 0, 14);
        payloadCard.Controls.Add(Heading("1  选择安装组件"), 0, 0);
        payloadCard.Controls.Add(InputRow(folder, browse, validate), 0, 1);
        status.Padding = new Padding(12, 10, 12, 10); status.Margin = Padding.Empty;
        status.BackColor = Color.FromArgb(255, 247, 225); status.ForeColor = Color.FromArgb(126, 82, 14);
        payloadCard.Controls.Add(status, 0, 2);
        layout.Controls.Add(payloadCard, 0, 1);

        var deviceCard = Stack(5, new Padding(18));
        deviceCard.BackColor = Color.White; deviceCard.Margin = new Padding(0, 0, 0, 14);
        deviceCard.Controls.Add(InputRow(Heading("2  连接并安装设备"), admit), 0, 0);
        deviceCard.Controls.Add(new WrapLabel { Text = "尚未开启 ADB？点击“开启 ADB（热点辅助）”，按提示连接电脑热点并启动本地脚本；已连接的设备可直接安装。\n“检查 / 请求 root ADB”仅请求设备支持的 adb root；新核心验证通过后才直接删除原厂学习软件。", ForeColor = Color.FromArgb(91, 105, 125), Margin = new Padding(0, 0, 0, 10) }, 0, 1);
        deviceCard.Controls.Add(InputRow(devices, scan, root, install), 0, 2);
        consent.Dock = DockStyle.Top; consent.Margin = new Padding(0, 2, 0, 12);
        // The consent wraps at the live card width, including at the minimum window size.
        deviceCard.SizeChanged += (_, _) => {
            int width = Math.Max(1, deviceCard.ClientSize.Width - deviceCard.Padding.Horizontal);
            if (consent.MaximumSize.Width != width) consent.MaximumSize = new Size(width, 0);
        };
        deviceCard.Controls.Add(consent, 0, 3);
        var options = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, WrapContents = true, Margin = Padding.Empty };
        reboot.Margin = new Padding(0, 8, 22, 8); counts.Margin = new Padding(0, 8, 22, 8); logs.Margin = Padding.Empty;
        options.Controls.AddRange([reboot, counts, logs]); deviceCard.Controls.Add(options, 0, 4);
        layout.Controls.Add(deviceCard, 0, 2);

        layout.Controls.Add(new WrapLabel { Text = "安装范围：主页 · 包管理器 · 启动保活 · 核心更新 · 仓库公钥 · Windows / Linux 发布工具。\n通过 ADB 安装用户空间组件，不格式化存储，不刷写 bootloader 或整机分区。", ForeColor = Color.FromArgb(91, 105, 125), Margin = new Padding(0, 0, 0, 8) }, 0, 3);
        progress.Margin = new Padding(0, 0, 0, 10); layout.Controls.Add(progress, 0, 4);
        var logHeading = Heading("运行日志"); logHeading.Margin = new Padding(0, 0, 0, 8); layout.Controls.Add(logHeading, 0, 5);
        var logPanel = new Panel { Dock = DockStyle.Fill, Padding = new Padding(12), BackColor = Color.White, Margin = Padding.Empty, MinimumSize = new Size(0, 100) };
        logBox.BackColor = Color.White; logBox.ForeColor = Color.FromArgb(55, 65, 81);
        logPanel.Controls.Add(logBox); layout.Controls.Add(logPanel, 0, 6);
        Controls.Add(layout);
    }
}
