using System.Text.RegularExpressions;
using C1SlimInstaller;

namespace InstallerOfflineTests;

internal sealed record Call(string File, string[] Args, TimeSpan Timeout)
{
    public string Operation => Args.Length > 2 && Args[0] == "-s" ? Args[2] : Args[0];
    public string Shell => Operation == "shell" ? FakeCommandRunner.Unwrap(Args[3]) : "";
}

// Strict in-memory device model. No Process, CommandRunner, socket, HTTP or real ADB.
// An unknown command is a test bug, never a silently successful fake operation.
internal sealed class FakeCommandRunner(Fixture fixture) : ICommandRunner
{
    public const string Serial = "OFFLINE-SELECTED-DEVICE";
    public List<Call> Calls { get; } = [];
    public List<string> Logs { get; } = [];
    public Func<Call, CommandResult?>? Override { get; set; }
    public bool InitiallyRoot { get; set; } = true;
    public bool RootRequested { get; private set; }
    public bool DeveloperExists { get; set; }
    public bool Factory { get; set; }
    public bool CoreRunning { get; set; } = true;
    public bool StartProducesRunningCore { get; set; } = true;
    public bool EnrollmentVerified { get; private set; }
    public bool CoreStartRequested { get; private set; }
    public bool RebootRequested { get; private set; }
    public bool SuspendEnabled { get; set; }
    public bool DisableSuspendOnReboot { get; set; }
    public string BeforeBootId { get; set; } = "11111111-2222-4333-8444-555555555555";
    public string AfterBootId { get; set; } = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    public OfflineClock Clock { get; } = new();
    public bool EnrollmentPrepared { get; private set; }
    public int MetadataMutations { get; private set; }
    public int? MetadataFailureIndex { get; set; }
    public Action<Dictionary<string, MetadataNode>>? BeforeMetadata { get; set; }
    public Action<string, Dictionary<string, MetadataNode>>? BeforeMetadataStep { get; set; }
    public List<string> MetadataSteps { get; } = [];
    public HashSet<string> EnrollmentRehashed { get; } = new(StringComparer.Ordinal);
    public Dictionary<string, MetadataNode> Metadata { get; } = new(StringComparer.Ordinal);
    public sealed record MetadataNode(string Kind, int Links, int Mode, int Uid = 0, int Gid = 0);
    // Independent allowlist: a broadened or weakened production command must fail this model.
    public static readonly string[] EnrollmentDirectories = ["enrollment", "enrollment/release", "enrollment/release/artifacts"];
    public static readonly string[] EnrollmentFiles = [
        "enrollment/bootstrap.v1", "enrollment/bootstrap.v1.sig", "enrollment/core.ed25519.pem", "enrollment/core.ed25519.pub",
        "enrollment/app-daemon-bootstrap.sh", "enrollment/device-core-enroll.sh", "enrollment/enroll.sh",
        "enrollment/release/manifest.v1", "enrollment/release/manifest.v1.sig", "enrollment/release/artifacts/C1ancher",
        "enrollment/release/artifacts/c1pkg", "enrollment/release/artifacts/C1ancher-launcher", "enrollment/release/artifacts/c1updater"
    ];
    public string DevicesOutput { get; set; } = "List of devices attached\nOFFLINE-SELECTED-DEVICE\tdevice\nOTHER-DEVICE\tdevice\n";
    readonly Dictionary<string, string> remoteHashes = new(StringComparer.Ordinal);
    public InstallerEngine Engine => new(fixture.Payload, new AdbClient(fixture.At("tools/adb.exe"), this), Logs.Add, fixture.Evidence) { Clock = Clock };
    public string RunningSnapshot(bool worker = false, bool duplicate = false, bool wrongImage = false)
    {
        const string path = "/usr/data/c1/core/releases/7-1.2.3-test/artifacts/C1ancher";
        string app = wrongImage ? new string('0', 64) : Payload.Hash(fixture.At("enrollment/release/artifacts/C1ancher"));
        string launcher = Payload.Hash(fixture.At("enrollment/release/artifacts/C1ancher-launcher"));
        string output = "C1RUN_CURRENT\t" + path + "\nC1RUN_PROCESS\t90\t80\t100\t" + launcher + "\t" + path + "-launcher\n";
        if (CoreRunning) output += "C1RUN_PROCESS\t100\t90\t110\t" + app + "\t" + path + "\n";
        if (worker) output += "C1RUN_PROCESS\t101\t100\t120\t" + app + "\t" + path + "\n";
        if (duplicate) output += "C1RUN_PROCESS\t102\t90\t120\t" + app + "\t" + path + "\n";
        return output + "C1RUN_END";
    }
    public Task<CommandResult> RunAsync(string file, IEnumerable<string> args, TimeSpan timeout, CancellationToken cancellation = default)
    {
        var call = new Call(file, args.ToArray(), timeout); Calls.Add(call);
        Check.Equal(fixture.At("tools/adb.exe"), file, "Only fixture executable path may be passed to fake");
        Check.True(timeout > TimeSpan.Zero, "Timeout must be bounded");
        cancellation.ThrowIfCancellationRequested();
        // Fault injection must not hide an invalid native argument vector or wrong device.
        if (call.Operation == "devices") Check.True(call.Args.SequenceEqual(new[] { "devices" }), "Unexpected devices arguments");
        else
        {
            Check.True(call.Args.Length >= 3 && call.Args[0] == "-s", "Every device command must specify -s");
            Check.Equal(Serial, call.Args[1], "A command targeted a different device");
            int count = call.Operation switch { "root" or "reboot" => 3, "shell" => 4, "push" or "pull" => 5,
                _ => throw new InvalidOperationException("Unexpected fake operation: " + string.Join(" ", call.Args)) };
            Check.Equal(count, call.Args.Length, "Unexpected native arguments for " + call.Operation);
            if (call.Operation == "shell") _ = call.Shell;
        }
        CommandResult? overridden = Override?.Invoke(call);
        if (overridden != null) return Task.FromResult(overridden);
        if (call.Operation == "devices") return Task.FromResult(new CommandResult(0, DevicesOutput));
        switch (call.Operation)
        {
            case "reboot":
                Check.Equal(TimeSpan.FromSeconds(30), timeout, "Reboot must have bounded native timeout");
                RebootRequested = true;
                if (DisableSuspendOnReboot) SuspendEnabled = false;
                return Task.FromResult(new CommandResult(0, ""));
            case "root": RootRequested = true; return Task.FromResult(new CommandResult(0, "restarting adbd as root\n"));
            case "push":
                Check.True(call.Args.Length == 5, "Unexpected push arguments");
                Check.True(Path.GetFullPath(call.Args[3]).StartsWith(fixture.Payload.Root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase), "Push source escaped fixture");
                remoteHashes[call.Args[4]] = Payload.Hash(call.Args[3]);
                var staged = Regex.Match(call.Args[4], @"\A(/storage/c1-installer-[a-f0-9]{32})/(enrollment/.+)\z");
                if (staged.Success)
                {
                    string root = staged.Groups[1].Value;
                    foreach (string directory in EnrollmentDirectories)
                        Metadata.TryAdd(root + "/" + directory, new MetadataNode("directory", 2, 777));
                    Metadata[call.Args[4]] = new MetadataNode("regular", 1, 666);
                }
                return Task.FromResult(new CommandResult(0, "pushed"));
            case "pull": throw new InvalidOperationException("Default installer must not pull factory backups");
            case "shell":
                if (call.Shell.StartsWith(": c1-enrollment-metadata-v1", StringComparison.Ordinal))
                    return Task.FromResult(Reply(call, "", PrepareEnrollment(call.Shell) ? 0 : 1));
                string output = ModelShell(call.Shell);
                int exit = call.Shell.Contains("/device-setup.sh' verify-suspend ", StringComparison.Ordinal) && !SuspendEnabled ? 1 : 0;
                return Task.FromResult(Reply(call, output, exit));
            default: throw new InvalidOperationException("Unexpected fake operation: " + string.Join(" ", call.Args));
        }
    }
    public static string Unwrap(string wrapped)
    {
        const string suffix = " ); rc=$?; printf ";
        int end = wrapped.IndexOf(suffix, StringComparison.Ordinal);
        Check.True(wrapped.StartsWith("( ") && end > 2, "Unexpected shell wrapper");
        return wrapped[2..end];
    }
    public static CommandResult Reply(Call call, string output = "", int remoteExit = 0, int hostExit = 0)
    {
        var marker = Regex.Match(call.Args[3], "__C1_SETUP_EXIT_[0-9a-f]{32}=");
        Check.True(marker.Success, "Missing shell exit marker");
        return new CommandResult(hostExit, output + "\n" + marker.Value + remoteExit + "\n");
    }
    bool PrepareEnrollment(string command)
    {
        var match = Regex.Match(command, @"c1_root='(/storage/c1-installer-[a-f0-9]{32})'");
        Check.True(match.Success, "Metadata command lacks a generated staging root");
        string root = match.Groups[1].Value;
        string Q(string relative) => "'" + root + "/" + relative + "'";
        var expected = new List<string> { ": c1-enrollment-metadata-v1" };
        foreach (string directory in EnrollmentDirectories)
        { expected.Add("test ! -L " + Q(directory)); expected.Add("test -d " + Q(directory)); }
        foreach (string file in EnrollmentFiles)
        {
            expected.Add("test ! -L " + Q(file)); expected.Add("test -f " + Q(file));
            expected.Add("c1_meta=$(stat -c '%h' " + Q(file) + ")");
            expected.Add("test \"$c1_meta\" = '1'");
        }
        foreach (var (relative, mode) in EnrollmentDirectories.Select(p => (p, 700)).Concat(EnrollmentFiles.Select(p => (p, 600))))
        { expected.Add("chown 0:0 " + Q(relative)); expected.Add("chmod " + mode + " " + Q(relative)); }
        foreach (var (relative, mode) in EnrollmentDirectories.Select(p => (p, 700)).Concat(EnrollmentFiles.Select(p => (p, 600))))
        {
            expected.Add("c1_meta=$(stat -c '%a:%u:%g' " + Q(relative) + ")");
            expected.Add("test \"$c1_meta\" = '" + mode + ":0:0'");
        }
        string compact = ": c1-enrollment-metadata-v1 && c1_root='" + root
            + "' && c1_dirs='" + string.Join(" ", EnrollmentDirectories)
            + "' && c1_files='" + string.Join(" ", EnrollmentFiles) + "'"
            + " && for c1_p in $c1_dirs; do test ! -L \"$c1_root/$c1_p\" && test -d \"$c1_root/$c1_p\" || exit 1; done"
            + " && for c1_p in $c1_files; do test ! -L \"$c1_root/$c1_p\" && test -f \"$c1_root/$c1_p\" && c1_meta=$(stat -c '%h' \"$c1_root/$c1_p\") && test \"$c1_meta\" = '1' || exit 1; done"
            + " && for c1_p in $c1_dirs; do chown 0:0 \"$c1_root/$c1_p\" && chmod 700 \"$c1_root/$c1_p\" || exit 1; done"
            + " && for c1_p in $c1_files; do chown 0:0 \"$c1_root/$c1_p\" && chmod 600 \"$c1_root/$c1_p\" || exit 1; done"
            + " && for c1_p in $c1_dirs; do c1_meta=$(stat -c '%a:%u:%g' \"$c1_root/$c1_p\") && test \"$c1_meta\" = '700:0:0' || exit 1; done"
            + " && for c1_p in $c1_files; do c1_meta=$(stat -c '%a:%u:%g' \"$c1_root/$c1_p\") && test \"$c1_meta\" = '600:0:0' || exit 1; done";
        Check.Equal(compact, command, "Metadata allowlist, prechecks, mutation ordering or stat assertions changed");
        Check.True(System.Text.Encoding.UTF8.GetByteCount(Calls[^1].Args[3]) < 4096, "Wrapped metadata shell request exceeds legacy ADB budget");
        // Expand the independently checked fixed loops for per-operation fault injection.
        string[] steps = expected.ToArray();
        int stagedCount = fixture.Payload.Files.Count(p => p.StartsWith("enrollment/") || p.StartsWith("profile/") || p.StartsWith("accessories/") || p == "device-setup.sh" || p.StartsWith("usb/"));
        Check.Equal(stagedCount, Calls.Count(c => c.Operation == "push"), "Metadata preparation must follow every staging upload");
        Check.Equal(8, Calls.Count(c => c.Shell.StartsWith("sh -n ")), "Metadata preparation must follow eight syntax checks");
        Check.True(!EnrollmentPrepared, "Repeated enrollment metadata preparation");
        foreach (var (path, node) in Metadata)
            Check.Equal(node.Kind == "directory" ? 777 : 666, node.Mode, "Staging must model permissive uploaded metadata: " + path);
        BeforeMetadata?.Invoke(Metadata);
        string statValue = "";
        foreach (string step in steps)
        {
            int index = MetadataSteps.Count; MetadataSteps.Add(step);
            if (MetadataFailureIndex == index) return false;
            BeforeMetadataStep?.Invoke(step, Metadata);
            if (step.StartsWith(": ")) continue;
            if (step.StartsWith("test \"$c1_meta\" = "))
            { if (step != "test \"$c1_meta\" = '" + statValue + "'") return false; continue; }
            var pathMatch = Regex.Match(step, "'(/storage/c1-installer-[a-f0-9]{32}/[^']+)'", RegexOptions.CultureInvariant);
            Check.True(pathMatch.Success, "Metadata operation escaped generated staging");
            string path = pathMatch.Groups[1].Value;
            if (!Metadata.TryGetValue(path, out var node)) return false;
            if (step.StartsWith("test ! -L ")) { if (node.Kind == "symlink") return false; }
            else if (step.StartsWith("test -d ")) { if (node.Kind != "directory") return false; }
            else if (step.StartsWith("test -f ")) { if (node.Kind != "regular") return false; }
            else if (step.Contains("stat -c '%h'")) statValue = node.Links.ToString();
            else if (step.StartsWith("chown 0:0 ")) { Metadata[path] = node with { Uid = 0, Gid = 0 }; MetadataMutations++; }
            else if (step.StartsWith("chmod ")) { Metadata[path] = node with { Mode = int.Parse(step.Split(' ')[1]) }; MetadataMutations++; }
            else if (step.Contains("stat -c '%a:%u:%g'")) statValue = node.Mode + ":" + node.Uid + ":" + node.Gid;
            else throw new InvalidOperationException("Unmodelled metadata operation: " + step);
        }
        EnrollmentPrepared = true;
        return true;
    }
    void RequireEnrollmentReady()
    {
        Check.True(EnrollmentPrepared, "Configuration/enrollment executed with directory 777 / file 666 metadata");
        Check.Equal(13, EnrollmentRehashed.Count, "Configuration/enrollment preceded post-metadata hash verification");
    }
    string ModelShell(string command)
    {
        if (command == "id -u") return InitiallyRoot || RootRequested ? "0" : "2000";
        if (command == "cat /sys/kernel/config/usb_gadget/demo/strings/0x409/product; uname -m") return "mp-d261\nmips\n";
        if (command == "cat /proc/mounts") return "/dev/root / squashfs ro,relatime 0 0\n";
        if (command == "sha256sum /etc/init.d/S90usb") return Payload.OriginalUsbHash + "  /etc/init.d/S90usb";
        if (command == "sha256sum /etc/app_daemon") return Payload.OriginalDaemonHash + "  /etc/app_daemon";
        if (command == "sha256sum /usr/data/c1/core/current/manifest.v1") return Payload.Hash(fixture.At("enrollment/release/manifest.v1")) + "  manifest.v1";
        if (command == "sha256sum /etc/c1updater/core.ed25519.pub") return fixture.Payload.CoreKeyHash + "  core.ed25519.pub";
        if (command == "if [ -e /usr/data/c1/update/enrolled.v1 ]; then echo enrolled; else echo factory; fi") return Factory ? "factory" : "enrolled";
        if (command == "if [ -f /storage/c1/update/enrollment/apply.log ]; then tail -c 16384 /storage/c1/update/enrollment/apply.log; fi") return "OFFLINE simulated diagnostic log";
        if (command.StartsWith("if [ -e /storage/c1/update/enrollment/apply.pid ]")) return "complete";
        if (command == "cat /proc/sys/kernel/random/boot_id") return RebootRequested ? AfterBootId : BeforeBootId;
        if (command == RunningProcessProbe.Command) return RunningSnapshot();
        // No historical enrollment staging exists in this model.
        if (Regex.IsMatch(command, @"\Ash '/storage/c1-installer-[a-f0-9]{32}/enrollment/device-core-enroll\.sh' verify\z"))
        {
            RequireEnrollmentReady();
            string path = command[4..^8];
            Check.Equal(Payload.Hash(fixture.At("enrollment/device-core-enroll.sh")), remoteHashes[path], "Verification must use this attempt's authenticated upload");
            Check.Equal(TimeSpan.FromSeconds(90), Calls[^1].Timeout);
            EnrollmentVerified = true; return "";
        }
        if (command.StartsWith("sha256sum '"))
        {
            string path = command[11..^1];
            if (remoteHashes.TryGetValue(path, out string? hash))
            {
                if (EnrollmentPrepared && Metadata.TryGetValue(path, out var node) && node.Kind == "regular")
                    EnrollmentRehashed.Add(path);
                return hash + "  " + path;
            }
            if (DeveloperExists && path.StartsWith("/storage/mtp/C1DeveloperTools/") && !path.Contains("/.incoming-"))
                return Payload.Hash(fixture.At("developer/" + path[(path.LastIndexOf('/') + 1)..])) + "  " + path;
            string? profile = path switch {
                "/usr/data/c1/pkg/repository.ed25519.pub" => "profile/repository.ed25519.pub",
                "/usr/data/c1/pkg/repository.url" => "profile/repository.url",
                "/usr/data/c1/update/repository.url" => "profile/core-repository.url", _ => null };
            if (profile != null) return Payload.Hash(fixture.At(profile)) + "  " + path;
            throw new InvalidOperationException("Hash requested for unmodelled file: " + path);
        }
        if (command.StartsWith("sync && test ! -e '/storage/mtp/C1DeveloperTools/"))
        {
            var move = Regex.Match(command, " && mv '([^']+)' '([^']+)'$");
            Check.True(move.Success, "Unexpected developer directory promotion");
            string from = move.Groups[1].Value, to = move.Groups[2].Value;
            var moved = remoteHashes.Keys.Where(p => p.StartsWith(from + "/", StringComparison.Ordinal)).ToArray();
            Check.True(moved.Length > 0, "Promoted an empty developer directory");
            foreach (string path in moved) { remoteHashes[to + path[from.Length..]] = remoteHashes[path]; remoteHashes.Remove(path); }
            return "";
        }
        if (command.StartsWith("test ! -L '/storage/mtp/C1DeveloperTools/") && command.Contains("then echo existing;")) return DeveloperExists ? "existing" : "new";
        var helper = Regex.Match(command, "^sh '/storage/c1-installer-[0-9a-f]{32}/device-setup\\.sh' ([a-z-]+)(?: |$)");
        if (helper.Success)
        {
            RequireEnrollmentReady();
            string action = helper.Groups[1].Value;
            Check.True(new[] { "preflight", "prepare", "accessories", "start-core", "enable-suspend", "verify-suspend", "remove-factory", "verify" }.Contains(action), "Unknown helper action");
            string root = Regex.Match(command, @"/storage/c1-installer-[a-f0-9]{32}").Value;
            string extra = action == "accessories" ? " '" + root + "/accessories'" :
                action is "enable-suspend" or "verify-suspend" ? " " + Payload.Hash(fixture.At("enrollment/release/manifest.v1"))
                    + " " + Payload.Hash(fixture.At("enrollment/release/artifacts/c1pkg")) : "";
            Check.Equal("sh '" + root + "/device-setup.sh' " + action + extra, command, "Unexpected helper arguments");
            if (action == "start-core")
            {
                Check.True(EnrollmentVerified, "Core start preceded authenticated enrollment verification");
                Check.Equal(TimeSpan.FromSeconds(90), Calls[^1].Timeout);
                CoreStartRequested = true;
                if (StartProducesRunningCore) CoreRunning = true;
            }
            if (action is "enable-suspend" or "verify-suspend")
            {
                Check.True(EnrollmentVerified && CoreStartRequested && CoreRunning, "Suspend action preceded verified running core");
                Check.True(Calls.Count(c => c.Shell == RunningProcessProbe.Command) >= 2, "Suspend action preceded stable running-image checks");
                if (action == "enable-suspend") SuspendEnabled = true;
                if (!SuspendEnabled) return "C1SETUP_ERROR automatic-suspend-not-enabled";
            }
            return "C1SETUP_OK " + action;
        }
        var usb = Regex.Match(command, @"\Ash '(/storage/c1-installer-[a-f0-9]{32})/usb/device-open-adb\.sh' (install|verify) ");
        if (usb.Success)
        {
            RequireEnrollmentReady();
            Check.Equal("sh '" + usb.Groups[1].Value + "/usb/device-open-adb.sh' " + usb.Groups[2].Value + " " + Payload.OriginalUsbHash + " " + Payload.Hash(fixture.At("usb/S90usb.open")), command, "Unexpected USB script arguments");
            return "";
        }
        if (command.StartsWith("sh '/storage/c1-installer-") && command.Contains("/profile/device-repository-config.sh' "))
        { RequireEnrollmentReady(); return ""; }
        if (command.StartsWith("chmod 700 '/storage/c1-installer-") && command.Contains("/device-core-enroll.sh' install "))
        {
            RequireEnrollmentReady();
            var root = Regex.Match(command, @"/storage/c1-installer-[a-f0-9]{32}").Value;
            string bundle = "'" + root + "/enrollment/";
            Check.Equal("chmod 700 " + bundle + "device-core-enroll.sh' " + bundle + "enroll.sh' && " + bundle + "device-core-enroll.sh' install '" + root + "/enrollment'", command,
                "Core script chmod must stop execution on failure");
            return "";
        }
        if (command.StartsWith("test ! -L /storage && mkdir -m 700 '/storage/c1-installer-") ||
            command.StartsWith("mkdir -p '/storage/c1-installer-") ||
            command.StartsWith("sh -n '/storage/c1-installer-") ||
            command.StartsWith("rm -rf '/storage/c1-installer-") ||
            command.StartsWith("mkdir '/storage/mtp/C1DeveloperTools/.incoming-") ||
            command.StartsWith("mkdir -p '/storage/mtp/C1DeveloperTools/.incoming-") ||
            command.StartsWith("sync && test ! -e '/storage/mtp/C1DeveloperTools/") ||
            (command.StartsWith("test ! -L '/storage") && (command.Contains(" && test -f ") || command.Contains(" && { test ! -e ")))) return "";
        throw new InvalidOperationException("Unexpected fake shell command: " + command);
    }
}

