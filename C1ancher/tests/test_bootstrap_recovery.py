"""Exercise the real fixed verifier with temporary signed generations only.
All signing material is generated inside the test temporary directory and
never uses a production key, configured repository, rootfs or connected device.
"""
from pathlib import Path
import fcntl
import hashlib
import os
import shutil
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get('C1_UPDATE_TEST_BUILD_DIR', 'build/lifecycle-fixes'))
if not BUILD.is_absolute():
    BUILD = ROOT / BUILD

LAUNCHER = b'''#!/usr/bin/env python3
import os, pathlib, struct, sys, time
ready = pathlib.Path(sys.argv[sys.argv.index('--ready-file') + 1])
ready.write_text(os.environ['FIXTURE_DIGEST'] + '\\n')
ready.chmod(0o600)
fd = int(os.environ['C1_SUPERVISOR_HEARTBEAT_FD'])
first = True
while True:
    if first or os.environ.get('FIXTURE_BEATS') == 'continuous':
        first = False
        try:
            os.write(fd, struct.pack('=qq', int(time.monotonic() * 1000), os.getpid()))
        except BlockingIOError:
            pass
    time.sleep(.1)
'''


class RecoveryVerifierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['make', f'BUILD_DIR={BUILD}', str(BUILD / 'host-c1updater')], cwd=ROOT, check=True)
        cls.updater = BUILD / 'host-c1updater'

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='c1-recovery-verifier-')
        self.root = Path(self.temp.name)
        for name in ('state', 'staging', 'core'):
            (self.root / name).mkdir(mode=0o700)
        self.state, self.core, self.staging = [self.root / name for name in ('state', 'core', 'staging')]
        self.private = self.root / 'test-only.pem'
        self.key = self.root / 'key'
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'ED25519', '-out', str(self.private)],
                       check=True, capture_output=True)
        public = subprocess.run(['openssl', 'pkey', '-in', str(self.private), '-pubout', '-outform', 'DER'],
                                check=True, capture_output=True).stdout
        self.key.write_bytes(public[-32:])
        self.key.chmod(0o600)
        self.env = dict(os.environ)

    def tearDown(self):
        self.temp.cleanup()

    def command(self, *args, **kwargs):
        return subprocess.run([str(self.updater), *map(str, args)], env=self.env,
                              capture_output=True, text=True, timeout=10, **kwargs)

    def fixture(self, sequence=1, minimum_bootstrap='1.0.0', minimum_updater='1.0.0', launcher=None, app=None):
        release = self.root / f'input-{sequence}'
        (release / 'artifacts').mkdir(parents=True, mode=0o700)
        payloads = [app or b'app', b'pkg', launcher or Path('/bin/true').read_bytes(), self.updater.read_bytes()]
        # Use a real ELF for recovery exec; it returns 0 only for the test marker
        # command by sharing the production updater's supervise implementation.
        manifest = (f'C1CORE-MANIFEST 1\nS\t{sequence}\nV\t1.0.{sequence}\nE\t1\n'
                    'T\tmips32r2-little-o32-hard-float-double-static\n'
                    f'B\t{minimum_bootstrap}\nU\t{minimum_updater}\nC\tc1-core-v1\nR\tfixture\nD\t1\n')
        for role, name, payload in zip(('c1ancher', 'c1pkg', 'launcher', 'updater'),
                                        ('C1ancher', 'c1pkg', 'C1ancher-launcher', 'c1updater'), payloads):
            path = release / 'artifacts' / name
            path.write_bytes(payload)
            path.chmod(0o600)
            manifest += f'F\t{role}\tartifacts/{name}\t{hashlib.sha256(payload).hexdigest()}\t{len(payload)}\t700\n'
        (release / 'manifest.v1').write_text(manifest)
        (release / 'manifest.v1').chmod(0o600)
        self.sign(release)
        return release

    def sign(self, release):
        subprocess.run(['openssl', 'pkeyutl', '-sign', '-rawin', '-inkey', str(self.private),
                        '-in', str(release / 'manifest.v1'), '-out', str(release / 'manifest.v1.sig')],
                       check=True, capture_output=True)
        (release / 'manifest.v1.sig').chmod(0o600)

    def prepare(self, release):
        return self.command('prepare-local', release, self.staging, self.core, self.state, self.key)

    def enrolled(self):
        release = self.fixture()
        result = self.prepare(release)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.command('bootstrap-activate', self.state, self.core, self.key)
        self.assertEqual(result.returncode, 0, result.stderr)
        return self.core / 'releases/1-1.0.1'

    def recovery(self, which='current'):
        return self.command('recovery-supervise', self.state, self.core, self.key, which,
                            self.root / 'ready', self.core / 'current/artifacts/C1ancher-launcher')

    def test_tampered_updater_never_executes_self_proof(self):
        release = self.enrolled()
        sentinel = self.root / 'executed'
        updater = release / 'artifacts/c1updater'
        updater.write_text(f'#!/bin/sh\ntouch "{sentinel}"\nexit 0\n')
        updater.chmod(0o700)
        result = self.recovery()
        self.assertEqual(result.returncode, 71, result.stderr)
        self.assertFalse(sentinel.exists())

    def test_bad_signature_key_and_escaping_pointer_fail_closed(self):
        release = self.enrolled()
        signature = release / 'manifest.v1.sig'
        original = signature.read_bytes()
        signature.write_bytes(b'x' * 64)
        self.assertEqual(self.recovery().returncode, 71)
        signature.write_bytes(original)
        original_key = self.key.read_bytes()
        self.key.write_bytes(b'x' * 32)
        self.assertEqual(self.recovery().returncode, 71)
        self.key.write_bytes(original_key)
        (self.core / 'current').unlink()
        (self.core / 'current').symlink_to('../outside')
        self.assertEqual(self.recovery().returncode, 71)

    def test_signed_incompatible_prepare_and_coldboot_are_rejected(self):
        release = self.fixture(minimum_updater='9.0.0')
        result = self.prepare(release)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('trusted', result.stderr)
        installed = self.core / 'releases/1-1.0.1'
        installed.parent.mkdir(mode=0o700)
        shutil.copytree(release, installed)
        for path in (installed / 'artifacts').iterdir():
            path.chmod(0o700)
        (self.core / 'current').symlink_to('releases/1-1.0.1')
        result = self.command('supervise', self.state, self.core, self.key,
                              self.root / 'ready', self.core / 'current/artifacts/C1ancher-launcher')
        self.assertEqual(result.returncode, 71, result.stderr)
        self.assertIn('trusted', result.stderr)
        self.assertEqual(self.recovery().returncode, 71)

    def test_activation_rechecks_compatibility_before_pointer_change(self):
        self.enrolled()
        release = self.fixture(2)
        self.assertEqual(self.prepare(release).returncode, 0)
        installed = self.core / 'releases/2-1.0.2'
        manifest = installed / 'manifest.v1'
        manifest.write_text(manifest.read_text().replace('B\t1.0.0\n', 'B\t9.0.0\n'))
        self.sign(installed)
        pointer = os.readlink(self.core / 'current')
        result = self.command('activate', self.state, self.core, self.key)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('trusted', result.stderr)
        self.assertEqual(os.readlink(self.core / 'current'), pointer)

    def test_pending_incompatibility_rolls_back_to_legacy_release(self):
        self.enrolled()
        release = self.fixture(2)
        self.assertEqual(self.prepare(release).returncode, 0)
        self.assertEqual(self.command('activate', self.state, self.core, self.key).returncode, 0)
        installed = self.core / 'releases/2-1.0.2'
        manifest = installed / 'manifest.v1'
        manifest.write_text(manifest.read_text().replace('U\t1.0.0\n', 'U\t9.0.0\n'))
        self.sign(installed)
        result = self.command('recover', self.state, self.core, self.key)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(os.readlink(self.core / 'current'), 'releases/1-1.0.1')
        self.assertIn('phase=idle', self.command('state', self.state).stdout)

    def test_recovery_lock_busy_never_changes_pointer(self):
        self.enrolled()
        pointer = os.readlink(self.core / 'current')
        lock = self.state / '.c1updater.lock'
        with lock.open('r+') as stream:
            fcntl.lockf(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            inode = lock.stat().st_ino
            result = self.recovery('previous')
            self.assertEqual(result.returncode, 75, result.stderr)
            self.assertEqual(os.readlink(self.core / 'current'), pointer)
            self.assertEqual(lock.stat().st_ino, inode)
        self.assertTrue(lock.exists())

    def test_verified_generation_exec_keeps_supervision_cancellable(self):
        self.enrolled()
        process = subprocess.Popen([str(self.updater), 'recovery-supervise', str(self.state), str(self.core),
                                    str(self.key), 'current', str(self.root / 'ready'),
                                    str(self.core / 'current/artifacts/C1ancher-launcher')],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                try:
                    executable = os.readlink(f'/proc/{process.pid}/exe')
                except FileNotFoundError:
                    break
                if '/releases/' in executable:
                    break
                time.sleep(.02)
            self.assertIn('/releases/', executable)
            process.terminate()
            self.assertEqual(process.wait(timeout=6), 0)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    def shutdown_chain(self, pending):
        # Real launcher and updater, signed disposable fixture. The simulated
        # UI exits with the shutdown code; it never executes poweroff.
        self.enrolled()
        launcher = self.root / 'test-launcher'
        subprocess.run(['cc', '-D_POSIX_C_SOURCE=200809L', '-std=c11', '-Wall',
                        '-Wextra', '-Wpedantic', '-Werror', '-I' + str(ROOT / 'src'),
                        str(ROOT / 'src/launcher/main.c'), str(ROOT / 'src/launcher/cleanup.c'),
                        str(ROOT / 'src/update/state.c'), str(ROOT / 'src/security/secure_file.c'),
                        str(ROOT / 'src/launcher/policy.c'),
                        str(ROOT / 'src/platform/liveness.c'), str(ROOT / 'src/platform/shutdown.c'),
                        '-o', str(launcher)], check=True)
        calls = self.root / 'ui-starts'
        app = f'#!/bin/sh\necho started >> "{calls}"\nexit 76\n'.encode()
        release = self.fixture(2, launcher=launcher.read_bytes(), app=app)
        self.assertEqual(self.prepare(release).returncode, 0)
        self.assertEqual(self.command('activate', self.state, self.core, self.key).returncode, 0)
        if not pending:
            self.assertEqual(self.command('confirm', self.state, self.core, self.key).returncode, 0)
        pointer = os.readlink(self.core / 'current')
        phase = self.command('state', self.state).stdout
        result = self.command('supervise', self.state, self.core, self.key,
                              self.root / 'ready', self.core / 'current/artifacts/C1ancher-launcher')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls.read_text().splitlines(), ['started'])
        self.assertEqual(os.readlink(self.core / 'current'), pointer)
        self.assertEqual(self.command('state', self.state).stdout, phase)
        self.assertFalse((self.root / 'ready').exists())

    def test_shutdown_stops_both_supervisors_without_restart(self):
        self.shutdown_chain(pending=False)

    def test_shutdown_during_pending_boot_does_not_rollback_or_restart(self):
        self.shutdown_chain(pending=True)

    def pending_health(self, continuous):
        self.enrolled()
        release = self.fixture(2, launcher=LAUNCHER)
        self.assertEqual(self.prepare(release).returncode, 0)
        self.assertEqual(self.command('activate', self.state, self.core, self.key).returncode, 0)
        self.env['FIXTURE_DIGEST'] = hashlib.sha256((release / 'manifest.v1').read_bytes()).hexdigest()
        self.env['FIXTURE_BEATS'] = 'continuous' if continuous else 'once'
        process = subprocess.Popen([str(self.updater), 'supervise', str(self.state), str(self.core),
                                    str(self.key), str(self.root / 'ready'),
                                    str(self.core / 'current/artifacts/C1ancher-launcher')], env=self.env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 50
            wanted = 'phase=confirmed' if continuous else 'phase=idle'
            while time.monotonic() < deadline:
                state = self.command('state', self.state).stdout
                if wanted in state:
                    break
                if not continuous:
                    self.assertNotIn('phase=confirmed', state, 'one ready/beat must never confirm a hung UI')
                time.sleep(.2)
            self.assertIn(wanted, state)
        finally:
            process.terminate()
            try:
                process.wait(timeout=6)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    def test_one_ready_and_one_heartbeat_never_confirms_hung_ui(self):
        self.pending_health(False)

    def test_continuous_ui_heartbeats_confirm_after_full_window(self):
        self.pending_health(True)


if __name__ == '__main__':
    unittest.main()
