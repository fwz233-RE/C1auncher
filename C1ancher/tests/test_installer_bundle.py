"""Offline bundle tests. All outputs and signed test fixtures stay in TemporaryDirectory.

Ephemeral Ed25519 test keys exist in memory only; signatures use the real verifier.
Synthetic core/tool bytes are NEVER executed and are NOT production artifacts.
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest.mock import patch

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

PROJECT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("installer_bundle", PROJECT / "installer/build-bundle.py")
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="c1-bundle-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.project = self.root / "workspace/C1ancher"
        self.workspace = self.project.parent
        self.adb = self.root / "platform-tools"
        self.publisher = self.root / "publisher"
        self.enrollment = self.root / "signed-test-enrollment"
        self.output = self.root / "output"
        self.key = Ed25519PrivateKey.generate()  # TEST ONLY; never saved or used in a shipped bundle.
        self.app_key = bytes(range(32))
        for name in bundle.LAYERS["tools"]:
            self.put(self.adb / name, b"NOT EXECUTABLE: test platform tools\n")
        self.put(self.adb / "NOTICE.txt", b"platform-tools notice fixture\r\n")
        self.put(self.project / "installer/USER-GUIDE.md", b"# Temporary user guide\r\n")
        self.put(self.project / "installer/THIRD-PARTY-NOTICES.txt", b"third-party fixture notices\r\n")
        for name in bundle.LAYERS["developer"]:
            self.put(self.publisher / name, b"NOT EXECUTABLE: test publisher\n")
        self.put(self.publisher / "repository.ed25519.pub", self.app_key)
        self.put(self.publisher / "server.url", (bundle.PUBLISH_URL + "\n").encode())
        self.put(self.project / "config/app-repo/repository.ed25519.pub", self.app_key)
        self.put(self.project / "scripts/build-repository-profile.ps1", (
            f"[string]$RepositoryUrl = '{bundle.APP_URL}',\n"
            f"[string]$CoreRepositoryUrl = '{bundle.CORE_URL}',\n").encode())
        for name in ("device-open-adb.sh", "device-repository-config.sh", "c1-update-check.sh"):
            self.put(self.project / "scripts" / name, b"#!/bin/sh\n# temporary test fixture\n")
        self.put(self.project / "installer/device-setup.sh", b"#!/bin/sh\n# temporary setup fixture\n")
        for name in bundle.LAYERS["accessories"]:
            if name != "wallpaper.raw":
                content = b"#!/bin/bash\n# temporary accessory\n" if name in ("neofetch", "neofetch.upstream") else b"temporary accessory\n"
                self.put(self.project / "third_party/neofetch" / name, content)
        self.put(self.workspace / "Pic/wallpaper.raw", b"\0" * 5624)
        # Public factory script only; the expected factory digest is never mocked.
        self.original_usb = (PROJECT.parent / "firmware-analysis/system-rootfs/etc/init.d/S90usb").read_bytes()
        self.put(self.workspace / "firmware-analysis/system-rootfs/etc/init.d/S90usb", self.original_usb)
        self.make_enrollment(self.enrollment, self.key)

    @staticmethod
    def put(path, content):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

    def make_enrollment(self, directory, key, version="1.0.0", sequence=1):
        pub = key.public_key()
        self.put(directory / "core.ed25519.pub", pub.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw))
        self.put(directory / "core.ed25519.pem", pub.public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
        for name in ("app-daemon-bootstrap.sh", "device-core-enroll.sh", "enroll.sh"):
            self.put(directory / name, b"#!/bin/sh\n# signed temporary test script only\n")
        lines = ["C1CORE-MANIFEST 1", f"S\t{sequence}", f"V\t{version}", "E\t1",
                 "T\tmips32r2-little-o32-hard-float-double-static", "B\t1.1.0", "U\t1.1.0", "C\ttest-only", "R\ttest-only", "D\t1"]
        for role, name in zip(("c1ancher", "c1pkg", "launcher", "updater"),
                              ("C1ancher", "c1pkg", "C1ancher-launcher", "c1updater")):
            data = b"NOT EXECUTABLE: TEST-ONLY " + name.encode() + version.encode()
            if role == "updater":
                data += b" C1RECOVERY-VERIFIER 1.1.0"
            # Structural ELF fixture only: no machine instructions or runnable entry point.
            header = bytearray(108)
            header[:7] = b"\x7fELF\x01\x01\x01"
            struct.pack_into("<HH", header, 16, 2, 8)
            struct.pack_into("<I", header, 28, 52)
            struct.pack_into("<I", header, 36, 0x70001000)
            struct.pack_into("<HH", header, 42, 32, 1)
            struct.pack_into("<II", header, 52, 0x70000003, 84)
            header[86:88] = b"\x20\x02"
            header[91] = 1
            data = bytes(header) + data
            self.put(directory / "release/artifacts" / name, data)
            lines.append(f"F\t{role}\tartifacts/{name}\t{bundle.digest(data)}\t{len(data)}\t700")
        manifest = ("\n".join(lines) + "\n").encode()
        self.put(directory / "release/manifest.v1", manifest)
        self.put(directory / "release/manifest.v1.sig", key.sign(manifest))
        self.resign_bootstrap(directory, key)

    def resign_bootstrap(self, directory, key):
        lines = ["C1CORE-BOOTSTRAP 1", "V\t1.1.0"]
        for tag, name in zip("KPBDLMGU", ("core.ed25519.pub", "core.ed25519.pem", "app-daemon-bootstrap.sh",
                            "device-core-enroll.sh", "enroll.sh", "release/manifest.v1", "release/manifest.v1.sig", "release/artifacts/c1updater")):
            lines.append(tag + "\t" + bundle.digest((directory / name).read_bytes()))
        lines.append("H\t" + bundle.ORIGINAL_DAEMON_SHA256)
        manifest = ("\n".join(lines) + "\n").encode()
        self.put(directory / "bootstrap.v1", manifest)
        self.put(directory / "bootstrap.v1.sig", key.sign(manifest))

    def build(self, enrollment=True, **kwargs):
        return bundle.build_bundle(self.enrollment if enrollment else None, self.adb, self.publisher, self.output,
                                   project=self.project, workspace=self.workspace, **kwargs)

    def snapshots(self):
        return {p.relative_to(self.output).as_posix(): p.read_bytes() for p in self.output.rglob("*") if p.is_file()}

    def assert_sums(self, path, root=None):
        root = root or path.parent
        raw = path.read_bytes()
        self.assertTrue(raw.endswith(b"\n"))
        self.assertNotIn(b"\r", raw)
        names = []
        for line in raw.decode().splitlines():
            checksum, name = line.split("  ")
            self.assertEqual(checksum, bundle.digest((root / name).read_bytes()), name)
            names.append(name)
        self.assertEqual(names, sorted(set(names)))

    def test_complete_signed_bundle_and_every_manifest(self):
        self.assertTrue(self.build())
        payload = self.output / "payload"
        bundle.exact_tree(payload / "enrollment", set(bundle.ENROLLMENT))
        self.assertEqual(sum(p.is_file() for p in (payload / "enrollment").rglob("*")), 13)
        for layer in bundle.LAYERS:
            self.assert_sums(payload / layer / "SHA256SUMS")
        for filename, (base, _) in bundle.ENROLLMENT_SUMS.items():
            self.assert_sums(payload / filename, payload / base)
        self.assert_sums(payload / "SHA256SUMS")
        self.assert_sums(self.output / "SHA256SUMS")
        self.assertEqual(len((payload / "accessories/SHA256SUMS").read_text().splitlines()), 6)
        self.assertEqual(len((payload / "profile/SHA256SUMS").read_text().splitlines()), 5)
        self.assertEqual(json.loads((self.output / "BUNDLE-STATUS.json").read_text())["status"], "READY")
        self.assertEqual((payload / "device-setup.sh").read_bytes(), (self.project / "installer/device-setup.sh").read_bytes())
        for name in bundle.ENROLLMENT:
            self.assertEqual((payload / "enrollment" / name).read_bytes(), (self.enrollment / name).read_bytes())

    def test_delivery_guides_and_notices_are_copied_verbatim_and_manifested(self):
        self.build()
        sources = {
            "USER-GUIDE.md": self.project / "installer/USER-GUIDE.md",
            "THIRD-PARTY-NOTICES.txt": self.project / "installer/THIRD-PARTY-NOTICES.txt",
            "PLATFORM-TOOLS-NOTICE.txt": self.adb / "NOTICE.txt",
        }
        manifest = (self.output / "SHA256SUMS").read_text()
        for name, source in sources.items():
            self.assertEqual((self.output / name).read_bytes(), source.read_bytes())
            self.assertIn("  " + name + "\n", manifest)
        self.assertIn("Read USER-GUIDE.md before use", (self.output / "README.txt").read_text())
        self.assertFalse((self.output / "GO-LICENSE.txt").exists())
        self.assertFalse((self.output / "DOTNET-THIRD-PARTY-NOTICES.txt").exists())
        self.assert_sums(self.output / "SHA256SUMS")
        before = self.snapshots()
        bundle.refresh_manifest(self.output, project=self.project)
        self.assertEqual(before, self.snapshots())

    def test_explicit_runtime_licenses_are_manifested_and_refresh_preserves_them(self):
        go = self.root / "license-inputs/LICENSE"
        dotnet = self.root / "license-inputs/ThirdPartyNotices.txt"
        self.put(go, b"Go license fixture\r\n")
        self.put(dotnet, b".NET notices fixture\r\n")
        self.build(go_license=go, dotnet_notices=dotnet)
        self.assertEqual((self.output / "GO-LICENSE.txt").read_bytes(), go.read_bytes())
        self.assertEqual((self.output / "DOTNET-THIRD-PARTY-NOTICES.txt").read_bytes(), dotnet.read_bytes())
        manifest = (self.output / "SHA256SUMS").read_text()
        self.assertIn("  GO-LICENSE.txt\n", manifest)
        self.assertIn("  DOTNET-THIRD-PARTY-NOTICES.txt\n", manifest)
        before = self.snapshots()
        bundle.refresh_manifest(self.output, project=self.project)
        self.assertEqual(before, self.snapshots())
        self.assert_sums(self.output / "SHA256SUMS")

    def test_missing_required_notice_or_explicit_optional_source_rejected(self):
        notice = self.adb / "NOTICE.txt"
        saved = notice.read_bytes()
        notice.unlink()
        with self.assertRaisesRegex(bundle.BundleError, "Missing input"):
            self.build()
        self.assertFalse(self.output.exists())
        self.put(notice, saved)
        with self.assertRaisesRegex(bundle.BundleError, "Missing input"):
            self.build(go_license=self.root / "absent-license")
        self.assertFalse(self.output.exists())

    def test_refresh_requires_delivery_guide(self):
        self.build()
        (self.output / "USER-GUIDE.md").unlink()
        before = self.snapshots()
        with self.assertRaisesRegex(bundle.BundleError, "Missing bundle files"):
            bundle.refresh_manifest(self.output, project=self.project)
        self.assertEqual(before, self.snapshots())

    def test_missing_enrollment_is_not_ready_template(self):
        self.assertFalse(self.build(enrollment=False))
        self.assertEqual(json.loads((self.output / "BUNDLE-STATUS.json").read_text())["status"], "NOT_READY")
        self.assertIn("NOT_READY", (self.output / "payload/NOT_READY.txt").read_text())
        self.assertFalse((self.output / "payload/enrollment/bootstrap.v1").exists())
        readme = (self.output / "README.txt").read_text()
        self.assertIn("NOT_READY templates cannot install or use refresh-manifest to become READY", readme)
        self.assertIn("--output-dir pointing to a NEW directory", readme)
        self.assertIn("For an existing READY bundle only", readme)
        self.assert_sums(self.output / "payload/SHA256SUMS")
        before = self.snapshots()
        with self.assertRaisesRegex(bundle.BundleError, "NOT_READY"):
            bundle.refresh_manifest(self.output, project=self.project)
        self.assertEqual(before, self.snapshots())

    def test_cli_missing_enrollment_exit_two(self):
        go = self.root / "explicit-go-license.txt"
        dotnet = self.root / "explicit-dotnet-notices.txt"
        self.put(go, b"test Go license\n")
        self.put(dotnet, b"test .NET notices\n")
        original = bundle.build_bundle
        def temporary_build(*args, **kwargs):
            return original(*args, project=self.project, workspace=self.workspace, **kwargs)
        with patch.object(bundle, "build_bundle", side_effect=temporary_build), contextlib.redirect_stdout(io.StringIO()) as stdout:
            result = bundle.main(["build", "--adb-platform-tools-dir", str(self.adb), "--publisher-dir", str(self.publisher), "--output-dir", str(self.output), "--go-license", str(go), "--dotnet-notices", str(dotnet)])
        self.assertEqual(result, 2)
        self.assertEqual((self.output / "GO-LICENSE.txt").read_bytes(), go.read_bytes())
        self.assertEqual((self.output / "DOTNET-THIRD-PARTY-NOTICES.txt").read_bytes(), dotnet.read_bytes())
        self.assertIn("NOT_READY", stdout.getvalue())

    def test_missing_explicit_enrollment_is_error_not_template(self):
        shutil.rmtree(self.enrollment)
        with self.assertRaisesRegex(bundle.BundleError, "Missing"):
            self.build()
        self.assertFalse(self.output.exists())

    def test_existing_output_is_never_overwritten(self):
        self.output.mkdir()
        self.put(self.output / "keep.txt", b"keep")
        with self.assertRaisesRegex(bundle.BundleError, "already exists"):
            self.build()
        self.assertEqual(self.snapshots(), {"keep.txt": b"keep"})

    def test_only_allowlisted_publisher_and_adb_inputs_are_read(self):
        # Deliberately named excluded sentinel files; contain no actual secrets.
        excluded = [self.publisher / "token.txt", self.publisher / "private-key.pem",
                    self.publisher / ".venv/huge.py", self.adb / "build/source.c"]
        for path in excluded:
            self.put(path, b"EXCLUDED TEST SENTINEL (not a credential)")
        read = bundle.read_file
        def checked(path, *args, **kwargs):
            self.assertNotIn(path, excluded)
            return read(path, *args, **kwargs)
        with patch.object(bundle, "read_file", side_effect=checked):
            self.build()
        self.assertFalse(any("token" in name or "private-key" in name or ".venv" in name for name in self.snapshots()))

    def test_extra_enrollment_file_rejected_without_reading(self):
        extra = self.enrollment / "token.txt"
        self.put(extra, b"EXCLUDED TEST SENTINEL")
        read = bundle.read_file
        def checked(path, *args, **kwargs):
            self.assertNotEqual(path, extra)
            return read(path, *args, **kwargs)
        with patch.object(bundle, "read_file", side_effect=checked), self.assertRaisesRegex(bundle.BundleError, "Unexpected"):
            self.build()

    def test_missing_enrollment_member_rejected(self):
        (self.enrollment / "release/artifacts/c1pkg").unlink()
        with self.assertRaisesRegex(bundle.BundleError, "Missing bundle files"):
            self.build()

    def test_fake_signature_rejected(self):
        self.put(self.enrollment / "bootstrap.v1.sig", b"\0" * 64)
        with self.assertRaisesRegex(bundle.BundleError, "signature mismatch"):
            self.build()
        self.assertFalse(self.output.exists())

    def test_release_signature_rejected_even_when_bootstrap_resigned(self):
        self.put(self.enrollment / "release/manifest.v1.sig", b"\0" * 64)
        self.resign_bootstrap(self.enrollment, self.key)
        with self.assertRaisesRegex(bundle.BundleError, "signature mismatch: release"):
            self.build()

    def test_core_key_pin_rejected(self):
        with self.assertRaisesRegex(bundle.BundleError, "trust key mismatch"):
            self.build(expected_key_sha256="0" * 64)

    def test_mismatched_public_pem_rejected(self):
        other = Ed25519PrivateKey.generate().public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
        self.put(self.enrollment / "core.ed25519.pem", other)
        with self.assertRaisesRegex(bundle.BundleError, "public keys mismatch"):
            self.build()

    def test_refresh_rejects_binary_only_replacement_before_any_write(self):
        self.build()
        self.put(self.output / "payload/enrollment/release/artifacts/C1ancher", b"new unsigned core")
        before = self.snapshots()
        with self.assertRaisesRegex(bundle.BundleError, "Signed release component mismatch"):
            bundle.refresh_manifest(self.output, project=self.project)
        self.assertEqual(before, self.snapshots())

    def test_refresh_rejects_release_only_resigning(self):
        self.build()
        target = self.output / "payload/enrollment"
        self.put(target / "release/manifest.v1", (target / "release/manifest.v1").read_bytes().replace(b"S\t1\n", b"S\t2\n"))
        self.put(target / "release/manifest.v1.sig", self.key.sign((target / "release/manifest.v1").read_bytes()))
        before = self.snapshots()
        with self.assertRaisesRegex(bundle.BundleError, "Bootstrap binding mismatch"):
            bundle.refresh_manifest(self.output, project=self.project)
        self.assertEqual(before, self.snapshots())

    def test_whole_enrollment_replacement_updates_without_exe_rebuild(self):
        exe = self.root / "gui.exe"
        self.put(exe, b"NOT EXECUTABLE: test GUI")
        self.build(installer_exe=exe)
        target = self.output / "payload/enrollment"
        shutil.rmtree(target)
        self.make_enrollment(target, self.key, version="2.0.0", sequence=2)
        signed_before = {name: (target / name).read_bytes() for name in bundle.ENROLLMENT}
        bundle.refresh_manifest(self.output, project=self.project)
        self.assertEqual((self.output / "C1SlimInstaller.exe").read_bytes(), exe.read_bytes())
        self.assertEqual(signed_before, {name: (target / name).read_bytes() for name in bundle.ENROLLMENT})
        state = json.loads((self.output / "BUNDLE-STATUS.json").read_text())
        self.assertEqual(state["enrollment"]["version"], "2.0.0")
        self.assert_sums(self.output / "SHA256SUMS")
        self.assert_sums(self.output / "payload/SHA256SUMS")

    def test_refresh_refuses_changed_core_trust_root(self):
        self.build()
        target = self.output / "payload/enrollment"
        shutil.rmtree(target)
        self.make_enrollment(target, Ed25519PrivateKey.generate())
        before = self.snapshots()
        with self.assertRaisesRegex(bundle.BundleError, "trust key mismatch"):
            bundle.refresh_manifest(self.output, project=self.project)
        self.assertEqual(before, self.snapshots())

    def test_refresh_rejects_extra_file_before_reading_it(self):
        self.build()
        self.put(self.output / "payload/developer/token.txt", b"EXCLUDED TEST SENTINEL")
        before = self.snapshots()
        with self.assertRaisesRegex(bundle.BundleError, "Unexpected bundle entry"):
            bundle.refresh_manifest(self.output, project=self.project)
        self.assertEqual(before, self.snapshots())

    def test_refresh_recreates_unsigned_manifests_but_never_signed_files(self):
        self.build()
        before = self.snapshots()
        for path in self.output.rglob("*SHA256SUMS"):
            path.unlink()
        bundle.refresh_manifest(self.output, project=self.project)
        self.assertEqual(before, self.snapshots())

    def test_profile_uses_reviewed_http_defaults(self):
        self.build()
        self.assertEqual((self.output / "payload/profile/repository.url").read_bytes(), b"http://www.fwz233.com/c1/v2\n")
        self.assertEqual((self.output / "payload/profile/core-repository.url").read_bytes(), b"http://www.fwz233.com/c1/core/v1/stable\n")

    def test_upstream_default_drift_is_rejected(self):
        path = self.project / "scripts/build-repository-profile.ps1"
        self.put(path, path.read_bytes().replace(b"http://", b"https://"))
        with self.assertRaisesRegex(bundle.BundleError, "default changed"):
            self.build()

    def test_publisher_trust_key_mismatch_is_rejected(self):
        self.put(self.publisher / "repository.ed25519.pub", b"x" * 32)
        with self.assertRaisesRegex(bundle.BundleError, "trust key mismatch"):
            self.build()
        self.assertFalse(self.output.exists())

    def test_publisher_url_with_credentials_is_rejected(self):
        self.put(self.publisher / "server.url", b"http://user:pass@www.fwz233.com\n")
        with self.assertRaisesRegex(bundle.BundleError, "server.url"):
            self.build()

    def test_missing_adb_dll_rejected_without_output(self):
        (self.adb / "AdbWinUsbApi.dll").unlink()
        with self.assertRaisesRegex(bundle.BundleError, "Missing input"):
            self.build()
        self.assertFalse(self.output.exists())

    def test_usb_transformation_matches_existing_script(self):
        self.build()
        candidate = (self.output / "payload/usb/S90usb.open").read_bytes()
        self.assertEqual(candidate, self.original_usb.replace(b"\t#/etc/init.d/usb/adb\t$1", b"\t/etc/init.d/usb/adb\t$1"))
        with self.assertRaisesRegex(bundle.BundleError, "SHA-256 mismatch"):
            bundle.open_usb(self.original_usb + b"\n")

    def test_wallpaper_size_rejected(self):
        self.put(self.workspace / "Pic/wallpaper.raw", b"invalid")
        with self.assertRaisesRegex(bundle.BundleError, "5624"):
            self.build()

    def test_hardlinked_allowlisted_input_rejected(self):
        original = self.adb / "adb.exe"
        os.link(original, self.root / "adb-hardlink")
        with self.assertRaisesRegex(bundle.BundleError, "Hard-linked"):
            self.build()

    def test_symlink_input_rejected(self):
        path = self.publisher / "README.md"
        path.unlink()
        target = self.root / "link-target.txt"
        self.put(target, b"link target")
        try:
            path.symlink_to(target)
        except OSError as exc:
            self.skipTest(f"Windows symbolic-link permission unavailable: {exc}")
        with self.assertRaisesRegex(bundle.BundleError, "Links/reparse"):
            self.build()

    def test_elf_rejects_wrong_architecture_dynamic_and_soft_float(self):
        original = (self.enrollment / "release/artifacts/C1ancher").read_bytes()
        bundle.validate_elf(original, "test fixture")
        wrong_arch = bytearray(original)
        struct.pack_into("<H", wrong_arch, 18, 62)
        dynamic = bytearray(original)
        struct.pack_into("<I", dynamic, 52, 3)
        soft_float = bytearray(original)
        soft_float[91] = 3
        bad_headers = bytearray(original)
        struct.pack_into("<I", bad_headers, 28, 2**32 - 1)
        for data in (wrong_arch, dynamic, soft_float, bad_headers):
            with self.subTest(data=data[:52]), self.assertRaises(bundle.BundleError):
                bundle.validate_elf(bytes(data), "test fixture")

    def test_signed_noncanonical_bootstrap_rejected(self):
        path = self.enrollment / "bootstrap.v1"
        content = path.read_bytes().replace(b"\n", b"\r\n")
        self.put(path, content)
        self.put(self.enrollment / "bootstrap.v1.sig", self.key.sign(content))
        with self.assertRaisesRegex(bundle.BundleError, "Non-canonical"):
            self.build()

    def test_signed_unapproved_daemon_baseline_rejected(self):
        path = self.enrollment / "bootstrap.v1"
        content = path.read_bytes().replace(bundle.ORIGINAL_DAEMON_SHA256.encode(), b"0" * 64)
        self.put(path, content)
        self.put(self.enrollment / "bootstrap.v1.sig", self.key.sign(content))
        with self.assertRaisesRegex(bundle.BundleError, "factory daemon baseline"):
            self.build()

    def test_all_unsigned_shell_sources_normalize_crlf_and_preserve_inputs(self):
        before = {}
        for target, source in bundle.SHELL_SOURCES.items():
            path = self.project / source
            content = path.read_bytes().replace(b"\n", b"\r\n")
            self.put(path, content)
            before[target] = content
        signed_before = {name: (self.enrollment / name).read_bytes() for name in bundle.ENROLLMENT}
        self.build()
        for target, source in bundle.SHELL_SOURCES.items():
            self.assertEqual((self.project / source).read_bytes(), before[target])
            self.assertEqual((self.output / "payload" / target).read_bytes(), before[target].replace(b"\r\n", b"\n"))
        self.assertEqual(signed_before, {name: (self.output / "payload/enrollment" / name).read_bytes() for name in bundle.ENROLLMENT})
        self.assertEqual((self.output / "payload/usb/S90usb.original").read_bytes(), self.original_usb)
        self.assert_sums(self.output / "payload/SHA256SUMS")
        before_refresh = self.snapshots()
        bundle.refresh_manifest(self.output, project=self.project)
        self.assertEqual(before_refresh, self.snapshots())

    def test_shell_invalid_encodings_rejected(self):
        for target, source in bundle.SHELL_SOURCES.items():
            path = self.project / source
            original = path.read_bytes()
            for invalid in (b"\xef\xbb\xbf" + original, original.replace(b"\n", b"\r"),
                            original + b"\0\n", original + b"\xff\n"):
                with self.subTest(target=target, invalid=invalid[:20]):
                    self.put(path, invalid)
                    with self.assertRaisesRegex(bundle.BundleError, "Device shell"):
                        self.build()
                    self.assertFalse(self.output.exists())
            self.put(path, original)

    def test_refresh_rejects_crlf_scripts_before_any_write(self):
        self.build()
        for target in bundle.SHELL_SOURCES:
            path = self.output / "payload" / target
            original = path.read_bytes()
            self.put(path, original.replace(b"\n", b"\r\n"))
            before = self.snapshots()
            with self.subTest(target=target), self.assertRaisesRegex(bundle.BundleError, "Device shell"):
                bundle.refresh_manifest(self.output, project=self.project)
            self.assertEqual(before, self.snapshots())
            self.put(path, original)

    def test_signed_shell_crlf_rejected_without_rewriting_or_resigning(self):
        for name in ("app-daemon-bootstrap.sh", "device-core-enroll.sh", "enroll.sh"):
            path = self.enrollment / name
            original = path.read_bytes()
            self.put(path, original.replace(b"\n", b"\r\n"))
            self.resign_bootstrap(self.enrollment, self.key)  # Test-only key, never production.
            before = {item: (self.enrollment / item).read_bytes() for item in bundle.ENROLLMENT}
            with self.subTest(name=name), self.assertRaisesRegex(bundle.BundleError, "Device shell"):
                self.build()
            self.assertFalse(self.output.exists())
            self.assertEqual(before, {item: (self.enrollment / item).read_bytes() for item in bundle.ENROLLMENT})
            self.put(path, original)
            self.resign_bootstrap(self.enrollment, self.key)

    def test_shell_without_final_newline_is_legal_and_preserved(self):
        for name in ("app-daemon-bootstrap.sh", "device-core-enroll.sh", "enroll.sh"):
            path = self.enrollment / name
            self.put(path, path.read_bytes().rstrip(b"\n"))
        self.resign_bootstrap(self.enrollment, self.key)
        for target, source in bundle.SHELL_SOURCES.items():
            path = self.project / source
            self.put(path, path.read_bytes().rstrip(b"\n"))
        self.build()
        for name in bundle.ENROLLMENT:
            self.assertEqual((self.output / "payload/enrollment" / name).read_bytes(), (self.enrollment / name).read_bytes())
        for target, source in bundle.SHELL_SOURCES.items():
            self.assertEqual((self.output / "payload" / target).read_bytes(), (self.project / source).read_bytes())

    def test_signed_and_pinned_inputs_cannot_be_normalized(self):
        for name in bundle.SIGNED_SHELL + ("usb/S90usb.original", "usb/S90usb.open"):
            with self.subTest(name=name), self.assertRaisesRegex(bundle.BundleError, "Refusing to normalize"):
                bundle.normalize_shell_source(b"#!/bin/sh\r\n", name)

    def test_public_workspace_sources_only(self):
        # Read only named public inputs; no build/artifact/credential discovery.
        key = bundle.public_defaults(PROJECT)
        self.assertEqual(len(key), 32)
        self.assertEqual(len(bundle.read_file(PROJECT.parent / "Pic/wallpaper.raw")), 5624)
        self.assertEqual(bundle.digest(self.original_usb), bundle.ORIGINAL_USB_SHA256)


if __name__ == "__main__":
    unittest.main(verbosity=2)
