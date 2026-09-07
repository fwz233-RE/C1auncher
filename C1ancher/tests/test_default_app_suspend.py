"""Host-only suspend preference tests; no ADB or production paths are used."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'scripts/device-default-app.sh'
HOST = ROOT / 'scripts/install-default-app.ps1'


def function(text, name):
    return re.search(r'^' + name + r'\(\) \{\n.*?^\}', text, re.M | re.S).group()


@unittest.skipUnless(os.name == 'posix', 'Requires Linux/WSL /bin/sh')
class DefaultAppSuspendTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='c1-default-suspend-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / 'usr/data/c1'
        self.data.mkdir(parents=True)
        self.marker = self.data / 'disable-auto-suspend'
        self.enabled = self.data / 'enabled'
        self.enrolled = self.data / 'update/enrolled.v1'
        text = SOURCE.read_text()
        names = ('validate_auto_suspend_path', 'validate_auto_suspend_request',
                 'configure_auto_suspend', 'verify_auto_suspend')
        self.functions = '\n'.join(function(text, name) for name in names)
        self.functions = self.functions.replace('/usr/data/c1', str(self.data))
        self.prefix = ('set -eu\numask 077\n'
                       f'auto_suspend_disabled={shlex.quote(str(self.marker))}\n'
                       f'enabled={shlex.quote(str(self.enabled))}\n'
                       f'core_enrolled={shlex.quote(str(self.enrolled))}\n'
                       'sync() { :; }\n'
                       'auto_suspend_supported() { [ "$SUPPORTED" = 1 ]; }\n')

    def run_policy(self, mode='default', supported=True, verify=False, ok=True):
        command = 'verify_auto_suspend' if verify else 'configure_auto_suspend'
        result = subprocess.run(['/bin/sh', '-c', self.prefix + self.functions +
                                 f'\n{command} {shlex.quote(mode)}\n'],
                                env=dict(os.environ, SUPPORTED=str(int(supported))),
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def test_supported_new_install_enables_without_probe(self):
        self.assertIn('automatic_suspend=enabled', self.run_policy().stdout)
        self.assertFalse(self.marker.exists())
        self.run_policy(verify=True)
        self.assertFalse((self.data / 'suspend-probe-passed').exists())

    def test_unsupported_new_install_and_update_fail_closed(self):
        for installed in (False, True):
            with self.subTest(installed=installed):
                if installed:
                    self.enabled.touch()
                self.run_policy(supported=False, verify=True, ok=False)
                self.assertFalse(self.marker.exists())  # Verify cannot mutate.
                self.run_policy(supported=False)
                self.assertEqual(self.marker.read_bytes(), b'')
                self.assertEqual(self.marker.stat().st_mode & 0o777, 0o600)
                self.run_policy(supported=False, verify=True)
                self.marker.unlink()

    def test_existing_disable_contents_and_metadata_preserved(self):
        for contents in (b'', b'C1SETUP 1\n', b'user disabled\x00\n'):
            for supported in (False, True):
                with self.subTest(contents=contents, supported=supported):
                    self.marker.write_bytes(contents)
                    self.marker.chmod(0o640)
                    before = self.marker.stat()
                    self.run_policy(supported=supported)
                    self.run_policy('disabled', supported=supported)
                    self.run_policy(verify=True, supported=supported)
                    after = self.marker.stat()
                    self.assertEqual(self.marker.read_bytes(), contents)
                    self.assertEqual((after.st_ino, after.st_mode, after.st_mtime_ns),
                                     (before.st_ino, before.st_mode, before.st_mtime_ns))

    def test_supported_installed_preferences_preserved(self):
        for path in (self.enabled, self.enrolled):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
            self.assertIn('automatic_suspend=preserved', self.run_policy().stdout)
            self.assertFalse(self.marker.exists())
            path.unlink()

    def test_explicit_enable_requires_capability_and_overrides_disable(self):
        self.marker.write_bytes(b'disabled by user\n')
        self.run_policy('enabled', supported=False, ok=False)
        self.assertEqual(self.marker.read_bytes(), b'disabled by user\n')
        self.run_policy('enabled')
        self.assertFalse(self.marker.exists())
        self.run_policy('enabled', verify=True)
        self.run_policy('disabled')
        self.assertTrue(self.marker.is_file())

    def test_unsafe_marker_and_invalid_modes_rejected(self):
        outside = self.root / 'outside'
        outside.write_bytes(b'keep')
        for kind in ('symlink', 'dangling', 'directory', 'hardlink', 'fifo'):
            with self.subTest(kind=kind):
                if kind == 'symlink': self.marker.symlink_to(outside)
                elif kind == 'dangling': self.marker.symlink_to(self.root / 'missing')
                elif kind == 'directory': self.marker.mkdir()
                elif kind == 'hardlink': os.link(outside, self.marker)
                else: os.mkfifo(self.marker)
                self.run_policy(ok=False)
                self.run_policy(verify=True, ok=False)
                if kind == 'directory': self.marker.rmdir()
                else: self.marker.unlink()
                self.assertEqual(outside.read_bytes(), b'keep')
        self.run_policy('invalid', ok=False)
        self.assertFalse(self.marker.exists())

    def test_capability_predicate_matches_both_installers_and_host(self):
        predicate = function(SOURCE.read_text(), 'auto_suspend_supported')
        self.assertEqual(predicate, function((ROOT / 'installer/device-setup.sh').read_text(),
                                             'auto_suspend_supported'))
        self.assertEqual(predicate, function(HOST.read_text(), 'auto_suspend_supported'))
        subprocess.run(['/bin/sh', '-n', str(SOURCE)], check=True)

    def test_cli_default_mode_and_host_switch_wiring(self):
        text = SOURCE.read_text()
        # Execute the actual argument parser with harmless dispatch stubs only.
        parser = text.split('action=${1:-}', 1)[1]
        stubs = ('install_default_app() { eval \'echo "mode=${13}"\'; }\n'
                 'verify_default_app() { eval \'echo "mode=${12}"\'; }\n')
        for mode in (None, 'default', 'enabled', 'disabled', 'invalid'):
            args = ['install'] + ['0' * 64] * 12 + ([] if mode is None else [mode])
            result = subprocess.run(['/bin/sh', '-c', stubs + 'action=${1:-}' + parser,
                                     'fixture'] + args, capture_output=True, text=True)
            self.assertEqual(result.returncode, 64 if mode == 'invalid' else 0, result.stderr)
            if mode != 'invalid': self.assertIn('mode=' + (mode or 'default'), result.stdout)
        host = HOST.read_text()
        assignments = re.findall(r'^\s*\$autoSuspendMode = (.*)$', host, re.M)
        self.assertEqual(len(assignments), 1)
        self.assertIn("elseif ($DisableAutoSuspend) { 'disabled' } else { 'default' }", assignments[0])
        self.assertNotIn('suspend-probe-passed', host)
        self.assertNotIn('disabled_by_default', host)
        self.assertIn('$expectedPreviousShimHash $autoSuspendMode"', host)


if __name__ == '__main__':
    unittest.main(verbosity=2)
