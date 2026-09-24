#!/usr/bin/env python3
"""Trusted offline-install host tests; only disposable test keys are generated.

Run: python3 tests/test_pkg_local.py
Needs Linux, a C11 compiler, Python stdlib, openssl, sha256sum and tar. Compiles
its own CLI in /tmp; never cleans the shared build directory or touches devices.
"""
import hashlib
import io
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]
ENTRY = "bin/c1-ime-service"
ID = "c1-ime"


def run(*args, **kwargs):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs)


def remove_tree(path):
    if path.exists():
        for directory, _, _ in os.walk(path):
            os.chmod(directory, 0o700)
        shutil.rmtree(path)


class LocalInstall(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="c1pkg-local-")
        cls.base = Path(cls.tmp.name)
        cls.root = cls.base / "runtime"
        cls.binary = cls.base / "c1pkg-local-host"
        defines = {
            "C1PKG_TEST_ROOT": cls.root,
            "C1PKG_STATE_ROOT": cls.root / "state",
            "C1PKG_APPS_ROOT": cls.root / "storage/c1/apps",
            "C1PKG_STAGING_ROOT": cls.root / "storage/c1/pkg/staging",
            "C1PKG_TRASH_ROOT": cls.root / "storage/c1/pkg/trash",
            "C1_APP_RUN_PATH": cls.root / "run.lock",
            "C1_APP_LEASE_PATH": cls.root / "lease.lock",
            "C1_APP_MODE_GUARD_PATH": cls.root / "mode-guard.lock",
            "C1_APP_MODE_PATH": cls.root / "app.mode",
        }
        sources = ["tests/pkg_local_host.c", "src/pkg/repo.c", "src/pkg/metrics.c", "src/security/sha256.c", "src/security/secure_file.c",
                   "src/pkg/util.c", "src/pkg/text.c", "src/pkg/tui_model.c",
                   "src/pkg/launch_mode.c", "src/platform/app_lease.c"]
        sources += [f"third_party/ed25519/{name}.c" for name in ("fe", "ge", "sc", "sha512", "verify")]
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O1",
            "-Isrc", "-Isrc/pkg", "-Ithird_party/ed25519"]
        command += [f'-D{name}="{path}"' for name, path in defines.items()]
        compiled = subprocess.run(command + sources + ["-o", str(cls.binary)], cwd=PROJECT, capture_output=True, text=True)
        if compiled.returncode:
            raise RuntimeError(compiled.stdout + compiled.stderr)
        cls.private = cls.base / "TEST-ONLY-private.pem"
        cls.key = cls.base / "TEST-ONLY-public.raw"
        run("openssl", "genpkey", "-algorithm", "ED25519", "-out", str(cls.private))
        public = run("openssl", "pkey", "-in", str(cls.private), "-pubout", "-outform", "DER").stdout
        assert len(public) == 44
        cls.key.write_bytes(public[-32:])
        cls.key.chmod(0o600)

    @classmethod
    def tearDownClass(cls):
        remove_tree(cls.root)
        cls.tmp.cleanup()

    def setUp(self):
        remove_tree(self.root)
        (self.root / "storage").mkdir(parents=True)
        self.repo = self.root / "bundle"
        (self.repo / "packages").mkdir(parents=True)
        self.current = self.root / f"storage/c1/apps/{ID}/current"
        self.state = self.root / "state"
        self.key.chmod(0o600)
        self.bundle()

    def cli(self, *args, ok=True, code=None, env=None, key=None):
        environment = os.environ.copy()
        environment.pop("C1PKG_KEY", None)
        environment.pop("C1PKG_REPO", None)
        for name in tuple(environment):
            if name.startswith("C1PKG_TEST_"):
                environment.pop(name)
        environment.update(env or {})
        result = subprocess.run([str(self.binary), "--key", str(key or self.key), *map(str, args)],
                                cwd=self.root, env=environment, capture_output=True, text=True, timeout=15)
        if code is not None:
            self.assertEqual(result.returncode, code, result.stderr)
        elif ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def install(self, **kwargs):
        return self.cli("install-local", self.repo, ID, **kwargs)

    def sign(self, data=None):
        if data is not None:
            (self.repo / "index.v1").write_bytes(data)
        run("openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(self.private),
            "-in", str(self.repo / "index.v1"), "-out", str(self.repo / "index.v1.sig"))

    def bundle(self, version="1.0.0", *, archive="packages/c1-ime.tar.gz", seq=1,
               special=None, index_format=2, manifest_version=None):
        self.archive = self.repo / "packages/c1-ime.tar.gz"
        with tarfile.open(self.archive, "w:gz", format=tarfile.USTAR_FORMAT) as tar:
            manifest = (f"C1PKG-PACKAGE 2\nid\t{ID}\nversion\t{manifest_version or version}\n"
                        f"entry\t{ENTRY}\nmode\tterminal\n").encode()
            entries = [("manifest.v1", manifest, 0o644),
                       (f"payload/{ENTRY}", b"#!/bin/sh\nexit 0\n", 0o755),
                       ("payload/share/rime-data/test.dict.yaml", b"name: test-only\n", 0o644)]
            for name, data, mode in entries:
                member = tarfile.TarInfo(name)
                member.size, member.mode = len(data), mode
                tar.addfile(member, io.BytesIO(data))
            if special:
                member = tarfile.TarInfo(special[0])
                member.type = special[1]
                member.linkname = special[2] if len(special) > 2 else ""
                tar.addfile(member)
        data = self.archive.read_bytes()
        fields = ["P", ID, version, "C1 IME", archive, hashlib.sha256(data).hexdigest(), str(len(data)), ENTRY]
        if index_format == 2:
            fields.append("Test Only")
        self.sign((f"C1PKG-INDEX {index_format}\nS\t{seq}\n" + "\t".join(fields) + "\n").encode())

    def assert_clean_failure(self):
        self.assertFalse(self.current.exists())
        self.assertFalse((self.state / "transaction").exists())
        staging = self.root / "storage/c1/pkg/staging"
        if staging.exists():
            self.assertEqual(list(staging.iterdir()), [])

    def test_read_only_real_install_and_online_isolation(self):
        self.state.mkdir(mode=0o700)
        (self.state / "cache").mkdir(mode=0o700)
        sentinels = {"highest-sequence": b"999999\n", "cache/verified.v1": b"online sentinel",
                     "repository.url": b"invalid online configuration\n"}
        for name, data in sentinels.items():
            (self.state / name).write_bytes(data)
        before = {str(p.relative_to(self.repo)): (p.read_bytes(), p.stat().st_mtime_ns)
                  for p in self.repo.rglob("*") if p.is_file()}
        for p in self.repo.rglob("*"):
            p.chmod(0o555 if p.is_dir() else 0o444)
        self.repo.chmod(0o555)
        result = self.install(env={"C1PKG_REPO": "file:///invalid/online"})
        self.assertIn("installed c1-ime 1.0.0", result.stdout)
        self.assertEqual((self.current / ENTRY).read_bytes(), b"#!/bin/sh\nexit 0\n")
        self.assertTrue((self.current / "share/rime-data/test.dict.yaml").is_file())
        self.assertEqual(self.current.readlink(), Path("versions/1.0.0"))
        self.assertEqual((self.current / ENTRY).stat().st_mode & 0o222, 0)
        self.assertFalse((self.state / "transaction").exists())
        for name, data in sentinels.items():
            self.assertEqual((self.state / name).read_bytes(), data)
        for name, (data, modified) in before.items():
            self.assertEqual((self.repo / name).read_bytes(), data)
            self.assertEqual((self.repo / name).stat().st_mtime_ns, modified)
        self.assertEqual(len(list(self.repo.rglob("*"))), 4)

    def test_same_version_always_authenticates_index(self):
        self.install()
        original = (self.repo / "index.v1.sig").read_bytes()
        (self.repo / "index.v1.sig").write_bytes(bytes(64))
        self.assertIn("signature rejected", self.install(ok=False).stderr)
        (self.repo / "index.v1.sig").write_bytes(original)
        self.archive.unlink()
        self.assertIn("skipped c1-ime 1.0.0", self.install().stdout)
        self.assertEqual(self.current.readlink(), Path("versions/1.0.0"))

    def test_index_tamper_and_wrong_key(self):
        data = (self.repo / "index.v1").read_bytes()
        (self.repo / "index.v1").write_bytes(data.replace(b"1.0.0", b"2.0.0"))
        self.assertIn("signature rejected", self.install(ok=False).stderr)
        (self.repo / "index.v1").write_bytes(data)
        key = self.root / "wrong.raw"
        key.write_bytes(bytes(32))
        self.assertIn("signature rejected", self.install(key=key, ok=False).stderr)
        self.assert_clean_failure()

    def test_key_safety(self):
        original = self.key.read_bytes()
        for size in (0, 31, 33, 64):
            with self.subTest(size=size):
                key = self.root / "bad-key"
                key.write_bytes(original[:size] if size <= 32 else original + b"x" * (size - 32))
                self.install(key=key, ok=False)
        self.key.chmod(0o666)
        self.install(ok=False)
        self.key.chmod(0o600)
        link = self.root / "key-link"
        link.symlink_to(self.key)
        self.install(key=link, ok=False)
        link.unlink()
        os.link(self.key, link)
        self.install(ok=False)
        link.unlink()
        os.mkfifo(link)
        self.install(key=link, ok=False)
        self.assert_clean_failure()

    def test_signed_parser_and_archive_path_rejections(self):
        for path in ("../outside.tar.gz", "/tmp/outside.tar.gz", "packages/../escape.tar.gz",
                     "packages//app.tar.gz", "packages/./app.tar.gz", "packages\\app.tar.gz"):
            with self.subTest(path=path):
                self.bundle(archive=path)
                self.assertIn("invalid package record", self.install(ok=False).stderr)
        self.bundle()
        original = (self.repo / "index.v1").read_bytes()
        record = original.splitlines(keepends=True)[2]
        for invalid in (original[:-1], original.replace(b"\n", b"\r\n"), original + record,
                        original.replace(b"S\t1", b"S\t0"), original + b"\x00\n"):
            with self.subTest(index=invalid[:20]):
                self.sign(invalid)
                self.install(ok=False)
        self.assert_clean_failure()

    def test_missing_id_and_argument_validation(self):
        result = self.cli("install-local", self.repo, "missing", ok=False)
        self.assertIn("not found in verified index", result.stderr)
        self.cli("install-local", self.repo, "../c1-ime", ok=False)
        self.cli("install-local", self.repo, code=2)
        self.cli("install-local", self.repo, ID, "extra", code=2)
        self.assert_clean_failure()

    def test_repository_path_safety_and_relative_directory(self):
        link = self.root / "repo-link"
        link.symlink_to(self.repo, target_is_directory=True)
        self.cli("install-local", link, ID, ok=False)
        self.cli("install-local", link / "packages/..", ID, ok=False)
        self.cli("install-local", self.repo / "../bundle", ID, ok=False)
        ancestor = self.root / "ancestor"
        ancestor.symlink_to(self.root, target_is_directory=True)
        self.cli("install-local", ancestor / "bundle", ID, ok=False)
        fifo = self.root / "fifo-repo"
        os.mkfifo(fifo)
        self.cli("install-local", fifo, ID, ok=False)
        self.cli("install-local", "./bundle/", ID)

    def test_metadata_and_archive_file_types(self):
        for filename in ("index.v1", "index.v1.sig", "packages/c1-ime.tar.gz"):
            path = self.repo / filename
            content = path.read_bytes()
            for kind in ("symlink", "hardlink", "fifo", "directory"):
                with self.subTest(filename=filename, kind=kind):
                    backing = self.root / "backing"
                    backing.write_bytes(content)
                    path.unlink()
                    if kind == "symlink":
                        path.symlink_to(backing)
                    elif kind == "hardlink":
                        os.link(backing, path)
                    elif kind == "fifo":
                        os.mkfifo(path)
                    else:
                        path.mkdir()
                    self.install(ok=False)
                    path.rmdir() if kind == "directory" else path.unlink()
                    backing.unlink()
                    path.write_bytes(content)
            self.assert_clean_failure()
        packages = self.repo / "packages"
        saved = self.root / "packages"
        packages.rename(saved)
        packages.symlink_to(saved, target_is_directory=True)
        self.install(ok=False)
        self.assert_clean_failure()

    def test_bounds(self):
        for filename, sizes in (("index.v1.sig", (0, 63, 65)), ("index.v1", (256 * 1024 + 1,)),
                                ("packages/c1-ime.tar.gz", (0, 32 * 1024 * 1024 + 1))):
            path = self.repo / filename
            original = path.read_bytes()
            for size in sizes:
                with self.subTest(filename=filename, size=size):
                    with path.open("wb") as f:
                        f.truncate(size)
                    self.install(ok=False)
                    self.assert_clean_failure()
            path.write_bytes(original)
        data = (self.repo / "index.v1").read_bytes()
        fields = data.splitlines()[2].split(b"\t")
        for size in (0, 32 * 1024 * 1024 + 1, 18446744073709551616):
            fields[6] = str(size).encode()
            self.sign(b"C1PKG-INDEX 2\nS\t1\n" + b"\t".join(fields) + b"\n")
            self.install(ok=False)
        self.assert_clean_failure()

    def test_archive_tamper_and_copied_bytes(self):
        data = self.archive.read_bytes()
        self.archive.write_bytes(bytes([data[0] ^ 1]) + data[1:])
        self.assertIn("SHA-256 mismatch", self.install(ok=False).stderr)
        self.archive.write_bytes(data[:-1])
        self.install(ok=False)
        self.archive.write_bytes(data + b"x")
        self.install(ok=False)
        self.archive.write_bytes(data)
        self.assertIn("SHA-256 mismatch", self.install(ok=False, env={"C1PKG_TEST_TAMPER_STAGING": "1"}).stderr)
        self.assert_clean_failure()
        self.install(env={"C1PKG_TEST_MUTATE_SOURCE": str(self.archive)})
        self.assertEqual(self.archive.read_bytes(), b"replaced source")
        self.assertTrue((self.current / ENTRY).is_file())

    def test_signed_tar_attacks_and_manifest_mismatch(self):
        attacks = [("payload/../../escaped", tarfile.REGTYPE), ("/tmp/c1pkg-escape", tarfile.REGTYPE),
                   ("payload/link", tarfile.SYMTYPE, "/tmp"),
                   ("payload/hard", tarfile.LNKTYPE, f"payload/{ENTRY}"),
                   ("payload/fifo", tarfile.FIFOTYPE), ("payload/device", tarfile.CHRTYPE),
                   ("payload/.c1pkg-entry", tarfile.REGTYPE), ("payload/.c1pkg-mode", tarfile.REGTYPE)]
        for attack in attacks:
            with self.subTest(attack=attack):
                self.bundle(special=attack)
                self.install(ok=False)
                self.assert_clean_failure()
        self.bundle(manifest_version="9.0.0")
        self.assertIn("manifest does not match", self.install(ok=False).stderr)
        self.assert_clean_failure()

    def test_copy_cancellation_and_source_size_races(self):
        original = self.archive.read_bytes()
        for change in ("cancel", "grow", "truncate"):
            with self.subTest(change=change):
                self.archive.write_bytes(original)
                self.install(ok=False, env={"C1PKG_TEST_SOURCE_CHANGE": change})
                self.assert_clean_failure()

    def test_signed_tar_size_limits(self):
        for count, size in ((1, 16 * 1024 * 1024 + 1), (5, 16 * 1024 * 1024)):
            with self.subTest(count=count, size=size):
                data = bytes(size)
                with tarfile.open(self.archive, "w:gz", format=tarfile.USTAR_FORMAT) as tar:
                    for i in range(count):
                        member = tarfile.TarInfo(f"payload/large-{i}")
                        member.size = size
                        tar.addfile(member, io.BytesIO(data))
                lines = (self.repo / "index.v1").read_bytes().splitlines()
                fields = lines[2].split(b"\t")
                archive = self.archive.read_bytes()
                fields[5] = hashlib.sha256(archive).hexdigest().encode()
                fields[6] = str(len(archive)).encode()
                self.sign(b"\n".join(lines[:2] + [b"\t".join(fields)]) + b"\n")
                self.assertIn("archive declared size exceeds limit", self.install(ok=False).stderr)
                self.assert_clean_failure()

    def test_upgrade_downgrade_retained_and_recovery(self):
        self.install()
        self.bundle("2.0.0")
        self.install()
        self.assertEqual(self.current.readlink(), Path("versions/2.0.0"))
        self.bundle("1.0.0")
        self.assertIn("downgrade", self.install(ok=False).stderr)
        self.cli("rollback", ID)
        self.assertEqual(self.current.readlink(), Path("versions/1.0.0"))
        self.bundle("2.0.0")
        retained = self.current.parent / "versions/2.0.0" / ENTRY
        retained.chmod(0o755)
        retained.write_bytes(b"#!/bin/sh\nexit 1\n")
        retained.chmod(0o555)
        self.assertIn("skipped", self.install().stdout)
        self.assertEqual(self.current.readlink(), Path("versions/1.0.0"))
        retained.chmod(0o755)
        retained.write_bytes(b"#!/bin/sh\nexit 0\n")
        retained.chmod(0o555)
        self.install()
        self.bundle("3.0.0")
        self.install(code=77, env={"C1PKG_TEST_CRASH_VERSION": "3.0.0"})
        self.assertTrue((self.state / "transaction").exists())
        self.assertEqual(self.current.readlink(), Path("versions/2.0.0"))
        self.assertIn("c1-ime\t3.0.0", self.cli("list").stdout)
        self.assertEqual(self.current.readlink(), Path("versions/3.0.0"))
        self.assertEqual((self.current.parent / "previous").readlink(), Path("versions/2.0.0"))
        self.assertFalse((self.state / "transaction").exists())
        self.assertIn("skipped", self.install().stdout)

    def test_v1_index_compatible(self):
        self.bundle(index_format=1)
        self.install()
        self.assertTrue((self.current / ENTRY).is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)
