using C1SlimInstaller;

namespace InstallerOfflineTests;

internal static partial class Program
{
    static void RegisterSuspendAcceptance()
    {
        foreach (bool factory in new[] { false, true })
        foreach (bool enabled in new[] { false, true })
        foreach (bool reboot in new[] { false, true })
            TestAsync($"suspend success gate: factory={factory}, enabled={enabled}, reboot={reboot}", async f => {
                var runner = new FakeCommandRunner(f) { Factory = factory, SuspendEnabled = enabled };
                await runner.Engine.InstallAsync(FakeCommandRunner.Serial, reboot);
                int enable = runner.Calls.FindIndex(c => IsHelper(c, "enable-suspend"));
                int removal = runner.Calls.FindIndex(c => IsHelper(c, "remove-factory"));
                Check.True(enable > 1 && IsRunningCheck(runner.Calls[enable - 1]) && IsRunningCheck(runner.Calls[enable - 2]), "Enable must follow authenticated stable running-core checks");
                Check.True(IsHelper(runner.Calls[enable + 1], "verify-suspend") && removal > enable + 1, "Immediate verification must precede factory removal");
                Check.Equal(1, runner.Calls.Count(c => IsHelper(c, "enable-suspend")));
                Check.Equal(2, runner.Calls.Count(c => IsHelper(c, "verify-suspend")));
                Check.True(IsHelper(runner.Calls[^2], "verify-suspend"), "Final switch check must immediately precede staging cleanup");
                Check.True(runner.SuspendEnabled, "Success left suspend disabled");
                string result = File.ReadAllText(Path.Combine(f.Evidence, "result.txt"));
                Check.True(result.Contains("\nautomatic_suspend=enabled\nautomatic_suspend_verified=true\n"), "Success omitted verified enabled preference");
                Check.True(runner.Logs.Last().Contains("深度休眠开关已确认开启"), "Success omitted switch acceptance");
            });

        foreach (string stage in new[] { "enable", "immediate check", "final check", "post-reboot check" })
        foreach (string failure in new[] { "remote error", "missing marker", "duplicate marker", "transport error" })
            TestAsync($"suspend failure blocks success: {stage}/{failure}", async f => {
                var runner = new FakeCommandRunner(f);
                bool injected = false;
                runner.Override = c => {
                    bool matches = stage == "enable" ? IsHelper(c, "enable-suspend") :
                        IsHelper(c, "verify-suspend") && runner.Calls.Count(x => IsHelper(x, "verify-suspend")) == (stage == "immediate check" ? 1 : 2);
                    if (!matches) return null;
                    injected = true;
                    if (failure == "transport error") throw new IOException("simulated suspend transport error");
                    string action = stage == "enable" ? "enable-suspend" : "verify-suspend";
                    return failure switch {
                        "remote error" => FakeCommandRunner.Reply(c, "C1SETUP_ERROR suspend-failed", 1),
                        "missing marker" => FakeCommandRunner.Reply(c, "automatic suspend: enabled"),
                        _ => FakeCommandRunner.Reply(c, "C1SETUP_OK " + action + "\nC1SETUP_OK " + action)
                    };
                };
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, stage == "post-reboot check"));
                Check.True(injected, "Suspend fault was not reached");
                AssertNoSuccess(runner, f);
                Check.True(!runner.Calls.Any(c => c.Shell.StartsWith("rm -rf '/storage/c1-installer-")), "Failed suspend gate cleaned recovery staging");
                if (stage is "enable" or "immediate check") Check.NoRemoval(runner);
            });

        TestAsync("claimed enable without changing disabled preference fails independent check", async f => {
            var runner = new FakeCommandRunner(f) { Override = c => IsHelper(c, "enable-suspend") ? FakeCommandRunner.Reply(c, "C1SETUP_OK enable-suspend") : null };
            await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false), "automatic-suspend-not-enabled");
            Check.NoRemoval(runner); AssertNoSuccess(runner, f);
        });
        foreach (bool reboot in new[] { false, true })
            TestAsync("preference disabled later cannot produce success: reboot=" + reboot, async f => {
                var runner = new FakeCommandRunner(f) { DisableSuspendOnReboot = reboot };
                if (!reboot) runner.Override = c => {
                    if (IsHelper(c, "remove-factory")) runner.SuspendEnabled = false;
                    return null;
                };
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, reboot), "automatic-suspend-not-enabled");
                Check.Equal(2, runner.Calls.Count(c => IsHelper(c, "verify-suspend")));
                Check.Equal(1, runner.Calls.Count(c => IsHelper(c, "enable-suspend")), "Final check must not silently repair a lost setting");
                AssertNoSuccess(runner, f);
            });
        foreach (string failure in new[] { "signed verifier", "running image" })
            TestAsync("unverified core never enables suspend: " + failure, async f => {
                var runner = new FakeCommandRunner(f);
                runner.Override = c => (failure == "signed verifier" && c.Shell.EndsWith("/enrollment/device-core-enroll.sh' verify")) ||
                    (failure == "running image" && IsRunningCheck(c)) ? FakeCommandRunner.Reply(c, "simulated core failure", 1) : null;
                await Check.ThrowsAsync<IOException>(() => runner.Engine.InstallAsync(FakeCommandRunner.Serial, false));
                Check.True(!runner.Calls.Any(c => IsHelper(c, "enable-suspend") || IsHelper(c, "verify-suspend")), "Unverified core reached suspend actions");
                Check.NoRemoval(runner); AssertNoSuccess(runner, f);
            });
    }
}
