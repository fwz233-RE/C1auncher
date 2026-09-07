using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using C1SlimInstaller;
using Org.BouncyCastle.Crypto.Parameters;
using Org.BouncyCastle.Crypto.Signers;

namespace InstallerOfflineTests;

// TEST ONLY. A fresh in-memory Ed25519 seed per fixture, never persisted or shipped.
internal sealed class Fixture : IDisposable
{
    public static string Workspace { get; set; } = "";
    public string Root { get; } = Path.Combine(Path.GetTempPath(), "c1-installer-offline-" + Guid.NewGuid().ToString("N"));
    public Payload Payload { get; }
    readonly Ed25519PrivateKeyParameters key;
    public string Evidence => Path.Combine(Root, "evidence");
    public static readonly string[] Binaries = ["C1ancher", "c1pkg", "C1ancher-launcher", "c1updater"];
    // Independent expected set: a production omission must fail the per-file regression matrix.
    public static readonly string[] ShellScripts = [
        "device-setup.sh", "usb/device-open-adb.sh", "usb/S90usb.original", "usb/S90usb.open",
        "profile/device-repository-config.sh", "profile/c1-update-check.sh",
        "enrollment/app-daemon-bootstrap.sh", "enrollment/device-core-enroll.sh", "enrollment/enroll.sh",
        "accessories/neofetch", "accessories/neofetch.upstream"
    ];
    public const string ShellConfig = "accessories/c1-config.conf";
    public Fixture()
    {
        byte[] seed = RandomNumberGenerator.GetBytes(32);
        key = new Ed25519PrivateKeyParameters(seed, 0);
        CryptographicOperations.ZeroMemory(seed);
        Payload = new Payload(Path.Combine(Root, "payload"));
        Directory.CreateDirectory(Payload.Root);
        foreach (string name in new[] {
            "tools/adb.exe", "tools/AdbWinApi.dll", "tools/AdbWinUsbApi.dll",
            "accessories/c1-logo.txt", "accessories/LICENSE.md",
            "developer/c1publish.exe", "developer/c1publish-linux-amd64", "developer/c1publish-linux-arm64", "developer/README.md" })
            Put(name, "NOT EXECUTABLE: temporary test fixture " + name + "\n");
        foreach (string name in ShellScripts.Where(n => n != "usb/S90usb.original"))
            Put(name, (name == "accessories/neofetch.upstream" ? "#!/usr/bin/env bash\n" : "#!/bin/sh\n") +
                "# Temporary fixture; never executed." + (name == "enrollment/device-core-enroll.sh" ? "" : "\n"));
        Put(ShellConfig, "# Sourced shell config; intentionally no shebang.\nC1_TEST_ONLY=1\n");
        Put("usb/S90usb.original", PublicBaseline("init.d/S90usb"));
        Put("accessories/wallpaper.raw", new byte[5624]);
        byte[] publicKey = key.GeneratePublicKey().GetEncoded();
        Put("enrollment/core.ed25519.pub", publicKey);
        Put("enrollment/core.ed25519.pem", Pem(publicKey));
        // Unrelated application trust key is also temporary public test data.
        byte[] appKey = RandomNumberGenerator.GetBytes(32);
        Put("profile/repository.ed25519.pub", appKey);
        Put("developer/repository.ed25519.pub", appKey);
        foreach (string name in new[] { "profile/repository.url", "profile/core-repository.url", "developer/server.url" })
            Put(name, "https://offline.invalid/test-only\n");
        foreach (string name in Binaries) Put("enrollment/release/artifacts/" + name, MinimalElf());
        RefreshRelease();
        RefreshLayer("profile"); RefreshLayer("accessories"); RefreshSums();
    }
    public static byte[] PublicBaseline(string name) => File.ReadAllBytes(Path.Combine(Workspace, "firmware-analysis", "system-rootfs", "etc", name.Replace('/', Path.DirectorySeparatorChar)));
    public string At(string name) => Payload.FileAt(name);
    public void Put(string name, string text) => Put(name, Encoding.ASCII.GetBytes(text));
    public void Put(string name, byte[] bytes)
    {
        string path = At(name); Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllBytes(path, bytes);
    }
    public static string Pem(byte[] publicKey) => "-----BEGIN PUBLIC KEY-----\n" + Convert.ToBase64String(Convert.FromHexString("302A300506032B6570032100").Concat(publicKey).ToArray()) + "\n-----END PUBLIC KEY-----\n";
    public void Sign(string name)
    {
        byte[] bytes = File.ReadAllBytes(At(name));
        var signer = new Ed25519Signer(); signer.Init(true, key); signer.BlockUpdate(bytes, 0, bytes.Length);
        Put(name + ".sig", signer.GenerateSignature());
    }
    public void RefreshRelease()
    {
        var lines = new List<string> { "C1CORE-MANIFEST 1", "S\t7", "V\t1.2.3-test", "E\t1", "T\tmips32r2-little-o32-hard-float-double-static", "B\t1.1.0", "U\t1.1.0", "C\ttest-only", "R\ttest-only", "D\t1" };
        string[] roles = ["c1ancher", "c1pkg", "launcher", "updater"];
        for (int i = 0; i < Binaries.Length; i++)
        {
            string path = At("enrollment/release/artifacts/" + Binaries[i]);
            lines.Add($"F\t{roles[i]}\tartifacts/{Binaries[i]}\t{Payload.Hash(path)}\t{new FileInfo(path).Length}\t700");
        }
        Put("enrollment/release/manifest.v1", string.Join('\n', lines) + "\n");
        Sign("enrollment/release/manifest.v1"); RefreshBootstrap();
    }
    public void RefreshBootstrap()
    {
        var lines = new List<string> { "C1CORE-BOOTSTRAP 1", "V\t1.1.0" };
        string[] names = ["core.ed25519.pub", "core.ed25519.pem", "app-daemon-bootstrap.sh", "device-core-enroll.sh", "enroll.sh", "release/manifest.v1", "release/manifest.v1.sig", "release/artifacts/c1updater"];
        for (int i = 0; i < names.Length; i++) lines.Add("KPBDLMGU"[i] + "\t" + Payload.Hash(At("enrollment/" + names[i])));
        lines.Add("H\t" + Payload.OriginalDaemonHash);
        Put("enrollment/bootstrap.v1", string.Join('\n', lines) + "\n"); Sign("enrollment/bootstrap.v1");
    }
    public void EditSigned(string name, Func<string, string> edit, bool refreshBootstrap = false)
    {
        Put(name, edit(File.ReadAllText(At(name)))); Sign(name);
        if (refreshBootstrap) RefreshBootstrap();
        RefreshSums();
    }
    public void EditShell(string name, Func<byte[], byte[]> edit)
    {
        Put(name, edit(File.ReadAllBytes(At(name))));
        // Re-sign the bootstrap's script digest with the fixture-only key, so malformed
        // signed scripts reach format validation rather than failing integrity checks.
        if (name.StartsWith("enrollment/", StringComparison.Ordinal)) RefreshBootstrap();
        if (name.StartsWith("profile/", StringComparison.Ordinal)) RefreshLayer("profile");
        if (name.StartsWith("accessories/", StringComparison.Ordinal)) RefreshLayer("accessories");
        RefreshSums();
    }
    void RefreshLayer(string layer)
    {
        var names = Directory.GetFiles(At(layer)).Select(p => Path.GetRelativePath(At(layer), p)).Where(n => n != "SHA256SUMS").Order(StringComparer.Ordinal);
        Put(layer + "/SHA256SUMS", string.Concat(names.Select(n => Payload.Hash(At(layer + "/" + n)) + "  " + n + "\n")));
    }
    public void RefreshSums()
    {
        var names = Directory.GetFiles(Payload.Root, "*", SearchOption.AllDirectories).Select(p => Path.GetRelativePath(Payload.Root, p).Replace('\\', '/')).Where(n => n != "SHA256SUMS").Order(StringComparer.Ordinal).ToArray();
        Put("SHA256SUMS", string.Concat(names.Select(n => Payload.Hash(At(n)) + "  " + n + "\n")));
    }
    public static byte[] MinimalElf()
    {
        // Only ELF and ABI headers: no PT_LOAD, entry point or machine instructions.
        byte[] b = new byte[108]; new byte[] { 0x7f, 69, 76, 70, 1, 1, 1 }.CopyTo(b, 0);
        U16(b, 16, 2); U16(b, 18, 8); U32(b, 28, 52); U32(b, 36, 0x70001000);
        U16(b, 42, 32); U16(b, 44, 1); U32(b, 52, 0x70000003); U32(b, 56, 84);
        b[86] = 32; b[87] = 2; b[91] = 1; return b;
    }
    public static void U16(byte[] b, int offset, ushort value) => BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(offset), value);
    public static void U32(byte[] b, int offset, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(offset), value);
    public void MakeBackup(string root)
    {
        Directory.CreateDirectory(Path.Combine(root, "d261"));
        File.WriteAllBytes(Path.Combine(root, "S80app"), PublicBaseline("init.d/S80app"));
        File.WriteAllBytes(Path.Combine(root, "app_daemon"), PublicBaseline("app_daemon"));
        string file = Path.Combine(root, "d261", "test-only.txt"); File.WriteAllText(file, "test backup\n");
        string manifest = Path.Combine(root, "tree.v1");
        File.WriteAllText(manifest, "D 755 .\nF 644 " + new FileInfo(file).Length + " " + Payload.Hash(file) + " ./test-only.txt\n");
        File.WriteAllText(Path.Combine(root, "complete.v1"), "C1FACTORY 1 " + Payload.Hash(manifest) + "\n");
    }
    public void Dispose() { if (Directory.Exists(Root)) Directory.Delete(Root, true); }
}
