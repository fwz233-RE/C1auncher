using System.Diagnostics;
using System.Security.Cryptography;
using C1SlimInstaller;

namespace InstallerOfflineTests;

internal static partial class Program
{
    static readonly List<(string Name, Func<Task> Run)> Tests = [];
    static readonly string[] SyntaxScripts = [
        "device-setup.sh", "enrollment/app-daemon-bootstrap.sh", "enrollment/device-core-enroll.sh", "enrollment/enroll.sh",
        "profile/c1-update-check.sh", "profile/device-repository-config.sh", "usb/S90usb.open", "usb/device-open-adb.sh"
    ];
    static void Test(string name, Action<Fixture> test) => TestAsync(name, f => { test(f); return Task.CompletedTask; });
    static void TestAsync(string name, Func<Fixture, Task> test) => Tests.Add((name, async () => { using var f = new Fixture(); await test(f); }));
    static void Reject(string name, Action<Fixture> mutate, string message, bool refresh = true) => Test(name, f => { mutate(f); if (refresh) f.RefreshSums(); Check.Throws<IOException>(f.Payload.Validate, message); });
    static async Task<int> Main(string[] args)
    {
        if (args is ["--print-running-process-command"]) { Console.Write(RunningProcessProbe.Command); return 0; }
        if (args is ["--verify-running-process-readonly", var adbPath, var serial, var expectedApp, var expectedLauncher]) {
            var client = new AdbClient(adbPath, new CommandRunner());
            string? identity = null;
            for (int observation = 0; observation < 2; observation++) {
                string snapshot = await client.ShellAsync(serial, RunningProcessProbe.Command);
                var result = RunningProcessProbe.Analyze(snapshot, expectedApp, expectedLauncher);
                Console.WriteLine(snapshot);
                Console.WriteLine(result.Reason);
                if (!result.Valid || (identity != null && identity != result.Identity)) return 1;
                identity = result.Identity;
            }
            Console.WriteLine("READONLY_RUNNING_PROCESS_VERIFIED " + serial);
            return 0;
        }
        if (args is ["--analyze-running-process-snapshot", var snapshotFile, var appDigest, var launcherDigest]) {
            var observed = RunningProcessProbe.Analyze(snapshotFile == "-" ? Console.In.ReadToEnd() : File.ReadAllText(snapshotFile), appDigest, launcherDigest);
            Console.WriteLine(observed.Reason); return observed.Valid ? 0 : 1;
        }
        if (args.Length > 0 && args[0] == "--print-enrollment-metadata-command")
        {
            if (args.Length != 2) { Console.Error.WriteLine("Usage: --print-enrollment-metadata-command /storage/c1-installer-<32 lowercase hex digits>"); return 2; }
            try { Console.WriteLine(InstallerEngine.EnrollmentMetadataCommand(args[1])); return 0; }
            catch (ArgumentException e) { Console.Error.WriteLine(e.Message); return 2; }
        }
        if (args.Length == 2 && args[0] == "--runner-child")
        {
            if (args[1] == "wait") { await Task.Delay(TimeSpan.FromSeconds(30)); return 0; }
            Console.WriteLine("runner stdout"); Console.Error.WriteLine("runner stderr"); return 7;
        }
        // Public factory scripts are the only workspace fixture inputs. No shipped key/tool is read.
        string? workspace = null, validator = null;
        for (int i = 0; i < args.Length; i += 2)
        {
            if (i + 1 >= args.Length || args[i] is not ("--workspace" or "--validator-exe"))
            {
                Console.Error.WriteLine("Usage: Installer.OfflineTests [--workspace <c1slim directory>] [--validator-exe <published GUI EXE>]"); return 2;
            }
            if (args[i] == "--workspace") workspace = Path.GetFullPath(args[i + 1]);
            else validator = Path.GetFullPath(args[i + 1]);
        }
        Fixture.Workspace = workspace ?? FindWorkspace();
        RegisterPayload(); RegisterCommands(); RegisterEngine(); RegisterAcceptance(); RegisterSuspendAcceptance(); RegisterAdbAdmit(); RegisterProcessProbe();
        if (validator != null)
        {
            if (!File.Exists(validator)) throw new IOException("Published validator EXE does not exist");
            foreach (string state in new[] { "valid", "invalid signature", "template" })
                TestAsync("published EXE offline validation: " + state, async f => {
                    if (state == "invalid signature") { f.Put("enrollment/bootstrap.v1.sig", new byte[64]); f.RefreshSums(); }
                    if (state == "template") Directory.Delete(f.At("enrollment"), true);
                    string diagnostic = Path.Combine(f.Root, "validator-error.txt");
                    // The GUI's validation-only entry point never initializes the form or real ADB.
                    var result = await new CommandRunner().RunAsync(validator, ["--validate-payload", f.Payload.Root, diagnostic], TimeSpan.FromSeconds(30));
                    Check.Equal(state == "valid" ? 0 : 1, result.ExitCode);
                    if (state != "valid") Check.True(File.ReadAllText(diagnostic).Contains(state == "template" ? "缺少完整签名" : "数字签名验证失败"), "Unexpected EXE validation diagnostic");
                });
        }
        int failed = 0, skipped = 0; var watch = Stopwatch.StartNew();
        foreach (var (name, run) in Tests)
        {
            try { await run(); Console.WriteLine("PASS " + name); }
            catch (SkipException e) { skipped++; Console.WriteLine("SKIP " + name + ": " + e.Message); }
            catch (Exception e) { failed++; Console.Error.WriteLine("FAIL " + name + "\n" + e); }
        }
        Console.WriteLine($"RESULT total={Tests.Count} passed={Tests.Count - failed - skipped} failed={failed} skipped={skipped} elapsed={watch.Elapsed.TotalSeconds:F2}s");
        return failed == 0 ? 0 : 1;
    }
    static void RegisterAdbAdmit()
    {
        Test("ADB helper falls back to requested external script path", f =>
            Check.Equal(@"D:\c1slim\adb_admit_local.py", AdbAdmitLauncher.DefaultScript(f.Root)));
        Test("ADB helper prefers script beside EXE", f => {
            string script = Path.Combine(f.Root, "adb_admit_local.py"); File.WriteAllText(script, "raise RuntimeError('MUST NEVER EXECUTE')");
            Check.Equal(script, AdbAdmitLauncher.DefaultScript(f.Root));
        });
        Test("ADB helper quotes PowerShell metacharacters as literals", f =>
            Check.Equal("'a''b;$env:TEMP`中文'", AdbAdmitLauncher.Literal("a'b;$env:TEMP`中文")));
        foreach (string ip in new[] { "127.0.0.1", "0.0.0.0", "224.0.0.1", "255.255.255.255", "::1", "192.168.1", "1.2.3.4;exit", "invalid" })
            Test("ADB helper rejects invalid device IP: " + ip, f => Check.Throws<ArgumentException>(() => AdbAdmitLauncher.NormalizeDeviceIp(ip), "IPv4"));
        Test("ADB helper accepts blank or canonical IPv4", f => {
            Check.Equal("", AdbAdmitLauncher.NormalizeDeviceIp("  "));
            Check.Equal("192.168.137.231", AdbAdmitLauncher.NormalizeDeviceIp(" 192.168.137.231 "));
        });
        Test("ADB helper rejects missing script before elevation", f =>
            Check.Throws<IOException>(() => AdbAdmitLauncher.Command(Path.Combine(f.Root, "missing.py"), "", f.Root), "脚本"));
        Test("ADB helper rejects non Python file before elevation", f => {
            string script = Path.Combine(f.Root, "helper.cmd"); File.WriteAllText(script, "NEVER EXECUTE");
            Check.Throws<IOException>(() => AdbAdmitLauncher.Command(script, "", f.Root), "脚本");
        });
        if (OperatingSystem.IsWindows()) foreach (string ip in new[] { "", "192.168.137.231" })
            TestAsync("ADB helper executes harmless Python fixture only: " + (ip == "" ? "default" : "explicit IP"), async f => {
                string script = Path.Combine(f.Root, "中文 space ' ; $x ` harmless.py");
                string expected = ip.Length == 0 ? "[]" : "['--device-ip', '192.168.137.231']";
                File.WriteAllText(script, "import sys\nassert sys.argv[1:] == " + expected + "\nprint('HARMLESS_HELPER_ARGS_OK', flush=True)\n");
                string command = AdbAdmitLauncher.Command(script, ip, Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData));
                var result = await new CommandRunner().RunAsync(Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe"),
                    ["-NoProfile", "-NonInteractive", "-EncodedCommand", Convert.ToBase64String(System.Text.Encoding.Unicode.GetBytes(command))], TimeSpan.FromSeconds(20));
                Check.Equal(0, result.ExitCode);
                Check.True(result.Output.Contains("HARMLESS_HELPER_ARGS_OK"), "Harmless Python launcher fixture failed: " + result.Output);
            });
        foreach (string ip in new[] { "", "192.168.137.231" })
            TestAsync("ADB helper visible elevated launch and parse-only validation: " + (ip == "" ? "default" : "explicit IP"), async f => {
                string script = Path.Combine(f.Root, "中文 space ' ; $x ` helper.py"); File.WriteAllText(script, "raise RuntimeError('MUST NEVER EXECUTE')");
                var info = AdbAdmitLauncher.StartInfo(script, ip, f.Root, Environment.SystemDirectory);
                Check.True(info.UseShellExecute && info.Verb == "runas" && !info.RedirectStandardOutput && !info.CreateNoWindow, "Must launch a visible UAC terminal");
                Check.Equal(ProcessWindowStyle.Normal, info.WindowStyle);
                Check.Equal(Path.GetDirectoryName(script)!, info.WorkingDirectory!);
                const string prefix = "-NoLogo -NoProfile -NoExit -EncodedCommand ";
                Check.True(info.Arguments.StartsWith(prefix), "Window must retain diagnostics");
                string command = System.Text.Encoding.Unicode.GetString(Convert.FromBase64String(info.Arguments[prefix.Length..]));
                Check.Equal(AdbAdmitLauncher.Command(script, ip, f.Root), command);
                Check.True(command.Contains("$script = " + AdbAdmitLauncher.Literal(script)), "Path must remain one literal");
                Check.Equal(ip.Length != 0, command.Contains("$scriptArgs += @('--device-ip', '192.168.137.231')"));
                Check.True(!command.Contains("ExecutionPolicy") && !command.Contains("kill-server"), "No policy bypass or shared ADB shutdown");
                if (OperatingSystem.IsWindows()) {
                    // Parse only. Never evaluate the generated launcher, import Python dependencies or contact a device.
                    string parse = "[void][scriptblock]::Create([Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('" + info.Arguments[prefix.Length..] + "'))); Write-Output 'PARSE_OK'";
                    var result = await new CommandRunner().RunAsync(info.FileName, ["-NoProfile", "-NonInteractive", "-EncodedCommand", Convert.ToBase64String(System.Text.Encoding.Unicode.GetBytes(parse))], TimeSpan.FromSeconds(15));
                    Check.Equal(0, result.ExitCode); Check.True(result.Output.Contains("PARSE_OK"), "PowerShell parse failed: " + result.Output);
                }
            });
    }
    static string FindWorkspace()
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
            if (Directory.Exists(Path.Combine(d.FullName, "firmware-analysis", "system-rootfs"))) return d.FullName;
        throw new IOException("Cannot locate public factory fixtures. Pass --workspace <c1slim directory>.");
    }
    static IEnumerable<(string File, string Defect, Func<byte[], byte[]> Edit)> InvalidShellCases()
    {
        foreach (string file in Fixture.ShellScripts.Append(Fixture.ShellConfig))
        {
            yield return (file, "CRLF", b => b.SelectMany(v => v == 10 ? new byte[] { 13, 10 } : new[] { v }).ToArray());
            yield return (file, "UTF-8 BOM", b => new byte[] { 0xef, 0xbb, 0xbf }.Concat(b).ToArray());
            // Keep the shebang and final LF intact; embedded controls must still be rejected.
            yield return (file, "bare CR", b => b[..^1].Concat(new byte[] { 13, 35, 10 }).ToArray());
            yield return (file, "NUL", b => b[..^1].Concat(new byte[] { 0, 10 }).ToArray());
            yield return (file, "invalid UTF-8", b => b[..^1].Concat(new byte[] { 0xc3, 0x28, 10 }).ToArray());
            yield return (file, "empty file", _ => []);
            if (file != Fixture.ShellConfig)
                yield return (file, "missing shebang", b => b[2..]);
        }
    }
    static string ShellFormatError(string file) => "设备 Shell 文件格式无效：" + file + "。";
    static void RegisterPayload()
    {
        Test("valid signed payload with four minimal MIPS ELF fixtures", f => {
            f.Payload.Validate(); Check.Equal("1.2.3-test", f.Payload.Version); Check.Equal("7", f.Payload.Sequence);
            Check.Equal(13, f.Payload.Files.Count(n => n.StartsWith("enrollment/")));
            Check.Equal(Directory.GetFiles(f.Payload.Root, "*", SearchOption.AllDirectories).Length - 1, f.Payload.Files.Count);
            Check.Equal(Payload.OriginalUsbHash, Payload.Hash(f.At("usb/S90usb.original")));
        });
        Test("device shell allowlist covers all expected scripts and sourced config", f => {
            Check.True(Payload.DeviceShellScripts.Order().SequenceEqual(Fixture.ShellScripts.Order()), "Device shell script set differs");
            Check.Equal(Fixture.ShellConfig, Payload.DeviceShellConfig);
            Check.True(!File.ReadAllText(f.At(Fixture.ShellConfig)).StartsWith("#!"), "Config fixture must test the shebang exemption");
            Check.True(File.ReadAllText(f.At("accessories/neofetch.upstream")).StartsWith("#!/usr/bin/env bash\n"), "Upstream fixture must use bash");
            f.Payload.Validate();
        });
        foreach (var (file, defect, edit) in InvalidShellCases())
            Test("device shell format rejects " + file + ": " + defect, f => {
                f.EditShell(file, edit);
                var before = Directory.GetFiles(f.Payload.Root, "*", SearchOption.AllDirectories).ToDictionary(p => p, File.ReadAllBytes);
                Check.Throws<IOException>(f.Payload.Validate, ShellFormatError(file));
                foreach (var (path, bytes) in before) Check.Bytes(bytes, File.ReadAllBytes(path));
            });
        foreach (string file in Fixture.ShellScripts.Append(Fixture.ShellConfig).Where(n => n != "usb/S90usb.original"))
            Test("device shell accepts valid non-ASCII UTF-8 without changing bytes: " + file, f => {
                f.EditShell(file, b => b.Concat(System.Text.Encoding.UTF8.GetBytes("# 测试 UTF-8 é 😀\n")).ToArray());
                var before = Directory.GetFiles(f.Payload.Root, "*", SearchOption.AllDirectories).ToDictionary(p => p, File.ReadAllBytes);
                f.Payload.Validate();
                foreach (var (path, bytes) in before) Check.Bytes(bytes, File.ReadAllBytes(path));
            });
        foreach (string file in Fixture.ShellScripts.Append(Fixture.ShellConfig).Where(n => n != "usb/S90usb.original"))
            Test("device shell accepts and snapshots missing final LF without changing bytes: " + file, f => {
                f.EditShell(file, b => b[^1] == 10 ? b[..^1] : b);
                Check.True(File.ReadAllBytes(f.At(file))[^1] != 10, "Fixture still has a final LF");
                var before = Directory.GetFiles(f.Payload.Root, "*", SearchOption.AllDirectories).ToDictionary(p => p, File.ReadAllBytes);
                f.Payload.Validate();
                var snapshot = f.Payload.CreateSnapshot(Path.Combine(f.Root, "snapshot"));
                foreach (var (path, bytes) in before) {
                    Check.Bytes(bytes, File.ReadAllBytes(path));
                    Check.Bytes(bytes, File.ReadAllBytes(snapshot.FileAt(Path.GetRelativePath(f.Payload.Root, path).Replace('\\', '/'))));
                }
            });
        Reject("USB baseline without final LF passes format but retains pinned digest", f => f.EditShell("usb/S90usb.original", b => b[..^1]), "USB 基线校验失败");
        Reject("outer manifest detects content tampering", f => f.Put("developer/README.md", "tampered\n"), "安装包文件校验失败", false);
        Reject("outer manifest rejects unlisted file", f => f.Put("extra.txt", "unexpected"), "额外文件", false);
        Reject("outer manifest rejects duplicate entry", f => File.AppendAllText(f.At("SHA256SUMS"), File.ReadLines(f.At("SHA256SUMS")).First() + "\n"), "安装包文件校验失败", false);
        Reject("outer manifest rejects malformed line", f => File.AppendAllText(f.At("SHA256SUMS"), "bad digest\n"), "文件清单无效", false);
        Reject("outer manifest rejects case-insensitive duplicate", f => {
            string line = File.ReadLines(f.At("SHA256SUMS")).First(); File.AppendAllText(f.At("SHA256SUMS"), line[..66] + line[66..].ToUpperInvariant() + "\n");
        }, "安装包文件校验失败", false);
        Test("outer manifest path traversal rejected before reading outside", f => {
            File.AppendAllText(f.At("SHA256SUMS"), new string('0', 64) + "  ../outside\n");
            Check.Throws<InvalidDataException>(f.Payload.Validate, "路径无效");
        });
        Reject("extra enrollment member rejected even with fresh outer hash", f => f.Put("enrollment/extra.txt", "test"), "13 个");
        Reject("missing enrollment artifact rejected", f => File.Delete(f.At("enrollment/release/artifacts/c1pkg")), "13 个");
        Reject("missing required ADB DLL rejected", f => File.Delete(f.At("tools/AdbWinUsbApi.dll")), "缺少安装组件");
        Reject("bootstrap signature corruption rejected with fresh outer hash", f => { byte[] sig = File.ReadAllBytes(f.At("enrollment/bootstrap.v1.sig")); sig[0] ^= 1; f.Put("enrollment/bootstrap.v1.sig", sig); }, "数字签名验证失败");
        Reject("bootstrap signature length rejected", f => f.Put("enrollment/bootstrap.v1.sig", new byte[63]), "签名或公钥长度无效");
        Reject("release signature rejected despite valid resigned bootstrap", f => { f.Put("enrollment/release/manifest.v1.sig", new byte[64]); f.RefreshBootstrap(); }, "数字签名验证失败");
        Reject("public PEM and raw key mismatch rejected", f => f.Put("enrollment/core.ed25519.pem", Fixture.Pem(RandomNumberGenerator.GetBytes(32))), "核心公钥不一致");
        Reject("matching malformed public key length rejected", f => {
            byte[] shortKey = new byte[31]; f.Put("enrollment/core.ed25519.pub", shortKey); f.Put("enrollment/core.ed25519.pem", Fixture.Pem(shortKey));
        }, "核心公钥不一致");
        Reject("bootstrap binds script digest independently of outer hashes", f => f.Put("enrollment/enroll.sh", "changed signed script\n"), "核心引导文件校验失败");
        Reject("bootstrap rejects release-only resigning", f => f.EditSigned("enrollment/release/manifest.v1", s => s.Replace("S\t7\n", "S\t8\n")), "核心引导文件校验失败");
        Reject("release artifact hash rejects change even after outer refresh", f => { var b = File.ReadAllBytes(f.At("enrollment/release/artifacts/C1ancher")); b[100] = 1; f.Put("enrollment/release/artifacts/C1ancher", b); }, "核心组件校验失败");
        foreach (var (name, edit, message) in new (string, Func<string, string>, string)[] {
            ("CRLF", s => s.Replace("\n", "\r\n"), "编码无效"),
            ("missing newline", s => s.TrimEnd('\n'), "编码无效"),
            ("wrong bootstrap version", s => s.Replace("V\t1.1.0", "V\t2.0.0"), "引导版本无效"),
            ("duplicate daemon approval", s => s + "H\t" + Payload.OriginalDaemonHash + "\n", "未批准原厂设备基线"),
            ("unapproved daemon baseline", s => s.Replace(Payload.OriginalDaemonHash, new string('0', 64)), "未批准原厂设备基线") })
            Reject("signed bootstrap rejects " + name, f => f.EditSigned("enrollment/bootstrap.v1", edit), message);
        foreach (var (name, edit, message) in new (string, Func<string, string>, string)[] {
            ("zero sequence", s => s.Replace("S\t7\n", "S\t0\n"), "核心清单无效"),
            ("overflow sequence", s => s.Replace("S\t7\n", "S\t18446744073709551616\n"), "数字字段无效"),
            ("zero epoch", s => s.Replace("E\t1\n", "E\t0\n"), "数字字段无效"),
            ("leading-zero epoch", s => s.Replace("E\t1\n", "E\t01\n"), "数字字段无效"),
            ("wrong epoch tag", s => s.Replace("E\t1\n", "X\t1\n"), "数字字段无效"),
            ("invalid version suffix", s => s.Replace("V\t1.2.3-test", "V\t1.2.3-"), "兼容性字段无效"),
            ("wrong target", s => s.Replace("mips32r2-little-o32-hard-float-double-static", "x86_64"), "目标平台无效"),
            ("invalid compatibility", s => s.Replace("C\ttest-only", "C\tbad/value"), "兼容性字段无效"),
            ("artifact traversal", s => s.Replace("artifacts/C1ancher\t", "../artifacts/C1ancher\t"), "核心组件校验失败"),
            ("artifact mode", s => s.Replace("\t700\n", "\t755\n"), "核心组件校验失败"),
            ("artifact size", s => s.Replace("\t108\t", "\t109\t"), "核心组件校验失败"),
            ("CRLF", s => s.Replace("\n", "\r\n"), "编码无效") })
            Reject("signed release rejects " + name, f => f.EditSigned("enrollment/release/manifest.v1", edit, true), message);
        foreach (var (name, edit, message) in new (string, Action<byte[]>, string)[] {
            ("wrong machine", b => Fixture.U16(b, 18, 62), "静态 MIPS ELF32"),
            ("ELF64", b => b[4] = 2, "静态 MIPS ELF32"),
            ("big endian", b => b[5] = 2, "静态 MIPS ELF32"),
            ("shared object", b => Fixture.U16(b, 16, 3), "静态 MIPS ELF32"),
            ("wrong ISA flags", b => Fixture.U32(b, 36, 0x50001000), "MIPS32r2 o32 ABI"),
            ("wrong ABI flags", b => Fixture.U32(b, 36, 0x70002000), "MIPS32r2 o32 ABI"),
            ("program header overflow", b => Fixture.U32(b, 28, uint.MaxValue), "程序头无效"),
            ("overlapping program header", b => Fixture.U32(b, 28, 0), "程序头无效"),
            ("program header width", b => Fixture.U16(b, 42, 31), "程序头无效"),
            ("no program headers", b => Fixture.U16(b, 44, 0), "程序头无效"),
            ("dynamic segment", b => Fixture.U32(b, 52, 2), "不能依赖动态解释器"),
            ("interpreter", b => Fixture.U32(b, 52, 3), "不能依赖动态解释器"),
            ("absent ABI segment", b => Fixture.U32(b, 52, 1), "缺少 MIPS ABI"),
            ("ABI segment out of bounds", b => Fixture.U32(b, 56, uint.MaxValue), "双精度 hard-float"),
            ("wrong ABI ISA", b => b[86] = 64, "双精度 hard-float"),
            ("wrong ABI ISA revision", b => b[87] = 1, "双精度 hard-float"),
            ("soft float", b => b[91] = 3, "双精度 hard-float") })
            Reject("fully resigned ELF rejects " + name, f => { byte[] elf = Fixture.MinimalElf(); edit(elf); f.Put("enrollment/release/artifacts/C1ancher", elf); f.RefreshRelease(); }, message);
        Reject("fully resigned truncated ELF rejected", f => { f.Put("enrollment/release/artifacts/C1ancher", new byte[51]); f.RefreshRelease(); }, "静态 MIPS ELF32");
        Reject("developer and profile keys must match", f => f.Put("developer/repository.ed25519.pub", new byte[32]), "开发工具附带的公钥");
        Reject("repository public key must be 32 bytes", f => { f.Put("profile/repository.ed25519.pub", new byte[31]); f.Put("developer/repository.ed25519.pub", new byte[31]); }, "应用公钥无效");
        Reject("USB baseline is genuinely pinned", f => f.Put("usb/S90usb.original", "#!/bin/sh\n# not factory\n"), "USB 基线校验失败");
        Reject("wallpaper exact length checked", f => f.Put("accessories/wallpaper.raw", new byte[5623]), "壁纸尺寸无效");
        foreach (string file in new[] { "developer/server.url", "profile/repository.url", "profile/core-repository.url" })
            Reject(file + " rejects URL credentials", f => f.Put(file, "https://user:pass@offline.invalid/\n"), file.StartsWith("developer/") ? "发布地址无效" : "仓库地址无效");
        foreach (string path in new[] { "", "/absolute", "../outside", "a/../b", "a/./b", "a//b", "a/", "C:/outside", "a\\b", "a:stream", "a b", "a\nb", "a/\u4e2d" })
            Test("FileAt rejects " + System.Text.Json.JsonSerializer.Serialize(path), f => Check.Throws<InvalidDataException>(() => f.Payload.FileAt(path), "路径无效"));
        Test("snapshot copies exact bytes and isolates later source mutation", f => {
            f.Payload.Validate(); string destination = Path.Combine(f.Root, "snapshot"); var snapshot = f.Payload.CreateSnapshot(destination);
            Check.True(snapshot.Files.SequenceEqual(f.Payload.Files), "Snapshot membership differs");
            foreach (string name in snapshot.Files.Append("SHA256SUMS")) Check.Bytes(File.ReadAllBytes(f.At(name)), File.ReadAllBytes(snapshot.FileAt(name)));
            string original = Payload.Hash(snapshot.FileAt("enrollment/release/artifacts/C1ancher"));
            f.Put("enrollment/release/artifacts/C1ancher", "changed after snapshot"); File.Delete(f.At("developer/README.md")); f.Put("extra.txt", "late addition");
            snapshot.Validate(); Check.Equal(original, Payload.Hash(snapshot.FileAt("enrollment/release/artifacts/C1ancher")));
            File.WriteAllText(snapshot.FileAt("developer/README.md"), "snapshot-only mutation");
            Check.True(!File.Exists(f.At("developer/README.md")), "Snapshot mutation affected source");
            Check.Throws<IOException>(snapshot.Validate, "安装包文件校验失败");
        });
        Test("snapshot refuses existing destination without overwrite", f => {
            string destination = Path.Combine(f.Root, "snapshot"); Directory.CreateDirectory(destination); File.WriteAllText(Path.Combine(destination, "keep"), "keep");
            Check.Throws<IOException>(() => f.Payload.CreateSnapshot(destination), "已存在"); Check.Equal("keep", File.ReadAllText(Path.Combine(destination, "keep")));
        });
        Test("invalid source fails before snapshot directory creation", f => {
            f.Put("developer/README.md", "tampered"); string destination = Path.Combine(f.Root, "snapshot");
            Check.Throws<IOException>(() => f.Payload.CreateSnapshot(destination)); Check.True(!Directory.Exists(destination), "Invalid snapshot destination created");
        });
        Test("payload rejects symbolic-link member", f => {
            string link = f.At("developer/README.md"); string target = Path.Combine(f.Root, "link-target.txt"); File.Copy(link, target); File.Delete(link);
            try { File.CreateSymbolicLink(link, target); }
            catch (UnauthorizedAccessException e) { throw new SkipException("Symbolic-link privilege unavailable: " + e.Message); }
            catch (IOException e) when ((e.HResult & 0xffff) == 1314) { throw new SkipException("Windows symbolic-link privilege unavailable"); }
            Check.Throws<IOException>(f.Payload.Validate, "链接");
        });
    }
    static void RegisterCommands()
    {
        (string, string[]) Child(string mode)
        {
            string host = Environment.ProcessPath ?? throw new IOException("Test executable missing");
            string[] args = Path.GetFileNameWithoutExtension(host).Equals("dotnet", StringComparison.OrdinalIgnoreCase)
                ? [typeof(Program).Assembly.Location, "--runner-child", mode] : ["--runner-child", mode];
            return (host, args);
        }
        TestAsync("local command captures both streams and preserves exit code", async _ => {
            var (host, args) = Child("output");
            var result = await new CommandRunner().RunAsync(host, args, TimeSpan.FromSeconds(15));
            Check.Equal(7, result.ExitCode);
            Check.True(result.Output.Contains("runner stdout") && result.Output.Contains("runner stderr"), "Command output was lost");
        });
        foreach (bool cancel in new[] { false, true })
            TestAsync("local command bounded cleanup on " + (cancel ? "cancellation" : "timeout"), async _ => {
                var (host, args) = Child("wait");
                using var cancellation = new CancellationTokenSource();
                if (cancel) cancellation.CancelAfter(500);
                var watch = Stopwatch.StartNew();
                await Check.ThrowsAsync<IOException>(() => new CommandRunner().RunAsync(host, args,
                    cancel ? TimeSpan.FromSeconds(30) : TimeSpan.FromMilliseconds(500), cancellation.Token), "本地命令等待超时");
                Check.True(watch.Elapsed < TimeSpan.FromSeconds(8), "Local process cleanup hung");
            });
        Test("ADB device parser preserves serial and state", _ => {
            var devices = AdbClient.ParseDevices("* daemon started successfully *\r\nList of devices attached\r\nA\tdevice product:x\nB unauthorized\nC offline\nD recovery\nE sideload\nnoise nonsense\n");
            Check.Equal("A,B,C,D,E", string.Join(',', devices.Select(d => d.Serial))); Check.Equal("device,unauthorized,offline,recovery,sideload", string.Join(',', devices.Select(d => d.State)));
        });
        foreach (string serial in new[] { "", " ", "-s", "bad\nserial", "bad\0serial" })
            TestAsync("ADB rejects invalid serial " + System.Text.Json.JsonSerializer.Serialize(serial), async f => {
                var runner = new FakeCommandRunner(f); var adb = new AdbClient(f.At("tools/adb.exe"), runner);
                await Check.ThrowsAsync<IOException>(() => adb.RunAsync(serial, ["root"]), "序列号无效"); Check.Equal(0, runner.Calls.Count);
            });
        Test("shell quote handles apostrophe without execution", _ => Check.Equal("'a'\"'\"'b;$(touch nope)'", AdbClient.Quote("a'b;$(touch nope)")));
        TestAsync("shell accepts only matching successful exit marker", async f => {
            var runner = new FakeCommandRunner(f); var adb = new AdbClient(f.At("tools/adb.exe"), runner);
            Check.Equal("0", await adb.ShellAsync(FakeCommandRunner.Serial, "id -u"));
            Check.True(runner.Calls.Single().Args.Length == 4, "Shell command must remain one argument");
        });
        foreach (string ending in new[] { "\n", "\r\n", "\r\r\n" })
        {
            TestAsync("shell normalizes transport ending " + System.Text.Json.JsonSerializer.Serialize(ending), async f => {
                var runner = new FakeCommandRunner(f) { Override = c => {
                    var reply = FakeCommandRunner.Reply(c, "first\nsecond");
                    return reply with { Output = reply.Output.Replace("\n", ending) };
                } };
                Check.Equal("first\nsecond", await new AdbClient(f.At("tools/adb.exe"), runner).ShellAsync(FakeCommandRunner.Serial, "id -u"));
            });
        }
        foreach (string ending in new[] { "\n", "\r\n", "\r\r\n" })
        foreach (string mode in new[] { "missing marker", "foreign marker", "remote failure", "host failure" })
            TestAsync("shell rejects " + mode + " with " + System.Text.Json.JsonSerializer.Serialize(ending), async f => {
                var runner = new FakeCommandRunner(f) { Override = c => {
                    var reply = mode switch {
                        "missing marker" => new CommandResult(0, "0\n"),
                        "foreign marker" => new CommandResult(0, "0\n__C1_SETUP_EXIT_00000000000000000000000000000000=0\n"),
                        "remote failure" => FakeCommandRunner.Reply(c, "", 1), _ => FakeCommandRunner.Reply(c, "", 0, 1) };
                    return reply with { Output = reply.Output.Replace("\n", ending) };
                } };
                await Check.ThrowsAsync<IOException>(() => new AdbClient(f.At("tools/adb.exe"), runner).ShellAsync(FakeCommandRunner.Serial, "id -u"), "设备命令未成功");
            });
        TestAsync("root request and reconnection stay on selected serial", async f => {
            var runner = new FakeCommandRunner(f) { InitiallyRoot = false }; var adb = new AdbClient(f.At("tools/adb.exe"), runner);
            Check.Equal(2, (await adb.DevicesAsync()).Count);
            await adb.EnsureRootAsync(FakeCommandRunner.Serial, runner.Logs.Add);
            Check.True(runner.RootRequested, "Root request missing");
            Check.Equal("shell,root,shell", string.Join(',', runner.Calls.Skip(1).Select(c => c.Operation)));
            Check.True(runner.Calls.Skip(1).All(c => c.Args[0] == "-s" && c.Args[1] == FakeCommandRunner.Serial), "Serial changed after root");
            Check.Equal(TimeSpan.FromSeconds(5), runner.Calls.Last().Timeout);
        });
        TestAsync("already-root device does not restart adbd", async f => {
            var runner = new FakeCommandRunner(f); await new AdbClient(f.At("tools/adb.exe"), runner).EnsureRootAsync(FakeCommandRunner.Serial, runner.Logs.Add);
            Check.Equal(1, runner.Calls.Count); Check.True(!runner.RootRequested, "Unnecessary root restart");
        });
        TestAsync("denied root fails without switching devices", async f => {
            var runner = new FakeCommandRunner(f) { InitiallyRoot = false, Override = c => c.Operation == "root" ? new CommandResult(0, "adbd cannot run as root in production builds") : null };
            await Check.ThrowsAsync<IOException>(() => runner.Engine.ProbeAsync(FakeCommandRunner.Serial), "未开放 root"); Check.Equal(2, runner.Calls.Count); Check.NoRemoval(runner);
        });
        TestAsync("push rejects post-upload hash mismatch", async f => {
            var runner = new FakeCommandRunner(f) { Override = c => c.Shell.StartsWith("sha256sum '") ? FakeCommandRunner.Reply(c, new string('0', 64) + "  target") : null };
            await Check.ThrowsAsync<IOException>(() => new AdbClient(f.At("tools/adb.exe"), runner).PushAsync(FakeCommandRunner.Serial, f.At("developer/README.md"), "/test-target", Payload.Hash(f.At("developer/README.md"))), "上传后校验不一致");
            Check.Equal(2, runner.Calls.Count); Check.Equal(TimeSpan.FromSeconds(180), runner.Calls[0].Timeout);
        });
    }
    static void AssertNoConfiguration(FakeCommandRunner runner, Fixture f)
    {
        Check.NoRemoval(runner);
        Check.True(!runner.Calls.Any(c =>
            (c.Shell.StartsWith("sh '") && c.Shell.Contains("/device-setup.sh' ")) ||
            c.Shell.Contains("/device-open-adb.sh' install") || c.Shell.Contains("/device-core-enroll.sh' install") ||
            c.Shell.Contains("device-core-enroll.sh' verify")), "Metadata/hash failure reached helper, USB configuration or enrollment");
        Check.True(!File.Exists(Path.Combine(f.Evidence, "result.txt")), "Metadata/hash failure recorded success");
    }
    static void AssertNoSuccess(FakeCommandRunner runner, Fixture f)
    {
        Check.True(!File.Exists(Path.Combine(f.Evidence, "result.txt")), "Failure recorded result.txt");
        Check.True(!runner.Logs.Any(s => s.StartsWith("安装完成：") || s.StartsWith("安装已写入并验证运行状态")), "Failure logged installation success");
    }
    static bool IsRunningCheck(Call c) => c.Shell == RunningProcessProbe.Command;
    static bool IsHelper(Call c, string action) => c.Shell.EndsWith("/device-setup.sh' " + action, StringComparison.Ordinal)
        || c.Shell.Contains("/device-setup.sh' " + action + " ", StringComparison.Ordinal);
    static void RegisterAcceptance()
    {
        foreach (string mode in new[] { "remote failure", "missing marker", "duplicate marker" })
            TestAsync("remove-factory failure stops acceptance without success: " + mode, async f => {
                var runner = new FakeCommandRunner(f) { Override = c => IsHelper(c, "remove-factory") ? mode switch {
                    "remote failure" => FakeCommandRunner.Reply(c, "simulated removal failure", 1),
                    "missing marker" => FakeCommandRunner.Reply(c, ""),
                    _ => FakeCommandRunner.Reply(c, "C1SETUP_OK remove-factory\nC1SETUP_OK remove-factory") } : null };
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, true));
                Check.Equal(1, runner.Calls.Count(c => IsHelper(c, "remove-factory")));
                Check.True(!runner.Calls.Any(c => IsHelper(c, "verify") || c.Operation == "reboot"), "Removal failure continued acceptance");
                AssertNoSuccess(runner, f);
            });
        foreach (bool running in new[] { true, false })
            TestAsync("confirmed enrollment retry uses fresh verifier and starts core: initially running=" + running, async f => {
                var runner = new FakeCommandRunner(f) { CoreRunning = running };
                await runner.Engine.InstallAsync(FakeCommandRunner.Serial, false);
                Check.True(!runner.Calls.Any(c => c.Shell.Contains("/storage/c1/update/enrollment/bundle/") || c.Shell.Contains("/device-core-enroll.sh' install ") || c.Shell.Contains("apply.pid")), "Confirmed core was re-enrolled or used historical staging");
                int verify = runner.Calls.FindIndex(c => c.Shell.EndsWith("/enrollment/device-core-enroll.sh' verify"));
                int start = runner.Calls.FindIndex(c => IsHelper(c, "start-core"));
                int process = runner.Calls.FindIndex(IsRunningCheck);
                int remove = runner.Calls.FindIndex(c => IsHelper(c, "remove-factory"));
                Check.True(verify > 0 && start == verify + 1 && process == start + 1 && remove > process, "Fresh verify/start/running hash/removal order differs");
                Check.True(runner.CoreStartRequested && runner.CoreRunning, "Stopped confirmed core did not start");
                Check.Equal(1, runner.Calls.Count(c => IsHelper(c, "start-core")));
            });
        TestAsync("start-core remote failure blocks process check and removal", async f => {
            var runner = new FakeCommandRunner(f) { CoreRunning = false, Override = c => IsHelper(c, "start-core") ? FakeCommandRunner.Reply(c, "bootstrap failed", 1) : null };
            await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "bootstrap failed");
            Check.True(!runner.Calls.Any(IsRunningCheck), "Continued after failed start helper");
            Check.NoRemoval(runner); AssertNoSuccess(runner, f);
        });
        foreach (string state in new[] { "stopped", "wrong image", "duplicate images" })
            TestAsync("start-core success still requires actual unique running hash: " + state, async f => {
                var runner = new FakeCommandRunner(f) { CoreRunning = state != "stopped", StartProducesRunningCore = false };
                runner.Override = c => IsRunningCheck(c) && state != "stopped" ? FakeCommandRunner.Reply(c,
                    runner.RunningSnapshot(wrongImage: state == "wrong image", duplicate: state == "duplicate images")) : null;
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "唯一且匹配");
                Check.True(runner.CoreStartRequested, "Start action not reached");
                Check.Equal(30, runner.Calls.Count(IsRunningCheck));
                Check.NoRemoval(runner); AssertNoSuccess(runner, f);
            });
        foreach (bool reboot in new[] { false, true })
            TestAsync("success evidence follows final cleanup: reboot=" + reboot, async f => {
                var runner = new FakeCommandRunner(f); bool cleanup = false;
                runner.Override = c => {
                    if (c.Shell.StartsWith("rm -rf '/storage/c1-installer-")) { AssertNoSuccess(runner, f); cleanup = true; }
                    return null;
                };
                await runner.Engine.InstallAsync(FakeCommandRunner.Serial, reboot);
                Check.True(cleanup, "Remote cleanup not reached");
                Check.True(runner.Calls.Last().Shell.StartsWith("rm -rf '/storage/c1-installer-"), "Cleanup must be final device command");
                Check.True(File.ReadAllText(Path.Combine(f.Evidence, "result.txt")).Contains("reboot_verified=" + reboot), "Wrong reboot evidence");
                Check.True(runner.Logs.Last().StartsWith(reboot ? "安装完成：" : "安装已写入并验证运行状态"), "Final success log missing");
                Check.Equal(reboot ? 1 : 0, runner.Calls.Count(c => c.Operation == "reboot"));
                if (!reboot) return;
                int rebootIndex = runner.Calls.FindIndex(c => c.Operation == "reboot");
                Check.Equal("cat /proc/sys/kernel/random/boot_id", runner.Calls[rebootIndex - 1].Shell);
                var acceptance = runner.Calls.Skip(rebootIndex + 1).ToArray();
                Check.Equal(10, acceptance.Length);
                Check.Equal("cat /proc/sys/kernel/random/boot_id", acceptance[0].Shell);
                Check.Equal(TimeSpan.FromSeconds(10), acceptance[0].Timeout);
                Check.True(IsHelper(acceptance[1], "verify") && IsRunningCheck(acceptance[2]) && IsRunningCheck(acceptance[3]), "Post-boot helper and two actual process checks missing");
                Check.True(acceptance.Skip(4).Take(3).All(c => c.Shell.StartsWith("sha256sum '/usr/data/c1/")), "Post-boot repository verification missing");
                Check.True(acceptance[7].Shell.Contains("/usb/device-open-adb.sh' verify "), "Post-boot USB verification missing");
                Check.True(runner.Calls.All(c => c.Args[0] == "-s" && c.Args[1] == FakeCommandRunner.Serial), "Acceptance switched selected serial");
            });
        foreach (string failure in new[] { "nonzero", "timeout" })
            TestAsync("reboot command " + failure + " fails before polling", async f => {
                var runner = new FakeCommandRunner(f) { Override = c => {
                    if (c.Operation != "reboot") return null;
                    if (failure == "timeout") throw new IOException("simulated reboot timeout");
                    return new CommandResult(17, "stdout detail\nstderr reboot refused");
                } };
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, true), failure == "timeout" ? "simulated reboot timeout" : "stderr reboot refused");
                if (failure == "nonzero") Check.True(runner.Logs.Any(s => s.Contains("退出码 17") && s.Contains("stdout detail")), "Reboot exit code/output missing");
                Check.Equal(1, runner.Calls.Count(c => c.Shell == "cat /proc/sys/kernel/random/boot_id"));
                Check.Equal(0, runner.Clock.Delays.Count(d => d == TimeSpan.FromSeconds(3)));
                AssertNoSuccess(runner, f);
            });
        foreach (bool before in new[] { true, false })
        foreach (string identity in new[] { "", "garbage", "11111111222243338444555555555555", "{11111111-2222-4333-8444-555555555555}", "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE", "00000000-0000-0000-0000-000000000000", "11111111-2222-4333-8444-555555555555\nextra" })
            TestAsync("boot identity rejects malformed " + (before ? "baseline: " : "return: ") + System.Text.Json.JsonSerializer.Serialize(identity), async f => {
                var runner = new FakeCommandRunner(f);
                if (before) runner.BeforeBootId = identity; else runner.AfterBootId = identity;
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, true), "启动编号无效");
                Check.Equal(before ? 0 : 1, runner.Calls.Count(c => c.Operation == "reboot"));
                Check.Equal(before ? 1 : 2, runner.Calls.Count(c => c.Shell == "cat /proc/sys/kernel/random/boot_id"));
                Check.Equal(1, runner.Calls.Count(c => IsHelper(c, "verify")), "Invalid boot reached post-reboot acceptance");
                AssertNoSuccess(runner, f);
            });
        foreach (bool disconnect in new[] { false, true })
            TestAsync("reboot timeout rejects " + (disconnect ? "continued disconnection" : "unchanged canonical identity"), async f => {
                var runner = new FakeCommandRunner(f); runner.AfterBootId = runner.BeforeBootId;
                runner.Override = c => disconnect && runner.RebootRequested && c.Shell == "cat /proc/sys/kernel/random/boot_id" ? FakeCommandRunner.Reply(c, "offline", 1) : null;
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, true), "未在时限内恢复");
                Check.Equal(100, runner.Clock.Delays.Count(d => d == TimeSpan.FromSeconds(3)));
                Check.Equal(101, runner.Calls.Count(c => c.Shell == "cat /proc/sys/kernel/random/boot_id"));
                Check.Equal(1, runner.Calls.Count(c => IsHelper(c, "verify")));
                AssertNoSuccess(runner, f);
            });
        TestAsync("reboot tolerates transport loss and unchanged identity before genuine new boot", async f => {
            var runner = new FakeCommandRunner(f); int polls = 0;
            runner.Override = c => {
                if (!runner.RebootRequested || c.Shell != "cat /proc/sys/kernel/random/boot_id") return null;
                return ++polls switch { 1 => FakeCommandRunner.Reply(c, "offline", 1), 2 => FakeCommandRunner.Reply(c, runner.BeforeBootId), _ => null };
            };
            await runner.Engine.InstallAsync(FakeCommandRunner.Serial, true);
            Check.Equal(3, polls); Check.Equal(3, runner.Clock.Delays.Count(d => d == TimeSpan.FromSeconds(3)));
            Check.True(File.ReadAllText(Path.Combine(f.Evidence, "result.txt")).Contains("reboot_verified=True"), "Reboot acceptance missing");
        });
        foreach (string phase in new[] { "no reboot", "before reboot", "after reboot" })
        foreach (string gate in new[] { "helper exit", "helper missing marker", "helper duplicate marker", "running exit", "running absent", "running mismatch", "running duplicate", "repository key", "repository URL", "core repository URL", "USB", "cleanup" })
        {
            if (gate == "USB" && phase != "after reboot" || gate == "cleanup" && phase == "before reboot") continue;
            TestAsync("post-removal failure records no success: " + phase + "/" + gate, async f => {
                var runner = new FakeCommandRunner(f); bool injected = false;
                runner.Override = c => {
                    if (!runner.Calls.Any(call => IsHelper(call, "remove-factory")) || (phase == "after reboot") != runner.RebootRequested) return null;
                    bool matches = gate switch {
                        "helper exit" or "helper missing marker" or "helper duplicate marker" => IsHelper(c, "verify"),
                        "running exit" or "running absent" or "running mismatch" or "running duplicate" => IsRunningCheck(c),
                        "repository key" => c.Shell == "sha256sum '/usr/data/c1/pkg/repository.ed25519.pub'",
                        "repository URL" => c.Shell == "sha256sum '/usr/data/c1/pkg/repository.url'",
                        "core repository URL" => c.Shell == "sha256sum '/usr/data/c1/update/repository.url'",
                        "USB" => c.Shell.Contains("/usb/device-open-adb.sh' verify "),
                        _ => c.Shell.StartsWith("rm -rf '/storage/c1-installer-") };
                    if (!matches) return null;
                    injected = true;
                    return gate switch {
                        "helper missing marker" or "running absent" => FakeCommandRunner.Reply(c, ""),
                        "helper duplicate marker" => FakeCommandRunner.Reply(c, "C1SETUP_OK verify\nC1SETUP_OK verify"),
                        "running mismatch" => FakeCommandRunner.Reply(c, runner.RunningSnapshot(wrongImage: true)),
                        "repository key" or "repository URL" or "core repository URL" => FakeCommandRunner.Reply(c, new string('0', 64) + "  mismatch"),
                        "running duplicate" => FakeCommandRunner.Reply(c, runner.RunningSnapshot(duplicate: true)),
                        _ => FakeCommandRunner.Reply(c, "simulated final gate failure", 1) };
                };
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, phase != "no reboot"));
                Check.True(injected, "Final gate failure not reached"); AssertNoSuccess(runner, f);
                Check.Equal(phase == "after reboot" ? 1 : 0, runner.Calls.Count(c => c.Operation == "reboot"));
                Check.True(File.Exists(Path.Combine(f.Evidence, "device-apply.log")), "Failure diagnostics missing");
                Check.True(runner.Logs.Any(s => s.StartsWith("保留设备临时安装目录供恢复：")), "Staging recovery log missing");
                if (gate != "cleanup") Check.True(!runner.Calls.Any(c => c.Shell.StartsWith("rm -rf '/storage/c1-installer-")), "Failure continued to cleanup");
            });
        }
    }
    static void RegisterEngine()
    {
        foreach (string path in new[] { "", "/storage/c1-installer-" + new string('0', 31), "/storage/c1-installer-" + new string('0', 33),
            "/storage/c1-installer-" + new string('A', 32), "/storage/c1-installer-" + new string('0', 32) + "\n",
            "/storage/c1-installer-" + new string('0', 32) + "/enrollment", "/storage/c1/update/enrollment", "/tmp/stage", "'; chmod -R 777 /; '" })
            Test("metadata constructor rejects non-generated path " + System.Text.Json.JsonSerializer.Serialize(path), _ =>
                Check.Throws<ArgumentException>(() => InstallerEngine.EnrollmentMetadataCommand(path)));
        TestAsync("metadata preparation fixes only current 3 directories and 13 files, then rehashes", async f => {
            // The minimal fixture normally stages 30 files; add inert data to exercise the observed 31-file package.
            f.Put("accessories/metadata-order-sentinel.txt", "inert upload-order fixture\n"); f.RefreshSums();
            var runner = new FakeCommandRunner(f) { BeforeMetadata = nodes => {
                Check.Equal(16, nodes.Count);
                foreach (string path in nodes.Keys.ToArray()) nodes[path] = nodes[path] with { Uid = 123, Gid = 456 };
            } };
            await runner.Engine.InstallAsync(FakeCommandRunner.Serial, false);
            Check.True(runner.EnrollmentPrepared, "Metadata was never prepared");
            Check.Equal(32, runner.MetadataMutations);
            Check.Equal(13, runner.EnrollmentRehashed.Count);
            Check.True(FakeCommandRunner.EnrollmentFiles.Order().SequenceEqual(f.Payload.Files.Where(p => p.StartsWith("enrollment/")).Order()), "Metadata allowlist differs from payload");
            foreach (var node in runner.Metadata.Values) {
                Check.Equal(node.Kind == "directory" ? 700 : 600, node.Mode); Check.Equal(0, node.Uid); Check.Equal(0, node.Gid);
            }
            Check.True(runner.Logs.Any(s => s.Contains("准备本次核心 enrollment 安全权限")) && runner.Logs.Any(s => s.Contains("安全权限准备成功，13 个文件 SHA-256")), "Missing metadata progress/success logs");
        });
        foreach (string relative in FakeCommandRunner.EnrollmentDirectories.Concat(FakeCommandRunner.EnrollmentFiles))
        foreach (string defect in FakeCommandRunner.EnrollmentDirectories.Contains(relative)
            ? new[] { "symlink", "regular", "fifo", "missing" } : new[] { "symlink", "hardlink", "directory", "fifo", "socket", "device", "missing" })
            TestAsync("metadata precheck rejects " + relative + ": " + defect + " before any chmod/chown", async f => {
                bool injected = false;
                var runner = new FakeCommandRunner(f) { Factory = true, BeforeMetadata = nodes => {
                    string path = nodes.Keys.Single(p => p.EndsWith("/" + relative, StringComparison.Ordinal));
                    if (defect == "missing") nodes.Remove(path);
                    else nodes[path] = defect == "hardlink" ? nodes[path] with { Links = 2 } : nodes[path] with { Kind = defect };
                    injected = true;
                } };
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "设备命令未成功");
                Check.True(injected, "Metadata precheck fault not reached");
                Check.Equal(0, runner.MetadataMutations); Check.True(!runner.EnrollmentPrepared, "Rejected metadata marked prepared");
                AssertNoConfiguration(runner, f);
            });
        TestAsync("every metadata precheck, chown, chmod and stat failure blocks configuration", async f => {
            var baseline = new FakeCommandRunner(f); await baseline.Engine.InstallAsync(FakeCommandRunner.Serial, false);
            int count = baseline.MetadataSteps.Count;
            Check.True(count > 90, "Metadata baseline omitted protective steps");
            for (int index = 1; index < count; index++) {
                Directory.Delete(f.Evidence, true);
                var runner = new FakeCommandRunner(f) { Factory = true, MetadataFailureIndex = index };
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "设备命令未成功");
                Check.Equal(index + 1, runner.MetadataSteps.Count, "Metadata continued after failed operation");
                Check.True(!runner.EnrollmentPrepared, "Failed operation marked metadata prepared");
                AssertNoConfiguration(runner, f);
            }
            Console.WriteLine($"  Checked all {count - 1} metadata operation failure positions (fake ADB only).");
        });
        foreach (string relative in FakeCommandRunner.EnrollmentDirectories.Concat(FakeCommandRunner.EnrollmentFiles))
        foreach (string field in new[] { "mode", "uid", "gid" })
            TestAsync("metadata post-stat rejects incorrect " + field + ": " + relative, async f => {
                bool injected = false;
                var runner = new FakeCommandRunner(f) { Factory = true, BeforeMetadataStep = (step, nodes) => {
                    if (!step.Contains("stat -c '%a:%u:%g'") || !step.Contains("/" + relative + "'")) return;
                    string path = nodes.Keys.Single(p => p.EndsWith("/" + relative, StringComparison.Ordinal));
                    nodes[path] = field switch { "mode" => nodes[path] with { Mode = 666 }, "uid" => nodes[path] with { Uid = 1 }, _ => nodes[path] with { Gid = 1 } };
                    injected = true;
                } };
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "设备命令未成功");
                Check.True(injected, "Stat mismatch not reached"); AssertNoConfiguration(runner, f);
            });
        foreach (string relative in FakeCommandRunner.EnrollmentFiles)
            TestAsync("post-metadata hash mismatch blocks configuration: " + relative, async f => {
                bool injected = false;
                var runner = new FakeCommandRunner(f) { Factory = true };
                runner.Override = c => {
                    if (!runner.EnrollmentPrepared || !c.Shell.StartsWith("sha256sum '") || !c.Shell.EndsWith("/" + relative + "'", StringComparison.Ordinal)) return null;
                    injected = true; return FakeCommandRunner.Reply(c, new string('0', 64) + "  mismatch");
                };
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "权限准备后文件校验不一致：" + relative);
                Check.True(injected, "Post-metadata hash fault not reached");
                Check.Equal(Array.IndexOf(FakeCommandRunner.EnrollmentFiles, relative), runner.EnrollmentRehashed.Count, "Hash verification continued after mismatch");
                AssertNoConfiguration(runner, f);
                Check.True(!runner.Logs.Any(s => s.Contains("安全权限准备成功，13 个文件 SHA-256")), "Hash mismatch logged success");
            });
        TestAsync("factory core execution chmod uses fail-closed conjunction", async f => {
            var runner = new FakeCommandRunner(f) { Factory = true };
            await runner.Engine.InstallAsync(FakeCommandRunner.Serial, false);
            Check.True(runner.Calls.Any(c => c.Shell.StartsWith("chmod 700 '") && c.Shell.Contains("/enroll.sh' && ")), "Missing fail-closed execution chmod");
        });
        TestAsync("factory core execution chmod failure stops before background enrollment", async f => {
            bool injected = false;
            var runner = new FakeCommandRunner(f) { Factory = true, Override = c => {
                if (!c.Shell.StartsWith("chmod 700 '") || !c.Shell.Contains("/device-core-enroll.sh' install ")) return null;
                Check.True(c.Shell.Contains("/enroll.sh' && "), "chmod failure could fall through to execution");
                injected = true; return FakeCommandRunner.Reply(c, "test-only chmod failure", 1);
            } };
            await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "设备命令未成功");
            Check.True(injected, "Execution chmod failure not reached"); Check.NoRemoval(runner);
            Check.True(!runner.Calls.Any(c => c.Shell.Contains("apply.pid") || c.Shell.Contains("device-core-enroll.sh' verify")), "Continued after failed execution chmod");
            Check.True(!File.Exists(Path.Combine(f.Evidence, "result.txt")), "Execution chmod failure recorded success");
        });
        TestAsync("all eight uploaded shell syntax checks precede preflight in manifest order", async f => {
            var runner = new FakeCommandRunner(f); await runner.Engine.InstallAsync(FakeCommandRunner.Serial, false);
            var syntax = runner.Calls.Select((call, index) => (call, index)).Where(x => x.call.Shell.StartsWith("sh -n ")).ToArray();
            Check.Equal(SyntaxScripts.Length, syntax.Length);
            for (int i = 0; i < syntax.Length; i++)
                Check.True(syntax[i].call.Shell.EndsWith("/" + SyntaxScripts[i] + "'", StringComparison.Ordinal), "Syntax check order/file differs at " + i);
            int preflight = runner.Calls.FindIndex(c => c.Shell.Contains("/device-setup.sh' preflight"));
            Check.True(preflight > syntax[^1].index, "Preflight ran before all syntax checks completed");
            int uploadedCount = f.Payload.Files.Count(p => p.StartsWith("enrollment/") || p.StartsWith("profile/") || p.StartsWith("accessories/") || p == "device-setup.sh" || p.StartsWith("usb/"));
            Check.Equal(uploadedCount, runner.Calls.Take(syntax[0].index).Count(c => c.Operation == "push"), "Syntax checking began before every staged file was uploaded");
            Check.True(runner.Calls[syntax[0].index - 1].Shell.StartsWith("sha256sum '"), "Final upload verification did not precede syntax checking");
            Check.True(runner.Calls.Skip(syntax[0].index).Take(syntax[^1].index - syntax[0].index + 1).All(c => c.Shell.StartsWith("sh -n ")), "Other device work interrupted syntax checks");
            int metadata = runner.Calls.FindIndex(c => c.Shell.StartsWith(": c1-enrollment-metadata-v1"));
            Check.True(metadata > syntax[^1].index && metadata < preflight, "Metadata preparation must follow syntax checks and precede preflight");
            var rehash = runner.Calls.Skip(metadata + 1).Take(preflight - metadata - 1).ToArray();
            Check.Equal(13, rehash.Length);
            Check.True(rehash.Select(c => c.Shell).SequenceEqual(FakeCommandRunner.EnrollmentFiles.Select(p => "sha256sum '" + runner.Metadata.Keys.First().Split("/enrollment")[0] + "/" + p + "'")), "Post-metadata hashes must cover exactly the fixed 13 files in order");
        });
        foreach (string file in SyntaxScripts)
            TestAsync("syntax failure in " + file + " blocks all helpers and installation", async f => {
                bool injected = false;
                var runner = new FakeCommandRunner(f) { Factory = true, Override = c => {
                    if (!c.Shell.StartsWith("sh -n ") || !c.Shell.EndsWith("/" + file + "'", StringComparison.Ordinal)) return null;
                    injected = true; return FakeCommandRunner.Reply(c, "test-only shell syntax error", 2);
                } };
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "设备命令未成功");
                Check.True(injected, "Syntax failure was not reached");
                Check.Equal(Array.IndexOf(SyntaxScripts, file) + 1, runner.Calls.Count(c => c.Shell.StartsWith("sh -n ")), "Continued syntax checks after failure");
                Check.True(!runner.Calls.Any(c =>
                    (c.Shell.Contains("/device-setup.sh' ") && !c.Shell.StartsWith("sh -n ")) ||
                    c.Shell.Contains("/device-open-adb.sh' install") || c.Shell.Contains("/device-core-enroll.sh' install") ||
                    c.Shell.Contains("device-core-enroll.sh' verify") || c.Operation == "reboot"), "Syntax failure reached a helper, USB install, enrollment or reboot");
                Check.NoRemoval(runner);
                Check.True(!File.Exists(Path.Combine(f.Evidence, "result.txt")), "Syntax failure recorded success");
            });
        foreach (var (file, defect, edit) in InvalidShellCases())
            TestAsync("InstallAsync rejects " + file + ": " + defect + " before any ADB", async f => {
                f.EditShell(file, edit);
                var runner = new FakeCommandRunner(f);
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), ShellFormatError(file));
                Check.Equal(0, runner.Calls.Count, "Malformed shell input reached ADB");
                Check.NoRemoval(runner);
                Check.True(!Directory.Exists(f.Evidence), "Invalid payload created installation evidence");
            });
        foreach (bool timeout in new[] { false, true })
            TestAsync("upload " + (timeout ? "timeout" : "disconnect") + " names file, stops and never retries", async f => {
                string? failedFile = null;
                var runner = new FakeCommandRunner(f) { Override = c => {
                    if (c.Operation != "push") return null;
                    failedFile = Path.GetFileName(c.Args[3]);
                    if (timeout) throw new IOException("本地命令等待超时或已停止");
                    return new CommandResult(1, "error: closed");
                } };
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "文件传输或校验失败");
                Check.Equal(1, runner.Calls.Count(c => c.Operation == "push"));
                Check.True(failedFile != null && runner.Logs.Any(line => line.Contains("上传并校验：") && line.Contains(failedFile)), "Missing active file progress");
                Check.True(runner.Logs.Any(line => line.Contains("电脑端安装流程已停止") && line.Contains(failedFile!)), "Missing immediate contextual failure log");
                Check.True(!runner.Logs.Any(line => line.Contains("上传校验完成")), "Failed upload reported complete");
                Check.True(!runner.Calls.Any(c => c.Operation == "reboot" || c.Shell.Contains("backup-factory") || c.Shell.Contains("device-core-enroll.sh' install")), "Continued after failed upload");
                Check.NoRemoval(runner);
                Check.True(!File.Exists(Path.Combine(f.Evidence, "result.txt")), "Failed upload recorded success");
            });
        foreach (string mode in new[] { "missing directory", "missing enrollment", "missing signature", "missing tool", "tampered content" })
            TestAsync("InstallAsync " + mode + " fails before any ADB", async f => {
                switch (mode) {
                    case "missing directory": Directory.Delete(f.Payload.Root, true); break;
                    case "missing enrollment": Directory.Delete(f.At("enrollment"), true); f.Put("NOT_READY.txt", "External payload template; complete signed core required.\n"); f.RefreshSums(); break;
                    case "missing signature": File.Delete(f.At("enrollment/bootstrap.v1.sig")); f.RefreshSums(); break;
                    case "missing tool": File.Delete(f.At("tools/AdbWinApi.dll")); f.RefreshSums(); break;
                    default: f.Put("developer/README.md", "tampered"); break;
                }
                var runner = new FakeCommandRunner(f); await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false));
                Check.Equal(0, runner.Calls.Count, "Invalid payload reached ADB"); Check.NoRemoval(runner); Check.True(!Directory.Exists(f.Evidence), "Invalid payload created success evidence");
            });
        foreach (var (command, output, message) in new[] {
            ("cat /sys/kernel/config/usb_gadget/demo/strings/0x409/product; uname -m", "other-model\nmips", "设备型号不匹配"),
            ("cat /sys/kernel/config/usb_gadget/demo/strings/0x409/product; uname -m", "mp-d261\naarch64", "设备型号不匹配"),
            ("cat /proc/mounts", "/dev/root / ext4 rw,relatime 0 0", "只读状态"),
            ("sha256sum /etc/init.d/S90usb", new string('0', 64) + "  usb", "未知 USB"),
            ("sha256sum /etc/app_daemon", new string('0', 64) + "  daemon", "未知主页"),
            ("sha256sum /usr/data/c1/core/current/manifest.v1", new string('0', 64) + "  manifest", "已有其他核心版本"),
            ("sha256sum /etc/c1updater/core.ed25519.pub", new string('0', 64) + "  key", "核心公钥与安装包不同") })
            TestAsync("installation gate rejects " + message, async f => {
                var runner = new FakeCommandRunner(f) { Override = c => c.Shell == command ? FakeCommandRunner.Reply(c, output) : null };
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), message); Check.NoRemoval(runner);
                Check.True(!File.Exists(Path.Combine(f.Evidence, "result.txt")), "Rejected installation recorded success");
            });
        foreach (string state in new[] { "new", "existing" })
            TestAsync("developer tools " + state + " are verified data, never executed", async f => {
                f.Payload.Validate(); var runner = new FakeCommandRunner(f) { DeveloperExists = state == "existing" };
                string destination = await runner.Engine.InstallDeveloperToolsAsync(FakeCommandRunner.Serial);
                var files = f.Payload.Files.Where(n => n.StartsWith("developer/")).Order().ToArray();
                string identity = string.Join("\n", files.Select(n => n + ":" + Payload.Hash(f.At(n))));
                string digest = Convert.ToHexString(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(identity))).ToLowerInvariant();
                Check.Equal("/storage/mtp/C1DeveloperTools/" + digest, destination);
                var pushes = runner.Calls.Where(c => c.Operation == "push").ToArray(); Check.Equal(state == "new" ? files.Length : 0, pushes.Length);
                if (state == "new") Check.True(pushes.Select(c => c.Args[3]).Order().SequenceEqual(files.Select(f.At).Order()), "Developer upload set differs");
                foreach (var c in runner.Calls.Where(c => c.Operation == "shell")) {
                    Check.True(!c.Shell.Contains("chmod") && !c.Shell.Contains("sh '") && !c.Shell.Contains("--help") && !c.Shell.Contains("--version"), "Developer binary was made executable or invoked");
                    // Publisher names may occur only as inert operands to mkdir/test/sha256sum.
                    if (c.Shell.Contains("c1publish")) Check.True(c.Shell.StartsWith("test ! -L '") || c.Shell.StartsWith("sha256sum '"), "Publisher used as command");
                }
                Check.Equal(files.Length, runner.Calls.Count(c => c.Shell.StartsWith("sha256sum '") && c.Shell.Contains(destination)));
                Check.NoRemoval(runner);
            });
        foreach (string helper in new[] { "preflight", "prepare", "accessories", "start-core" })
            foreach (string mode in new[] { "missing", "duplicate", "wrong" })
                TestAsync(helper + " rejects " + mode + " success marker before removal", async f => {
                    var runner = new FakeCommandRunner(f) { Override = c => c.Shell.Contains("/device-setup.sh' " + helper) ? FakeCommandRunner.Reply(c, mode switch {
                        "missing" => "", "duplicate" => "C1SETUP_OK " + helper + "\nC1SETUP_OK " + helper, _ => "C1SETUP_OK different-step" }) : null };
                    await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "唯一成功标记"); Check.NoRemoval(runner);
                });
        TestAsync("default installation never backs up or pulls original factory files", async f => {
            var runner = new FakeCommandRunner(f); await runner.Engine.InstallAsync(FakeCommandRunner.Serial, false);
            Check.True(!runner.Calls.Any(c => c.Operation == "pull" || c.Shell.Contains("backup-factory") || c.Shell.Contains("restore-factory")), "Unexpected factory backup/restore operation");
            Check.True(!Directory.Exists(Path.Combine(f.Evidence, "factory-backup")), "Created factory backup directory");
            Check.True(File.ReadAllText(Path.Combine(f.Evidence, "result.txt")).Contains("factory_backup_created=false"), "Result must disclose no factory backup");
            Check.True(runner.Logs.Any(line => line.Contains("不创建原厂备份")), "Missing direct removal log");
        });
        TestAsync("complete simulated install establishes removal ordering", async f => {
            var runner = new FakeCommandRunner(f); await runner.Engine.InstallAsync(FakeCommandRunner.Serial, false);
            int remove = runner.Calls.FindIndex(c => c.Shell.Contains("/device-setup.sh' remove-factory")); Check.True(remove > 0, "Success path never reached removal");
            Check.Equal(1, runner.Calls.Count(c => c.Shell.Contains("/device-setup.sh' remove-factory")));
            foreach (string stage in new[] { "device-core-enroll.sh' verify", "C1RUN_CURRENT", "device-repository-config.sh", "C1DeveloperTools" })
                Check.True(runner.Calls.Take(remove).Any(c => c.Shell.Contains(stage)), "Removal preceded " + stage);
            Check.True(!runner.Calls.Any(c => c.Operation == "pull"), "No-backup workflow unexpectedly pulled files");
            Check.True(runner.Calls.Skip(remove + 1).Any(c => c.Shell.Contains("/device-setup.sh' verify")), "Post-removal verification missing");
            Check.True(File.ReadAllText(Path.Combine(f.Evidence, "result.txt")).Contains("reboot_verified=False"), "Unexpected result evidence");
            Check.True(!runner.Calls.Any(c => c.Operation == "reboot"), "No-reboot option ignored");
        });
        TestAsync("every command failure before remove-factory blocks removal", async f => {
            var baseline = new FakeCommandRunner(f); await baseline.Engine.InstallAsync(FakeCommandRunner.Serial, false);
            int remove = baseline.Calls.FindIndex(c => c.Shell.Contains("/device-setup.sh' remove-factory")); Check.True(remove > 50, "Baseline too short to exercise installation gates");
            for (int failureIndex = 0; failureIndex < remove; failureIndex++) {
                Directory.Delete(f.Evidence, true);
                int index = failureIndex; bool injected = false; var runner = new FakeCommandRunner(f);
                runner.Override = c => {
                    if (runner.Calls.Count != index + 1) return null;
                    injected = true; return c.Operation == "shell" ? FakeCommandRunner.Reply(c, "injected offline failure", 1) : new CommandResult(1, "injected offline failure");
                };
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false));
                Check.True(injected, "Failure point not reached: " + index); Check.NoRemoval(runner);
                Check.True(!File.Exists(Path.Combine(f.Evidence, "result.txt")), "Command failure recorded success at " + index);
                // Early probe failures do not create evidence; next iteration still needs a removable local directory.
                Directory.CreateDirectory(f.Evidence);
            }
            Console.WriteLine($"  Checked all {remove} command-failure positions before remove-factory (fake ADB only).");
        });
        TestAsync("factory enrollment background failure prevents removal", async f => {
            var runner = new FakeCommandRunner(f) { Factory = true, Override = c => c.Shell.StartsWith("if [ -e /storage/c1/update/enrollment/apply.pid ]") ? FakeCommandRunner.Reply(c, "failed") : null };
            await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "后台任务失败"); Check.NoRemoval(runner);
        });
    }
}
internal sealed class SkipException(string message) : Exception(message);
