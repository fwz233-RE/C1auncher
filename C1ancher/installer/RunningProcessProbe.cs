using System.Text.RegularExpressions;

namespace C1SlimInstaller;

// No writes or process control: all identity evidence comes from live /proc exe inodes.
internal static class RunningProcessProbe
{
    // C# raw strings preserve source checkout line endings. Device /bin/sh requires
    // LF: a CR after `do` makes BusyBox reject this read-only probe before it runs.
    internal static readonly string Command = CommandSource.ReplaceLineEndings("\n");
    internal const string CommandSource = """
        current=$(readlink -f /usr/data/c1/core/current/artifacts/C1ancher) || exit 1
        printf 'C1RUN_CURRENT\t%s\n' "$current"
        identity() {
            info=$(cat "$1/stat" 2>/dev/null) || return 1
            fields=${info##*) }
            [ "$fields" != "$info" ] || return 1
            set -- $fields
            [ "$#" -ge 20 ] || return 1
            parent=$2
            shift 19
            printf '%s %s' "$parent" "$1"
        }
        for p in /proc/[0-9]*/exe; do
            target=$(readlink "$p" 2>/dev/null) || continue
            case "$target" in
                */artifacts/C1ancher|*/artifacts/C1ancher-launcher|*/artifacts/C1ancher\ \(deleted\)|*/artifacts/C1ancher-launcher\ \(deleted\)) ;;
                *) continue ;;
            esac
            dir=${p%/exe}; pid=${dir##*/}
            before=$(identity "$dir") || { printf 'C1RUN_ERROR\t%s\tidentity-before\n' "$pid"; continue; }
            sum=$(sha256sum "$p" 2>/dev/null) || { printf 'C1RUN_ERROR\t%s\thash\n' "$pid"; continue; }
            after=$(identity "$dir") || { printf 'C1RUN_ERROR\t%s\tidentity-after\n' "$pid"; continue; }
            again=$(readlink "$p" 2>/dev/null) || { printf 'C1RUN_ERROR\t%s\texe-after\n' "$pid"; continue; }
            [ "$before" = "$after" ] && [ "$target" = "$again" ] || { printf 'C1RUN_ERROR\t%s\tchanged\n' "$pid"; continue; }
            set -- $before
            printf 'C1RUN_PROCESS\t%s\t%s\t%s\t%s\t%s\n' "$pid" "$1" "$2" "${sum%% *}" "$target"
        done
        again=$(readlink -f /usr/data/c1/core/current/artifacts/C1ancher) || exit 1
        [ "$current" = "$again" ] || printf 'C1RUN_ERROR\t0\tcurrent-changed\n'
        printf 'C1RUN_END\n'
        """;

    internal sealed record ProcessIdentity(int Pid, int Parent, ulong Started, string Hash, string Path);
    internal sealed record Observation(bool Valid, string Reason, string Identity = "");
    static readonly Regex Digest = new(@"\A[0-9a-f]{64}\z");
    static readonly Regex Image = new(@"\A/usr/data/c1/core/releases/[A-Za-z0-9._-]+/artifacts/C1ancher\z");

    internal static Observation Analyze(string output, string appHash, string launcherHash)
    {
        var rows = output.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        if (rows.Length < 2 || rows[0].Split('\t') is not ["C1RUN_CURRENT", var current]
            || !Image.IsMatch(current) || rows[^1] != "C1RUN_END")
            return new(false, "运行快照不完整或当前核心路径无效");
        var nodes = new Dictionary<int, ProcessIdentity>();
        foreach (string row in rows[1..^1])
        {
            if (row.StartsWith("C1RUN_ERROR\t", StringComparison.Ordinal)) return new(false, "进程扫描期间退出、变化或无法读取：" + row);
            var fields = row.Split('\t');
            if (fields is not ["C1RUN_PROCESS", _, _, _, _, _]
                || !int.TryParse(fields[1], out int pid) || pid <= 0 || fields[1] != pid.ToString()
                || !int.TryParse(fields[2], out int parent) || parent <= 0 || fields[2] != parent.ToString()
                || !ulong.TryParse(fields[3], out ulong started) || started == 0 || fields[3] != started.ToString()
                || !Digest.IsMatch(fields[4]) || !nodes.TryAdd(pid, new(pid, parent, started, fields[4], fields[5])))
                return new(false, "运行快照字段无效或 PID 重复");
        }
        var apps = nodes.Values.Where(p => p.Path == current).ToArray();
        var launchers = nodes.Values.Where(p => p.Path == current + "-launcher").ToArray();
        // Reject other releases, deleted images and any extra launcher. Never filter bad images out to obtain one match.
        if (nodes.Values.Any(p => p.Path != current && p.Path != current + "-launcher"))
            return new(false, "检测到其他核心版本或已删除的运行镜像");
        if (apps.Length == 0) return new(false, "未检测到当前核心的主页进程");
        if (launchers.Length != 1) return new(false, "当前核心启动器数量不是 1：" + launchers.Length);
        if (apps.Any(p => p.Hash != appHash) || launchers[0].Hash != launcherHash)
            return new(false, "主页或启动器的实际运行镜像摘要与安装包不一致");
        var appIds = apps.Select(p => p.Pid).ToHashSet();
        var roots = apps.Where(p => !appIds.Contains(p.Parent)).ToArray();
        if (roots.Length != 1 || roots[0].Parent != launchers[0].Pid)
            return new(false, "未检测到由已核验启动器直接启动的唯一主页主进程；独立主页数量：" + roots.Length);
        var root = roots[0];
        foreach (var app in apps)
        {
            var seen = new HashSet<int>();
            var item = app;
            while (item.Pid != root.Pid)
            {
                if (!seen.Add(item.Pid) || !appIds.Contains(item.Parent) || !nodes.TryGetValue(item.Parent, out var ancestor)
                    || ancestor.Started > item.Started)
                    return new(false, "同镜像进程不是唯一主页的有效派生进程");
                item = ancestor;
            }
        }
        if (launchers[0].Started > root.Started || appIds.Contains(launchers[0].Parent) || launchers[0].Parent == launchers[0].Pid)
            return new(false, "启动器与主页的进程关系无效");
        return new(true, $"唯一主页 PID={root.Pid}，启动器 PID={launchers[0].Pid}，已核验同镜像派生进程 {apps.Length - 1} 个",
            $"{current}:{launchers[0].Pid}:{launchers[0].Started}:{root.Pid}:{root.Started}");
    }
}
