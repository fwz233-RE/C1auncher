"""WSL/root-only integration of the EXE's actual enrollment permission command.
Every mutating command targets a private temporary directory. Production signed
files are read-only inputs; MIPS programs, ADB, and enrollment are never executed.
The native driver links the unchanged production secure reader/Ed25519 verifier.
"""
import os
from pathlib import Path
import runpy
import shutil
import stat
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]
PACKER = runpy.run_path(str(PROJECT / 'installer/build-bundle.py'))
ENROLLMENT = Path(os.environ.get('C1_ENROLLMENT_TEST_INPUT',
    str(PROJECT.parent / 'build/installer-enrollment-1.3.4-20260906')))
REMOTE = '/storage/c1-installer-' + '0' * 32


class EnrollmentMetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.geteuid() != 0:
            raise RuntimeError('Use WSL -u root: test requires real root-owned fixture metadata, not a bypass')
        cls.build_temp = tempfile.TemporaryDirectory(prefix='c1-native-metadata-')
        cls.addClassCleanup(cls.build_temp.cleanup)
        cls.driver = Path(cls.build_temp.name) / 'verify'
        sources = [PROJECT / 'tests/installer_metadata_verifier.c', PROJECT / 'src/security/secure_file.c',
                   PROJECT / 'src/security/trusted_ed25519.c']
        sources += [PROJECT / 'third_party/ed25519' / (name + '.c') for name in ('fe', 'ge', 'sc', 'sha512', 'verify')]
        subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(PROJECT / 'src'), '-I' + str(PROJECT / 'third_party/ed25519'),
                        *map(str, sources), '-o', str(cls.driver)], check=True, capture_output=True)
        cli = r'D:\c1slim\C1ancher\installer\tests\bin\Release\net8.0\Installer.OfflineTests.dll'
        result = subprocess.run(['/mnt/c/Program Files/dotnet/dotnet.exe', cli,
                                 '--print-enrollment-metadata-command', REMOTE],
                                check=True, capture_output=True, timeout=15)
        cls.production_command = result.stdout.decode('utf-8-sig').strip()
        print(f'Exported production permission command: {len(cls.production_command.encode("utf-8"))} bytes', flush=True)
        if len(cls.production_command.encode('utf-8')) > 3500:
            raise RuntimeError('Permission command exceeds legacy ADB payload budget')
        if REMOTE not in cls.production_command or '\n' in cls.production_command:
            raise RuntimeError('Unexpected exported command; no commands executed')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='c1-staging-metadata-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stage = self.root / 'stage'
        self.bundle = self.stage / 'enrollment'
        shutil.copytree(ENROLLMENT, self.bundle)
        self.dirs = [self.bundle, self.bundle / 'release', self.bundle / 'release/artifacts']
        self.files = [self.bundle / name for name in PACKER['ENROLLMENT']]
        for path in self.dirs:
            path.chmod(0o777)
        for path in self.files:
            path.chmod(0o666)
        self.bytes_before = {p.relative_to(self.bundle).as_posix(): p.read_bytes() for p in self.files}
        self.command = self.production_command.replace(REMOTE, str(self.stage))
        self.assertNotIn('/storage/', self.command)

    def prepare(self, success=True, env=None):
        result = subprocess.run(['/bin/sh', '-c', self.command], capture_output=True, timeout=10, env=env)
        self.assertEqual(result.returncode == 0, success, repr(result.stdout) + repr(result.stderr))
        return result

    def verify(self, bootstrap=True, success=True):
        payload = self.bundle / ('bootstrap.v1' if bootstrap else 'release/manifest.v1')
        result = subprocess.run([str(self.driver), str(payload), str(payload) + '.sig', str(self.bundle / 'core.ed25519.pub')],
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode == 0, success, repr(result.stderr))
        return result

    def assert_unmodified(self):
        self.assertEqual({p.relative_to(self.bundle).as_posix(): p.read_bytes() for p in self.files}, self.bytes_before)

    def test_real_0666_failure_then_permission_fix_validates_original_signatures(self):
        result = self.verify(success=False)
        self.assertIn(b'secure file metadata rejected', result.stderr)
        self.prepare()
        for path in self.dirs:
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700)
            self.assertEqual((path.stat().st_uid, path.stat().st_gid), (0, 0))
        for path in self.files:
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertEqual((path.stat().st_uid, path.stat().st_gid, path.stat().st_nlink), (0, 0, 1))
        self.assert_unmodified()
        self.verify()
        self.verify(bootstrap=False)
        self.prepare()  # Safe repeat on the same private stage.
        self.assert_unmodified()
        self.verify()

    def test_wrong_owner_and_group_are_corrected_only_in_private_stage(self):
        for path in self.dirs + self.files:
            os.chown(path, 65534, 65534)
        self.prepare()
        for path in self.dirs + self.files:
            self.assertEqual((path.stat().st_uid, path.stat().st_gid), (0, 0))
        self.assert_unmodified()
        self.verify()

    def test_each_file_symlink_rejected_before_any_mode_change(self):
        for path in self.files:
            with self.subTest(path=path.name):
                data = path.read_bytes()
                outside = self.root / 'untouched'
                outside.write_bytes(data)
                outside.chmod(0o666)
                path.unlink()
                path.symlink_to(outside)
                self.prepare(success=False)
                self.assertEqual(stat.S_IMODE(outside.stat().st_mode), 0o666)
                self.assertEqual(stat.S_IMODE(self.bundle.stat().st_mode), 0o777)
                path.unlink()
                path.write_bytes(data)
                path.chmod(0o666)

    def test_each_file_hardlink_rejected_before_any_mode_change(self):
        for path in self.files:
            with self.subTest(path=path.name):
                outside = self.root / 'hardlink'
                os.link(path, outside)
                self.prepare(success=False)
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o666)
                self.assertEqual(stat.S_IMODE(self.bundle.stat().st_mode), 0o777)
                outside.unlink()

    def test_fifo_rejected_before_any_mode_change(self):
        path = self.bundle / 'bootstrap.v1'
        path.unlink()
        os.mkfifo(path)
        self.prepare(success=False)
        self.assertEqual(stat.S_IMODE(self.bundle.stat().st_mode), 0o777)

    def test_directory_symlink_rejected_without_touching_target(self):
        original = self.bundle / 'release/artifacts'
        outside = self.root / 'outside-artifacts'
        original.rename(outside)
        original.symlink_to(outside, target_is_directory=True)
        self.prepare(success=False)
        self.assertEqual(stat.S_IMODE(outside.stat().st_mode), 0o777)
        self.assertEqual(stat.S_IMODE(self.bundle.stat().st_mode), 0o777)

    def test_correct_modes_do_not_bypass_cryptographic_rejection(self):
        self.prepare()
        path = self.bundle / 'bootstrap.v1.sig'
        path.write_bytes(bytes(64))
        result = self.verify(success=False)
        self.assertIn(b'Ed25519 signature rejected', result.stderr)

    def test_chown_or_chmod_failure_reports_failure(self):
        for name in ('chown', 'chmod'):
            with self.subTest(command=name):
                tools = self.root / ('fail-' + name)
                tools.mkdir()
                executable = tools / name
                executable.write_text('#!/bin/sh\nexit 1\n')
                executable.chmod(0o700)
                env = dict(os.environ, PATH=str(tools) + ':/usr/bin:/bin')
                self.prepare(success=False, env=env)
                self.assert_unmodified()


if __name__ == '__main__':
    unittest.main(verbosity=2)
