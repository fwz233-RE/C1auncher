using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using Org.BouncyCastle.Crypto.Parameters;
using Org.BouncyCastle.Crypto.Signers;

namespace C1SlimInstaller;

public sealed class Payload
{
    public string Root { get; }
    public string Version { get; private set; } = "";
    public string Sequence { get; private set; } = "";
    public string CoreKeyHash => Hash(FileAt("enrollment/core.ed25519.pub"));
    public string BootstrapHash => Hash(FileAt("enrollment/app-daemon-bootstrap.sh"));
    public IReadOnlyList<string> Files { get; private set; } = [];
    // Fixed device-side shell inputs. Validation is read-only; signed bytes are never normalized.
    public static IReadOnlyList<string> DeviceShellScripts { get; } = Array.AsReadOnly(new[] {
        "device-setup.sh", "usb/device-open-adb.sh", "usb/S90usb.original", "usb/S90usb.open",
        "profile/device-repository-config.sh", "profile/c1-update-check.sh",
        "enrollment/app-daemon-bootstrap.sh", "enrollment/device-core-enroll.sh", "enrollment/enroll.sh",
        "accessories/neofetch", "accessories/neofetch.upstream"
    });
    public const string DeviceShellConfig = "accessories/c1-config.conf";
    static readonly UTF8Encoding StrictUtf8 = new(false, true);
    public const string OriginalUsbHash = "c2b278b283e9bf851461d9e8f6edfd207cec3b120585f0e091777d163562e965";
    public const string OriginalDaemonHash = "ceb56ddf2ff3c10f7c4c2cd6216b298da1cea799ca7170322f8229d5e9af6ee7";
    public const string OriginalInitHash = "d35cdaa670636511c04d70b5e21b61e1938d2e93b33ae9379a01df1f340cafc2";
    public Payload(string root) => Root = Path.GetFullPath(root);
    public string FileAt(string relative)
    {
        if (!Regex.IsMatch(relative, @"^[A-Za-z0-9_./-]+$") || relative.Split('/').Any(p => p is "" or "." or "..") || Path.IsPathRooted(relative))
            throw new InvalidDataException("安装包路径无效。");
        return Path.Combine(Root, relative.Replace('/', Path.DirectorySeparatorChar));
    }
    public static string Hash(string path)
    {
        using var input = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(input)).ToLowerInvariant();
    }
    internal static void RejectLinks(string directory)
    {
        var current = new DirectoryInfo(directory);
        for (var parent = current; parent != null; parent = parent.Parent)
            if ((parent.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("组件目录不能使用链接或目录联接。");
        foreach (var item in current.EnumerateFileSystemInfos())
        {
            if ((item.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("组件不能使用链接：" + item.Name);
            if (item is DirectoryInfo child) RejectLinks(child.FullName);
        }
    }
    public Payload CreateSnapshot(string destination)
    {
        Validate();
        if (Directory.Exists(destination)) throw new IOException("组件快照目录已存在。");
        Directory.CreateDirectory(destination);
        var snapshot = new Payload(destination);
        foreach (string relative in Files.Append("SHA256SUMS"))
        {
            var target = snapshot.FileAt(relative); Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            using var source = new FileStream(FileAt(relative), FileMode.Open, FileAccess.Read, FileShare.Read);
            using var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None);
            source.CopyTo(output); output.Flush(true);
        }
        snapshot.Validate();
        return snapshot;
    }
    static string[] CanonicalLines(string path)
    {
        var data = File.ReadAllBytes(path);
        if (data.Length == 0 || data.Length > 65536 || data[^1] != 10 || data.Any(b => b != 9 && b != 10 && (b < 32 || b > 126)))
            throw new IOException("签名清单编码无效。");
        return Encoding.ASCII.GetString(data)[..^1].Split('\n');
    }
    static void VerifySignature(string content, string signature, byte[] key)
    {
        var bytes = File.ReadAllBytes(content);
        var sig = File.ReadAllBytes(signature);
        if (key.Length != 32 || sig.Length != 64) throw new IOException("签名或公钥长度无效。");
        var verifier = new Ed25519Signer();
        verifier.Init(false, new Ed25519PublicKeyParameters(key, 0));
        verifier.BlockUpdate(bytes, 0, bytes.Length);
        if (!verifier.VerifySignature(sig)) throw new IOException("安装包数字签名验证失败。");
    }
    static void ValidateElf(string path)
    {
        byte[] elf = File.ReadAllBytes(path);
        if (elf.Length < 52 || elf.Length > 33554432 || !elf[..7].SequenceEqual(new byte[] { 0x7f, 69, 76, 70, 1, 1, 1 }) ||
            BitConverter.ToUInt16(elf, 16) != 2 || BitConverter.ToUInt16(elf, 18) != 8) throw new IOException("核心组件不是静态 MIPS ELF32：" + path);
        uint flags = BitConverter.ToUInt32(elf, 36);
        if ((flags & 0xf000f000U) != 0x70001000U) throw new IOException("核心组件需要 MIPS32r2 o32 ABI。");
        uint start = BitConverter.ToUInt32(elf, 28);
        ushort width = BitConverter.ToUInt16(elf, 42), count = BitConverter.ToUInt16(elf, 44);
        if (width != 32 || count == 0 || start < 52 || (ulong)start + (ulong)width * count > (ulong)elf.Length) throw new IOException("核心 ELF 程序头无效。");
        bool abi = false;
        for (int i = 0; i < count; ++i)
        {
            int offset = checked((int)(start + i * width)); uint type = BitConverter.ToUInt32(elf, offset);
            if (type is 2 or 3) throw new IOException("核心必须静态链接，不能依赖动态解释器。");
            if (type == 0x70000003U)
            {
                uint p = BitConverter.ToUInt32(elf, offset + 4);
                if ((ulong)p + 24 > (ulong)elf.Length || elf[p + 2] != 32 || elf[p + 3] != 2 || elf[p + 7] != 1) throw new IOException("核心需要 MIPS32r2 双精度 hard-float。");
                abi = true;
            }
        }
        if (!abi) throw new IOException("核心组件缺少 MIPS ABI 信息。");
    }
    void ValidateDeviceShellText(string relative, bool requireShebang)
    {
        byte[] data = File.ReadAllBytes(FileAt(relative));
        string error = "设备 Shell 文件格式无效：" + relative + "。";
        // A final LF is optional in valid shell source, including signed enrollment scripts.
        if (data.Length == 0)
            throw new IOException(error + "文件必须非空。");
        if (data.AsSpan().StartsWith(new byte[] { 0xef, 0xbb, 0xbf }))
            throw new IOException(error + "必须使用无 BOM 的 UTF-8。");
        if (data.Contains((byte)'\r') || data.Contains((byte)0))
            throw new IOException(error + "禁止 CR（包括 CRLF）和 NUL 字节；换行必须为 LF。");
        try { StrictUtf8.GetCharCount(data); }
        catch (DecoderFallbackException e) { throw new IOException(error + "必须使用有效 UTF-8。", e); }
        if (requireShebang && (data.Length < 2 || data[0] != (byte)'#' || data[1] != (byte)'!'))
            throw new IOException(error + "脚本必须以 #! 起始。");
    }
    public void Validate()
    {
        if (!Directory.Exists(Root)) throw new IOException("未找到外置 payload 文件夹，请把 EXE 和完整组件目录一起复制。");
        RejectLinks(Root);
        if (!File.Exists(FileAt("enrollment/bootstrap.v1"))) throw new IOException("缺少完整签名核心安装包：请补入 payload/enrollment 后重新组装校验清单。");
        var required = new[] {
            "tools/adb.exe", "tools/AdbWinApi.dll", "tools/AdbWinUsbApi.dll", "device-setup.sh",
            "usb/S90usb.original", "usb/S90usb.open", "usb/device-open-adb.sh",
            "profile/device-repository-config.sh", "profile/c1-update-check.sh", "profile/SHA256SUMS",
            "profile/repository.ed25519.pub", "profile/repository.url", "profile/core-repository.url",
            "accessories/SHA256SUMS", "accessories/wallpaper.raw", "accessories/neofetch", "accessories/neofetch.upstream",
            "accessories/c1-config.conf", "accessories/c1-logo.txt", "accessories/LICENSE.md",
            "developer/c1publish.exe", "developer/c1publish-linux-amd64", "developer/c1publish-linux-arm64",
            "developer/README.md", "developer/server.url", "developer/repository.ed25519.pub"
        };
        foreach (var relative in required)
            if (!File.Exists(FileAt(relative))) throw new IOException("缺少安装组件：" + relative);
        if (new FileInfo(FileAt("SHA256SUMS")).Length > 65536) throw new IOException("组件清单过大。");
        var actual = Directory.GetFiles(Root, "*", SearchOption.AllDirectories)
            .Select(p => Path.GetRelativePath(Root, p).Replace('\\', '/')).Where(p => p != "SHA256SUMS").Order().ToArray();
        if (actual.Length > 256 || actual.Sum(name => new FileInfo(FileAt(name)).Length) > 512L * 1024 * 1024 ||
            actual.Any(name => new FileInfo(FileAt(name)).Length > 128L * 1024 * 1024)) throw new IOException("组件总量超过安装器限制。");
        var members = new List<string>();
        foreach (string line in File.ReadAllLines(FileAt("SHA256SUMS")))
        {
            var match = Regex.Match(line, @"^([0-9a-f]{64})  ([A-Za-z0-9_./-]+)$");
            if (!match.Success) throw new IOException("安装包文件清单无效。");
            var name = match.Groups[2].Value;
            if (members.Contains(name, StringComparer.OrdinalIgnoreCase) || Hash(FileAt(name)) != match.Groups[1].Value)
                throw new IOException("安装包文件校验失败：" + name);
            members.Add(name);
        }
        if (!actual.SequenceEqual(members.Order())) throw new IOException("安装包包含额外文件。");
        var enrollment = new[] { "bootstrap.v1", "bootstrap.v1.sig", "core.ed25519.pem", "core.ed25519.pub", "app-daemon-bootstrap.sh", "device-core-enroll.sh", "enroll.sh", "release/manifest.v1", "release/manifest.v1.sig", "release/artifacts/C1ancher", "release/artifacts/c1pkg", "release/artifacts/C1ancher-launcher", "release/artifacts/c1updater" };
        if (!actual.Where(name => name.StartsWith("enrollment/")).Order().SequenceEqual(enrollment.Select(name => "enrollment/" + name).Order())) throw new IOException("核心安装包必须为完整的 13 个签名组件文件。");
        Files = members;
        byte[] key = File.ReadAllBytes(FileAt("enrollment/core.ed25519.pub"));
        var pem = File.ReadAllText(FileAt("enrollment/core.ed25519.pem"));
        var der = Convert.FromBase64String(pem.Replace("-----BEGIN PUBLIC KEY-----", "").Replace("-----END PUBLIC KEY-----", "").Trim());
        byte[] prefix = [0x30,0x2a,0x30,0x05,0x06,0x03,0x2b,0x65,0x70,0x03,0x21,0x00];
        if (der.Length != 44 || !der[..12].SequenceEqual(prefix) || !der[12..].SequenceEqual(key)) throw new IOException("核心公钥不一致。");
        VerifySignature(FileAt("enrollment/bootstrap.v1"), FileAt("enrollment/bootstrap.v1.sig"), key);
        var bootstrap = CanonicalLines(FileAt("enrollment/bootstrap.v1"));
        if (bootstrap.Length < 11 || bootstrap.Length > 26 || bootstrap[0] != "C1CORE-BOOTSTRAP 1" || bootstrap[1] != "V\t1.1.0") throw new IOException("核心引导版本无效。");
        string[] tags = ["K", "P", "B", "D", "L", "M", "G", "U"];
        string[] names = ["core.ed25519.pub", "core.ed25519.pem", "app-daemon-bootstrap.sh", "device-core-enroll.sh", "enroll.sh", "release/manifest.v1", "release/manifest.v1.sig", "release/artifacts/c1updater"];
        for (int i = 0; i < names.Length; ++i)
            if (bootstrap[i + 2] != tags[i] + "\t" + Hash(FileAt("enrollment/" + names[i]))) throw new IOException("核心引导文件校验失败。");
        if (bootstrap.Skip(10).Any(line => !Regex.IsMatch(line, "^H\t[0-9a-f]{64}$")) || bootstrap.Skip(10).Distinct().Count() != bootstrap.Length - 10 || !bootstrap.Contains("H\t" + OriginalDaemonHash))
            throw new IOException("安装包未批准原厂设备基线。");
        VerifySignature(FileAt("enrollment/release/manifest.v1"), FileAt("enrollment/release/manifest.v1.sig"), key);
        var release = CanonicalLines(FileAt("enrollment/release/manifest.v1"));
        if (release.Length != 14 || release[0] != "C1CORE-MANIFEST 1" || !Regex.IsMatch(release[1], "^S\t[1-9][0-9]*$") || !Regex.IsMatch(release[2], "^V\t[A-Za-z0-9._+-]+$")) throw new IOException("核心清单无效。");
        Sequence = release[1][2..]; Version = release[2][2..];
        foreach (var field in new[] { (1, "S"), (3, "E"), (9, "D") })
            if (!Regex.IsMatch(release[field.Item1], "^" + field.Item2 + "\t[1-9][0-9]{0,19}$") ||
                !ulong.TryParse(release[field.Item1][2..], out ulong value) || value == 0) throw new IOException("核心数字字段无效。");
        if (release[4] != "T\tmips32r2-little-o32-hard-float-double-static") throw new IOException("核心目标平台无效。");
        foreach (var field in new[] { (2, "V"), (5, "B"), (6, "U"), (7, "C"), (8, "R") })
            if (!Regex.IsMatch(release[field.Item1], "^" + field.Item2 + "\t[A-Za-z0-9](?:[A-Za-z0-9._+-]{0,62}[A-Za-z0-9])?$")) throw new IOException("核心兼容性字段无效。");
        string[] roles = ["c1ancher", "c1pkg", "launcher", "updater"];
        string[] binaries = ["C1ancher", "c1pkg", "C1ancher-launcher", "c1updater"];
        for (int i = 0; i < 4; ++i)
        {
            var fields = release[i + 10].Split('\t');
            var path = FileAt("enrollment/release/artifacts/" + binaries[i]);
            if (fields.Length != 6 || fields[0] != "F" || fields[1] != roles[i] || fields[2] != "artifacts/" + binaries[i] || fields[3] != Hash(path) || fields[4] != new FileInfo(path).Length.ToString() || fields[5] != "700")
                throw new IOException("核心组件校验失败：" + binaries[i]);
            ValidateElf(path);
        }
        if (binaries.Sum(name => new FileInfo(FileAt("enrollment/release/artifacts/" + name)).Length) > 100663296) throw new IOException("核心总大小超限。");
        // Run after signature/digest checks, but before the pinned USB baseline check so
        // malformed (even correctly signed) shell inputs receive a precise format diagnostic.
        foreach (string relative in DeviceShellScripts) ValidateDeviceShellText(relative, requireShebang: true);
        ValidateDeviceShellText(DeviceShellConfig, requireShebang: false);
        if (Hash(FileAt("developer/repository.ed25519.pub")) != Hash(FileAt("profile/repository.ed25519.pub"))) throw new IOException("开发工具附带的公钥与应用仓库公钥不一致。");
        if (!Regex.IsMatch(File.ReadAllText(FileAt("developer/server.url")), @"\Ahttps?://[A-Za-z0-9.-]+(:[0-9]+)?(/[A-Za-z0-9._/-]*)?\n\z")) throw new IOException("发布地址无效。");
        if (Hash(FileAt("usb/S90usb.original")) != OriginalUsbHash) throw new IOException("USB 基线校验失败。");
        if (File.ReadAllBytes(FileAt("profile/repository.ed25519.pub")).Length != 32) throw new IOException("应用公钥无效。");
        if (new FileInfo(FileAt("accessories/wallpaper.raw")).Length != 5624) throw new IOException("壁纸尺寸无效。");
        foreach (var name in new[] { "repository.url", "core-repository.url" })
            if (!Regex.IsMatch(File.ReadAllText(FileAt("profile/" + name)), @"\Ahttps?://[A-Za-z0-9.-]+(:[0-9]+)?(/[A-Za-z0-9._/-]*)?\n\z")) throw new IOException("仓库地址无效。");
    }
}
