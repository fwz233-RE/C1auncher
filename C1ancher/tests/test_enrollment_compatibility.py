"""Native compatibility checks inside a PRIVATE chroot. No ADB or device writes.
The static driver links unchanged production C; only its filesystem is isolated.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]


class NativeEnrollmentCompatibilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.geteuid() != 0:
            raise RuntimeError('Use WSL root for private chroot, never host /etc')
        cls.build = tempfile.TemporaryDirectory(prefix='c1-compat-build-')
        cls.addClassCleanup(cls.build.cleanup)
        cls.verifier = Path(cls.build.name) / 'verify'
        sources = ['tests/installer_compatibility_verifier.c', 'src/update/protocol.c',
                   'src/security/secure_file.c', 'src/security/sha256.c']
        subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                        '-static', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-I' + str(PROJECT / 'src'), *[str(PROJECT / p) for p in sources],
                        '-o', str(cls.verifier)], check=True, capture_output=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='c1-compat-chroot-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.slots = self.root / 'etc/c1updater'
        self.slots.mkdir(parents=True, mode=0o700)
        self.script = self.root / 'etc/app_daemon'
        self.script.write_bytes((PROJECT.parent / 'firmware-analysis/system-rootfs/etc/app_daemon').read_bytes())
        self.script.chmod(0o755)
        self.record = self.slots / 'bootstrap.version'
        shutil.copyfile(self.verifier, self.root / 'verify')
        (self.root / 'verify').chmod(0o700)

    def check(self, success, bootstrap='1.1.0', updater='1.1.0'):
        result = subprocess.run(['chroot', str(self.root), '/verify', bootstrap, updater],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return result

    def install_bootstrap(self):
        self.script.write_bytes((PROJECT / 'scripts/app-daemon-bootstrap.sh').read_bytes())
        self.script.chmod(0o755)
        self.record.write_text('1.1.0 ' + hashlib.sha256(self.script.read_bytes()).hexdigest() + '\n')
        self.record.chmod(0o600)

    def test_observed_failure_then_genuine_bootstrap_binding_passes(self):
        result = self.check(False)
        self.assertIn('release requires newer trusted bootstrap/updater', result.stderr)
        self.install_bootstrap()
        self.check(True)

    def test_prospective_hash_without_installing_bootstrap_is_rejected(self):
        old = self.script.read_bytes()
        self.install_bootstrap()
        self.script.write_bytes(old)
        self.check(False)

    def test_modified_real_bootstrap_is_rejected(self):
        self.install_bootstrap()
        self.script.write_bytes(self.script.read_bytes() + b'\n# changed\n')
        self.check(False)

    def test_minimum_versions_still_enforced(self):
        self.install_bootstrap()
        self.check(False, bootstrap='9.0.0')
        self.check(False, updater='9.0.0')

    def test_record_metadata_and_links_rejected(self):
        self.install_bootstrap()
        self.record.chmod(0o666)
        self.check(False)
        self.record.chmod(0o600)
        os.link(self.record, self.slots / 'alias')
        self.check(False)
        (self.slots / 'alias').unlink()
        other = self.slots / 'real'
        self.record.rename(other)
        self.record.symlink_to('real')
        self.check(False)

    def test_bootstrap_ownership_and_writable_mode_rejected(self):
        self.install_bootstrap()
        self.script.chmod(0o777)
        self.check(False)
        self.script.chmod(0o755)
        os.chown(self.script, 65534, 65534)
        self.check(False)

    def test_legacy_contract_still_works_without_capability(self):
        self.check(True, bootstrap='1.0.0', updater='1.0.0')


if __name__ == '__main__':
    unittest.main(verbosity=2)
