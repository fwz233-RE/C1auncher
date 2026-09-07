using C1SlimInstaller;

namespace InstallerOfflineTests;

internal static partial class Program
{
    static void RegisterProcessProbe()
    {
        const string image = "/usr/data/c1/core/releases/7-1.2.3-test/artifacts/C1ancher";
        string App(Fixture f) => Payload.Hash(f.At("enrollment/release/artifacts/C1ancher"));
        string Launcher(Fixture f) => Payload.Hash(f.At("enrollment/release/artifacts/C1ancher-launcher"));
        Test("compiled process probe uses LF regardless of source checkout endings", f => {
            Check.True(!RunningProcessProbe.Command.Contains('\r'), "Compiled raw string contains device-incompatible CR");
            Check.True(RunningProcessProbe.Command.Contains("; do\n"), "Probe loop must have a POSIX newline");
            Check.Equal(RunningProcessProbe.CommandSource.ReplaceLineEndings("\n"), RunningProcessProbe.Command);
        });
        TestAsync("actual ADB process probe argument preserves LF and exit wrapper", async f => {
            var runner = new FakeCommandRunner(f);
            string output = await new AdbClient(f.At("tools/adb.exe"), runner).ShellAsync(FakeCommandRunner.Serial, RunningProcessProbe.Command);
            Check.Equal(runner.RunningSnapshot(), output);
            string wire = runner.Calls.Single().Args[3];
            Check.True(!wire.Contains('\r'), "CR reached outgoing ADB shell argument");
            Check.True(wire.StartsWith("( " + RunningProcessProbe.Command + " ); rc=$?; printf '"), "Exit wrapper changed");
        });
        TestAsync("process probe shell syntax failure blocks removal and success", async f => {
            var runner = new FakeCommandRunner(f);
            runner.Override = c => IsRunningCheck(c) ? FakeCommandRunner.Reply(c,
                "/bin/sh: syntax error: unexpected word (expecting \"do\")", remoteExit: 2) : null;
            await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "expecting");
            Check.NoRemoval(runner); AssertNoSuccess(runner, f);
        });
        Test("process probe command fits legacy ADB command budget", f => {
            Check.True(System.Text.Encoding.UTF8.GetByteCount(RunningProcessProbe.Command) + 160 < 4096, "Probe too large for legacy adbd");
            foreach (string forbidden in new[] { "kill ", "rm ", "chmod ", "mount ", " >", "-links", "ps -" })
                Check.True(!RunningProcessProbe.Command.Contains(forbidden), "Probe must be read-only and BusyBox compatible: " + forbidden);
        });
        foreach (bool worker in new[] { false, true })
            Test("process probe accepts one launcher/root with worker=" + worker, f => {
                var r = new FakeCommandRunner(f);
                var observation = RunningProcessProbe.Analyze(r.RunningSnapshot(worker: worker), App(f), Launcher(f));
                Check.True(observation.Valid, observation.Reason);
            });
        Test("process probe accepts nested same-image descendants", f => {
            var r = new FakeCommandRunner(f);
            string snapshot = r.RunningSnapshot(worker: true).Replace("C1RUN_END", "C1RUN_PROCESS\t102\t101\t130\t" + App(f) + "\t" + image + "\nC1RUN_END");
            Check.True(RunningProcessProbe.Analyze(snapshot, App(f), Launcher(f)).Valid, "Nested terminal descendants rejected");
        });
        var defects = new Dictionary<string, Func<string, string>> {
            ["missing end marker"] = s => s.Replace("C1RUN_END", ""),
            ["extra trailer"] = s => s + "\nextra",
            ["invalid current path"] = s => s.Replace("C1RUN_CURRENT\t" + image, "C1RUN_CURRENT\t/tmp/C1ancher"),
            ["deleted app"] = s => s.Replace("\t" + image + "\n", "\t" + image + " (deleted)\n"),
            ["duplicate PID"] = s => s.Replace("C1RUN_END", s.Split('\n')[1] + "\nC1RUN_END"),
            ["PID zero"] = s => s.Replace("\t100\t90\t", "\t0\t90\t"),
            ["noncanonical PID"] = s => s.Replace("\t100\t90\t", "\t0100\t90\t"),
            ["missing start time"] = s => s.Replace("\t110\t", "\t0\t"),
            ["invalid hash"] = s => s.Replace("\t" + image + "\n", "x\t" + image + "\n"),
            ["missing parent launcher"] = s => string.Join("\n", s.Split('\n').Where(row => !row.StartsWith("C1RUN_PROCESS\t90\t"))),
            ["different parent"] = s => s.Replace("\t100\t90\t", "\t100\t81\t"),
            ["self-parent"] = s => s.Replace("\t100\t90\t", "\t100\t100\t"),
            ["cycle"] = s => s.Replace("\t100\t90\t", "\t100\t101\t"),
            ["detached worker"] = s => s.Replace("\t101\t100\t", "\t101\t81\t"),
            ["second main directly under launcher"] = s => s.Replace("\t101\t100\t", "\t101\t90\t"),
            ["worker older than parent"] = s => s.Replace("\t120\t", "\t109\t"),
            ["app older than launcher"] = s => s.Replace("\t110\t", "\t99\t"),
            ["launcher parent is its app"] = s => s.Replace("\t90\t80\t", "\t90\t100\t"),
            ["launcher self-parent"] = s => s.Replace("\t90\t80\t", "\t90\t90\t"),
            ["scanner reports hash failure"] = s => s.Replace("C1RUN_END", "C1RUN_ERROR\t101\thash\nC1RUN_END"),
            ["scanner reports PID change"] = s => s.Replace("C1RUN_END", "C1RUN_ERROR\t101\tchanged\nC1RUN_END"),
            ["old release worker"] = s => s.Replace("\t120\t", "\t120\t").Replace(s.Split('\n')[3], s.Split('\n')[3].Replace("7-1.2.3-test", "6-old")),
            ["two launchers"] = s => s.Replace("C1RUN_END", s.Split('\n')[1].Replace("\t90\t80\t", "\t91\t80\t") + "\nC1RUN_END")
        };
        foreach (var (name, mutate) in defects)
            Test("process probe rejects " + name, f => {
                string snapshot = mutate(new FakeCommandRunner(f).RunningSnapshot(worker: true));
                var result = RunningProcessProbe.Analyze(snapshot, App(f), Launcher(f));
                Check.True(!result.Valid, "Unsafe snapshot accepted: " + name);
            });
        Test("process probe rejects wrong launcher hash even with correct app", f => {
            var r = new FakeCommandRunner(f);
            Check.True(!RunningProcessProbe.Analyze(r.RunningSnapshot(), App(f), new string('0', 64)).Valid, "Wrong launcher accepted");
        });
        foreach (bool reboot in new[] { false, true })
            TestAsync("full simulated install accepts legitimate terminal fork through all gates: reboot=" + reboot, async f => {
                var runner = new FakeCommandRunner(f);
                runner.Override = c => IsRunningCheck(c) ? FakeCommandRunner.Reply(c, runner.RunningSnapshot(worker: true)) : null;
                await runner.Engine.InstallAsync(FakeCommandRunner.Serial, reboot);
                Check.Equal(reboot ? 6 : 4, runner.Calls.Count(IsRunningCheck));
                Check.True(File.Exists(Path.Combine(f.Evidence, "result.txt")), "Healthy fork never completed");
            });
        TestAsync("process probe waits for two observations of same main identity", async f => {
            var runner = new FakeCommandRunner(f); int reads = 0;
            runner.Override = c => {
                if (!IsRunningCheck(c)) return null;
                reads++;
                return FakeCommandRunner.Reply(c, runner.RunningSnapshot().Replace("\t110\t", reads == 1 ? "\t111\t" : "\t112\t"));
            };
            await runner.Engine.InstallAsync(FakeCommandRunner.Serial, false);
            Check.Equal(5, reads);
        });
        TestAsync("process probe rejects endlessly changing main and preserves diagnostics", async f => {
            var runner = new FakeCommandRunner(f); int reads = 0;
            runner.Override = c => IsRunningCheck(c) ? FakeCommandRunner.Reply(c, runner.RunningSnapshot().Replace("\t110\t", "\t" + (110 + ++reads) + "\t")) : null;
            await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "before-removal");
            Check.NoRemoval(runner); AssertNoSuccess(runner, f);
            string diagnostic = File.ReadAllText(Path.Combine(f.Evidence, "running-process-before-removal.txt"));
            Check.True(diagnostic.Contains("C1RUN_PROCESS") && diagnostic.Contains("主页预期 SHA256="), "Missing diagnostic identities");
        });
    }
}