// Task.Delay's timer fires synchronously and advances only virtual time. No sleeping/device I/O.
internal sealed class OfflineClock : TimeProvider
{
    DateTimeOffset now = new(2026, 1, 1, 0, 0, 0, TimeSpan.Zero);
    public List<TimeSpan> Delays { get; } = [];
    public override DateTimeOffset GetUtcNow() => now;
    public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period)
    {
        Check.True(dueTime > TimeSpan.Zero && dueTime <= TimeSpan.FromSeconds(3), "Unexpected polling delay");
        Check.Equal(Timeout.InfiniteTimeSpan, period, "Only single-shot polling is modelled");
        Delays.Add(dueTime); now += dueTime;
        callback(state);
        return new CompletedTimer();
    }
    sealed class CompletedTimer : ITimer
    {
        public bool Change(TimeSpan dueTime, TimeSpan period) => throw new InvalidOperationException("Unexpected timer reuse");
        public void Dispose() { }
        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }
}

internal static class Check
{
    public static void True(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
    public static void Equal<T>(T expected, T actual, string message = "Values differ") => True(EqualityComparer<T>.Default.Equals(expected, actual), $"{message}: expected {expected}, actual {actual}");
    public static void Bytes(byte[] expected, byte[] actual) => True(expected.SequenceEqual(actual), "Byte sequences differ");
    public static void Throws<T>(Action action, string? message = null) where T : Exception
    {
        try { action(); } catch (T e) { if (message != null) True(e.Message.Contains(message, StringComparison.Ordinal), "Wrong rejection: " + e.Message); return; }
        throw new InvalidOperationException("Expected " + typeof(T).Name);
    }
    public static async Task ThrowsAsync<T>(Func<Task> action, string? message = null) where T : Exception
    {
        try { await action(); } catch (T e) { if (message != null) True(e.Message.Contains(message, StringComparison.Ordinal), "Wrong rejection: " + e.Message); return; }
        throw new InvalidOperationException("Expected " + typeof(T).Name);
    }
    public static void NoRemoval(FakeCommandRunner runner)
    {
        True(!runner.Calls.Any(c => c.Shell.Contains("remove-factory", StringComparison.Ordinal)), "Failure entered remove-factory");
        True(!runner.Calls.Any(c => c.Operation == "reboot"), "Failure rebooted the device");
    }
}
