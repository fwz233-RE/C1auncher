"""Run the actual normalized USB helper only in a rewritten private filesystem.
No ADB, device, real mount, process control, or system path is accessed by helper.
"""
import hashlib
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]
PACKER = runpy.run_path(str(PROJECT / 'installer/build-bundle.py'))


def sha(data):
    return hashlib.sha256(data).hexdigest()


class InstallerUsbTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='c1-usb-helper-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.original = (PROJECT.parent / 'firmware-analysis/system-rootfs/etc/init.d/S90usb').read_bytes()
        self.candidate = PACKER['open_usb'](self.original)
        for relative in ('etc/init.d', 'usr/data', 'storage', 'dev/shm', 'proc',
                         'sys/kernel/config/usb_gadget/demo/configs/c.1/ffs.adb',
                         'sys/kernel/config/usb_gadget/demo/configs/c.1/ffs.mtp'):
            (self.root / relative).mkdir(parents=True, exist_ok=True)
        self.target = self.root / 'etc/init.d/S90usb'
        self.target.write_bytes(self.original)
        (self.root / 'dev/shm/c1-S90usb.open-root-adb').write_bytes(self.candidate)
        self.mounts = self.root / 'proc/mounts'
        self.mounts.write_text('/dev/root / ext4 ro,relatime 0 0\n')
        source = PACKER['normalize_shell_source']((PROJECT / 'scripts/device-open-adb.sh').read_bytes(), 'usb/device-open-adb.sh').decode('utf-8')
        for prefix in ('/etc/init.d', '/usr/data', '/storage', '/dev/shm', '/proc/mounts',
                       '/sys/kernel', '/data/misc', '/adb_keys'):
            source = source.replace(prefix, str(self.root) + prefix)
        self.assertEqual(source.count('/bin/mount'), 3)
        source = source.replace('/bin/mount', 'fixture_mount')
        self.assertEqual(source.count('pidof adbd'), 1)
        source = source.replace('pidof adbd', 'true')
        prelude = f'''fixture_mount() {{
    printf '%s\\n' "$2" >> '{self.root}/mount-events'
    case "$2" in
      remount,rw) [ "${{TEST_FAIL_RW:-0}}" != 1 ] || return 1; option=rw ;;
      remount,ro) [ "${{TEST_FAIL_RO:-0}}" != 1 ] || return 1; option=ro ;;
      *) return 1 ;;
    esac
    printf '/dev/root / ext4 %s,relatime 0 0\\n' "$option" > '{self.mounts}'
}}
sync() {{ :; }}
'''
        source = source.replace('set -eu\n', 'set -eu\n' + prelude, 1)
        self.helper = self.root / 'device-open-adb.sh'
        self.helper.write_bytes(source.encode('utf-8'))
        self.backups = [self.root / 'etc/init.d/S90usb.c1-original',
                        self.root / 'usr/data/c1/recovery/open-adb/S90usb.original',
                        self.root / 'storage/c1/recovery/open-adb/S90usb.original']

    def run_helper(self, action='install', success=True, **extra):
        env = {k: v for k, v in os.environ.items() if k not in ('ENV', 'BASH_ENV')}
        env.update(extra)
        result = subprocess.run(['/bin/sh', str(self.helper), action, sha(self.original), sha(self.candidate)],
                                capture_output=True, timeout=10, env=env)
        self.assertEqual(result.returncode == 0, success, repr(result.stdout) + repr(result.stderr))
        return result

    def test_install_verify_repeat_and_usb_only_rollback_copy(self):
        self.run_helper()
        self.assertEqual(self.target.read_bytes(), self.candidate)
        self.assertIn(' ro,', self.mounts.read_text())
        for backup in self.backups:
            self.assertEqual(backup.read_bytes(), self.original)
        self.assertFalse((self.root / 'usr/bin/d261').exists())  # No factory learning backup created.
        self.run_helper('verify')
        before = (self.root / 'mount-events').read_bytes()
        self.run_helper()
        self.assertEqual((self.root / 'mount-events').read_bytes(), before)
        self.assertEqual(self.target.read_bytes(), self.candidate)

    def test_candidate_hash_failure_does_not_modify_startup(self):
        (self.root / 'dev/shm/c1-S90usb.open-root-adb').write_bytes(b'bad')
        self.run_helper(success=False)
        self.assertEqual(self.target.read_bytes(), self.original)
        self.assertFalse((self.root / 'mount-events').exists())
        self.assertFalse(any(p.exists() for p in self.backups))

    def test_unknown_startup_is_preserved(self):
        self.target.write_bytes(b'unknown startup')
        self.run_helper(success=False)
        self.assertEqual(self.target.read_bytes(), b'unknown startup')
        self.assertFalse((self.root / 'mount-events').exists())

    def test_existing_adb_authentication_key_is_preserved(self):
        key = self.root / 'adb_keys'
        key.write_bytes(b'test public key')
        self.run_helper(success=False)
        self.assertEqual(key.read_bytes(), b'test public key')
        self.assertEqual(self.target.read_bytes(), self.original)
        self.assertFalse((self.root / 'mount-events').exists())

    def test_remount_rw_failure_never_replaces_startup(self):
        self.run_helper(success=False, TEST_FAIL_RW='1')
        self.assertEqual(self.target.read_bytes(), self.original)
        self.assertIn(' ro,', self.mounts.read_text())

    def test_remount_ro_failure_reports_failure_and_rolls_back_startup(self):
        self.run_helper(success=False, TEST_FAIL_RO='1')
        self.assertEqual(self.target.read_bytes(), self.original)
        self.assertIn(' rw,', self.mounts.read_text())  # Failure remains explicit; not claimed safe.

    def test_corrupt_existing_usb_backup_blocks_retry(self):
        self.run_helper()
        self.backups[1].write_bytes(b'corrupt')
        before = (self.root / 'mount-events').read_bytes()
        self.run_helper(success=False)
        self.assertEqual(self.target.read_bytes(), self.candidate)
        self.assertEqual((self.root / 'mount-events').read_bytes(), before)


if __name__ == '__main__':
    unittest.main(verbosity=2)
