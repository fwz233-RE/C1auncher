"""Automatic post-update partition cleanup on disposable host fixtures only."""
import fcntl
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
DRIVER = r'''
#include "launcher/cleanup.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc < 2) return 64;
    int result = argc == 3 ? c1_launcher_cleanup_once(argv[1]) :
                            c1_launcher_cleanup_after_update(argv[1]);
    if (result != 0) fprintf(stderr, "cleanup error: %s\n", strerror(errno));
    return result == 0 ? 0 : 1;
}
'''


class PartitionCleanupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='c1-partition-cleanup-')
        self.root = Path(self.temp.name)
        self.data = self.root / 'usr/data'
        self.c1 = self.data / 'c1'
        self.core = self.c1 / 'core'
        self.releases = self.core / 'releases'
        self.state = self.c1 / 'update/state'
        self.state.mkdir(parents=True, mode=0o700)
        self.releases.mkdir(parents=True, mode=0o700)
        self.c1.chmod(0o777)
        self.state.parent.chmod(0o700)
        self.core.chmod(0o700)
        self.exe = self.release('26-current', launcher=True)
        self.release('25-previous')
        self.release('21-old')
        self.release('24-old')
        (self.core / 'current').symlink_to('releases/26-current')
        (self.core / 'previous').symlink_to('releases/25-previous')
        self.set_state()
        self.driver = self.root / 'driver.c'
        self.driver.write_text(DRIVER)
        self.probe = self.root / 'cleanup-probe'
        self.compile()
        self.process = None

    def tearDown(self):
        if self.process is not None:
            self.process.terminate()
            self.process.wait(timeout=10)
        self.temp.cleanup()

    def compile(self, extra=(), launcher=False):
        sources = [ROOT / 'src/launcher/cleanup.c', ROOT / 'src/update/state.c',
                   ROOT / 'src/security/secure_file.c']
        if launcher:
            sources += [ROOT / 'src/launcher/main.c', ROOT / 'src/launcher/policy.c',
                        ROOT / 'src/platform/liveness.c', ROOT / 'src/platform/shutdown.c']
        else:
            sources += [self.driver]
        sanitizer = (['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
                     if os.environ.get('C1_CLEANUP_TEST_SANITIZE') == '1' else [])
        subprocess.run(['cc', '-D_POSIX_C_SOURCE=200809L', '-std=c11', '-Wall',
                        '-Wextra', '-Wpedantic', '-Werror', '-I' + str(ROOT / 'src'),
                        '-DC1_LAUNCHER_DATA_ROOT="' + str(self.data) + '"', *sanitizer, *extra,
                        *map(str, sources), '-o', str(self.exe if launcher else self.probe)],
                       check=True, capture_output=True)
        if launcher:
            self.exe.chmod(0o700)

    def set_state(self, phase='confirmed', sequence=26, version='current'):
        generation = self.state / 'generations/1'
        generation.mkdir(parents=True, exist_ok=True, mode=0o700)
        generation.parent.chmod(0o700)
        target = generation / 'state.v1'
        target.write_text(f'C1CORE-STATE 1\nP\t{phase}\nS\t{sequence}\nE\t1\nR\t{version}\nD\t' + 'a' * 64 + '\n')
        target.chmod(0o600)
        if not (self.state / 'current').is_symlink():
            (self.state / 'current').symlink_to('generations/1')

    def file(self, relative, text='must survive', mode=0o600, base=None):
        path = (base or self.data) / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        path.chmod(mode)
        return path

    def release(self, name, launcher=False):
        path = self.releases / name / 'artifacts' / ('C1ancher-launcher' if launcher else 'payload')
        path.parent.mkdir(parents=True)
        path.write_text('release')
        path.chmod(0o700)
        return path

    def run_cleanup(self, startup=False):
        return subprocess.run([str(self.probe), str(self.exe)] + (['startup'] if startup else []),
                              capture_output=True, text=True, timeout=40)

    def assert_kept(self, paths):
        for path in paths:
            self.assertTrue(path.exists() or path.is_symlink(), str(path))

    def assert_removed(self, paths):
        for path in paths:
            self.assertFalse(path.exists() or path.is_symlink(), str(path))

    def test_default_removes_every_non_desktop_entry_and_preserves_desktop(self):
        removed = [self.file(name + '/nested/payload', 'obsolete', 0o666).parent.parent for name in (
            'inkwars', 'inkwars-test', 'unknown-app', '.hidden', 'ota_res', 'lost+found',
            'pinao-validation-Ab12cd', 'c1/backup', 'c1/backups', 'c1/book-reader',
            'c1/music-player', 'c1/flomo', 'c1/netease-music', 'c1/pjournal',
            'c1/recovery', 'c1/unknown-data', 'c1/bin/debug-tools', 'c1/core/old-debug')]
        removed += [self.file(name, 'obsolete', 0o666) for name in
                    ('STAGE', 'UPDATE_RET', 'pinao-smoke.sh', '.log', 'c1/bin/test-script')]
        for path in removed:
            if path.is_dir(): path.chmod(0o777)
        kept = [self.file(name) for name in (
            'c1/bin/neofetch', 'c1/bin/c1-update-check', 'c1/neofetch/LICENSE.md',
            'c1/desktop.conf', 'c1/battery-history.cache', 'c1/desktop-summary.cache',
            'c1/desktop-seen.cache', 'c1/wifi/wpa_supplicant.conf', 'c1/enabled',
            'c1/disable-auto-suspend', 'c1/pkg/metrics/device-seed', 'c1/pkg/metrics/queued-report',
            'c1/pkg/cooldown/gate', 'c1/pkg/repository.ed25519.pub', 'c1/pkg/highest-sequence',
            'c1/update/enrolled.v1', 'c1/update/updater-slot', 'c1/update/ready')]
        kept += [self.file('storage/c1/apps/app/payload', base=self.root),
                 self.file('usr/resource/wifi.conf', base=self.root)]
        for name in ('C1ancher', 'app_daemon', 'c1pkg', 'c1updater'):
            path = self.c1 / 'bin' / name
            path.symlink_to('../core/current/artifacts/' + name)
            kept.append(path)
        hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in kept if not p.is_symlink()}
        result = self.run_cleanup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_removed(removed + [self.releases / '21-old', self.releases / '24-old'])
        self.assert_kept(kept + [self.exe, self.releases / '25-previous'])
        self.assertEqual(hashes, {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in kept if not p.is_symlink()})
        self.assertEqual(stat.S_IMODE(self.c1.stat().st_mode), 0o777)
        self.assertEqual(os.readlink(self.core / 'current'), 'releases/26-current')
        self.assertEqual(os.readlink(self.core / 'previous'), 'releases/25-previous')
        self.assertFalse((self.core / '.launcher-cleanup').exists())
        self.assertTrue((self.core / 'cleanup-completed.v1').exists())

    def test_pending_failed_and_mismatched_updates_never_cleanup(self):
        garbage = self.file('unknown/data')
        for phase, sequence, version in [('pending-boot', 26, 'current'), ('prepared', 26, 'current'),
                                          ('idle', 26, 'current'), ('confirmed', 25, 'previous'),
                                          ('confirmed', 26, 'wrong')]:
            self.set_state(phase, sequence, version)
            self.assertNotEqual(self.run_cleanup().returncode, 0)
            self.assert_kept([garbage, self.releases / '21-old'])
        self.set_state()
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_removed([garbage])

    def test_once_per_successful_update_not_each_startup(self):
        garbage = self.file('unknown/data')
        self.assertEqual(self.run_cleanup(startup=True).returncode, 0)
        self.assert_kept([garbage])
        self.assertEqual(self.run_cleanup().returncode, 0)
        later = self.file('new-debug/data')
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_kept([later])
        self.exe = self.release('27-next', launcher=True)
        (self.core / 'current').unlink()
        (self.core / 'current').symlink_to('releases/27-next')
        (self.core / 'previous').unlink()
        (self.core / 'previous').symlink_to('releases/26-current')
        self.set_state('confirmed', 27, 'next')
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_removed([later, self.releases / '25-previous'])
        self.assert_kept([self.releases / '26-current', self.exe])

    def test_update_lock_defers_then_retries_without_replacing_lock(self):
        garbage = self.file('unknown/data')
        lock = self.state / '.c1updater.lock'
        with lock.open('w') as stream:
            lock.chmod(0o600)
            inode = lock.stat().st_ino
            fcntl.lockf(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertNotEqual(self.run_cleanup().returncode, 0)
            self.assert_kept([garbage, self.releases / '21-old'])
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_removed([garbage])
        self.assertEqual(lock.stat().st_ino, inode)

    def test_links_and_special_files_deleted_without_following_targets(self):
        outside = self.file('storage/private/data', base=self.root)
        obsolete = self.file('unknown/payload').parent
        (obsolete / 'link').symlink_to(outside.parent)
        (self.c1 / 'backups').symlink_to(self.core)
        os.link(outside, obsolete / 'hardlink')
        os.mkfifo(obsolete / 'fifo')
        result = self.run_cleanup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_removed([obsolete, self.c1 / 'backups'])
        self.assert_kept([outside, self.exe])
        self.assertEqual(outside.read_text(), 'must survive')
        self.assertEqual(outside.stat().st_nlink, 1)

    def test_open_non_desktop_data_is_removed_but_active_core_waits(self):
        garbage = self.file('unknown/data')
        payload = self.releases / '21-old/artifacts/payload'
        with garbage.open('rb'), payload.open('rb'):
            result = self.run_cleanup()
            self.assertNotEqual(result.returncode, 0)
            self.assert_removed([garbage])
            self.assert_kept([payload])
            self.assertFalse((self.core / 'cleanup-completed.v1').exists())
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_removed([payload])

    def test_abnormal_releases_are_removed_except_current_and_previous(self):
        self.release('not-a-release')
        self.release('100-future')
        (self.releases / 'incomplete').mkdir()
        (self.releases / 'linked').symlink_to('26-current')
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assertEqual(sorted(p.name for p in self.releases.iterdir()), ['25-previous', '26-current'])

    def test_incomplete_pass_retries_private_trash(self):
        trash = self.core / '.launcher-cleanup'
        trash.mkdir(mode=0o700)
        pending = self.file('c1/core/.launcher-cleanup/data--unknown/payload')
        self.file('c1/core/.launcher-cleanup/recovery--old-format/payload')
        self.file('c1/core/.launcher-cleanup/unknown/payload')
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_removed([pending, trash])

    def test_long_names_and_deep_debug_directories_are_cleaned(self):
        longest = self.file('x' * 255)
        nested = self.file('deep/' + 'n/' * 64 + 'payload')
        result = self.run_cleanup()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_removed([longest, nested, self.data / 'deep'])

    def test_budget_does_not_mark_incomplete_pass_complete(self):
        self.compile(['-DC1_LAUNCHER_CLEANUP_BUDGET_MS=0'])
        garbage = self.file('unknown/data')
        self.assertNotEqual(self.run_cleanup().returncode, 0)
        self.assert_kept([garbage])
        self.assertFalse((self.core / 'cleanup-completed.v1').exists())
        self.compile()
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_removed([garbage])

    def test_quarantine_link_and_escaping_pointer_do_not_redirect_cleanup(self):
        outside = self.file('storage/private/data', base=self.root)
        (self.core / '.launcher-cleanup').symlink_to(outside.parent)
        self.assertNotEqual(self.run_cleanup().returncode, 0)
        self.assert_kept([outside, self.releases / '21-old'])
        (self.core / '.launcher-cleanup').unlink()
        (self.core / 'previous').unlink()
        (self.core / 'previous').symlink_to('../../storage/important')
        self.assertNotEqual(self.run_cleanup().returncode, 0)
        self.assert_kept([outside, self.releases / '21-old'])

    def test_shared_core_or_symlinked_container_not_followed(self):
        self.core.chmod(0o777)
        self.assertNotEqual(self.run_cleanup().returncode, 0)
        self.assert_kept([self.releases / '21-old'])
        self.core.chmod(0o700)
        moved = self.data / 'c1-real'
        self.c1.rename(moved)
        self.c1.symlink_to(moved)
        self.assertNotEqual(self.run_cleanup().returncode, 0)
        self.assert_kept([self.releases / '21-old'])

    @unittest.skipUnless(os.geteuid() == 0, 'ownership fixture needs root')
    def test_other_owned_non_desktop_entries_are_removed(self):
        garbage = self.file('unknown/data')
        os.chown(garbage, 65534, 65534)
        os.chown(garbage.parent, 65534, 65534)
        self.assertEqual(self.run_cleanup().returncode, 0)
        self.assert_removed([garbage.parent])

    def test_bind_mount_is_not_traversed_or_marked_complete(self):
        if not shutil.which('unshare'): self.skipTest('unshare unavailable')
        sentinel = self.file('storage/private/data', base=self.root)
        mountpoint = self.data / 'unknown/nested'
        mountpoint.mkdir(parents=True)
        command = ('mount --make-rprivate / && mount --bind ' + shlex.quote(str(sentinel.parent)) +
                   ' ' + shlex.quote(str(mountpoint)) + ' && echo MOUNT_READY && ' +
                   shlex.quote(str(self.probe)) + ' ' + shlex.quote(str(self.exe)))
        result = subprocess.run(['unshare', '--user', '--map-root-user', '--mount',
                                 'sh', '-c', command], capture_output=True, text=True, timeout=40)
        if 'MOUNT_READY' not in result.stdout: self.skipTest('private mount fixture unavailable')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_text(), 'must survive')
        self.assertFalse((self.core / 'cleanup-completed.v1').exists())

    def test_real_launcher_cleans_after_confirmation_without_ui_restart(self):
        self.compile(launcher=True)
        garbage = self.file('unknown/data')
        self.set_state('pending-boot')
        app = self.exe.parent / 'C1ancher'
        app.write_text('#!/usr/bin/python3\nimport os,time\n'
                       'while True:\n os.write(int(os.environ["C1_UI_HEARTBEAT_FD"]), b"H")\n time.sleep(.05)\n')
        app.chmod(0o700)
        self.process = subprocess.Popen([str(self.exe)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(.5)
        self.assertIsNone(self.process.poll())
        self.assert_kept([garbage])
        self.set_state()
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not (self.core / 'cleanup-completed.v1').exists():
            time.sleep(.05)
        self.assertTrue((self.core / 'cleanup-completed.v1').exists())
        self.assert_removed([garbage])
        self.assertIsNone(self.process.poll())
        self.assert_kept([app, self.exe, self.releases / '25-previous'])


if __name__ == '__main__':
    unittest.main()
