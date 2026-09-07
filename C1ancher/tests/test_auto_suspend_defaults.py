"""Private host fixtures for installer suspend defaults; no ADB or device writes.

Runs the production shell functions extracted into temporary, path-rewritten
scripts. Permission cases drop root privileges rather than faking access checks.
PowerShell option tests execute only the in-memory parameter/guard prefix, never
an installer body. No production binaries, keys, or release outputs are used.
"""
from pathlib import Path
import base64
import gzip
import os
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]
LEGACY = PROJECT / 'scripts/device-default-app.sh'
EXE = PROJECT / 'installer/device-setup.sh'
HOST = PROJECT / 'scripts/install-default-app.ps1'


def function(text, name):
    start = name + '() {\n'
    return start + text.split(start, 1)[1].split('\n}\n', 1)[0] + '\n}\n'


@unittest.skipUnless(os.name == 'posix', 'Requires Linux/WSL private fixtures')
class AutoSuspendDefaultsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='c1-suspend-defaults-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.root.chmod(0o755)
        self.config = self.root / 'usr/data/c1'
        self.config.mkdir(parents=True)
        self.marker = self.config / 'disable-auto-suspend'
        self.enabled = self.config / 'enabled'
        self.enrolled = self.config / 'update/enrolled.v1'
        self.cpu = self.root / 'proc/cpuinfo'
        self.cpu.parent.mkdir()
        self.cpu.write_text('processor : 0\nmachine\t\t: ingenic,halley6_v20\n')
        self.battery = self.root / 'sys/devices/platform/mpenbatt'
        self.battery.mkdir(parents=True)
        self.wake = self.root / 'sys/devices/platform/gpio_keys/power/wakeup'
        self.wake.parent.mkdir(parents=True)
        self.wake.write_text('enabled\n')
        self.state = self.root / 'sys/power/state'
        self.state.parent.mkdir(parents=True)
        self.state.write_text('freeze standby mem\n')
        self.state.chmod(0o666)  # Allows a dropped-privilege positive control.
        self.legacy = LEGACY.read_text()
        functions = ''.join(function(self.legacy, name) for name in (
            'auto_suspend_supported', 'validate_auto_suspend_path',
            'validate_auto_suspend_request', 'configure_auto_suspend', 'verify_auto_suspend'))
        self.functions = self.rewrite(functions)

    def rewrite(self, text):
        for path in ('/usr/data', '/proc', '/sys'):
            text = text.replace(path, str(self.root) + path)
        return text

    def shell(self, command, *, ok=True, unprivileged=False, functions=None):
        script = ('set -eu\nsync() { :; }\n'
                  f'auto_suspend_disabled={self.marker}\nenabled={self.enabled}\n'
                  f'core_enrolled={self.enrolled}\n' +
                  (self.functions if functions is None else functions) + command + '\n')
        kwargs = {'user': 65534, 'group': 65534, 'extra_groups': []} if unprivileged and os.geteuid() == 0 else {}
        result = subprocess.run(['/bin/sh', '-c', script], capture_output=True,
                                text=True, timeout=10, **kwargs)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def test_three_installation_predicates_are_identical(self):
        expected = function(self.legacy, 'auto_suspend_supported')
        self.assertEqual(function(EXE.read_text(), 'auto_suspend_supported'), expected)
        self.assertEqual(function(HOST.read_text(), 'auto_suspend_supported'), expected)
        self.assertNotIn('suspend-probe-passed', self.legacy)
        self.assertNotIn('suspend-probe-passed', HOST.read_text())

    def test_fresh_supported_default_and_verification_without_proof(self):
        self.shell('configure_auto_suspend default; verify_auto_suspend default; verify_auto_suspend enabled')
        self.assertFalse(self.marker.exists())
        self.assertFalse((self.config / 'suspend-probe-passed').exists())
        self.shell('configure_auto_suspend default')
        self.assertFalse(self.marker.exists())

    def test_hardware_failures_disable_fresh_and_reject_explicit_enable(self):
        failures = (
            (self.cpu, 'machine : other\n'),
            (self.cpu, 'machine : ingenic,halley6_v20-extra\n'),
            (self.cpu, 'machine : ingenic,halley6_v20:extra\n'),
            (self.cpu, 'machine : ingenic,halley6_v20\n' * 2),
            (self.cpu, 'processor : 0\n'),
            (self.cpu, None), (self.wake, None),
            (self.wake, 'disabled\n'), (self.wake, 'enabled extra\n'),
            (self.state, None), (self.state, 'freeze standby memory\n'))
        proof = self.config / 'suspend-probe-passed'
        proof.write_bytes(b'result=passed\n')
        for path, content in failures:
            with self.subTest(path=path, content=content):
                original = path.read_bytes()
                if content is None: path.unlink()
                else: path.write_text(content)
                self.shell('verify_auto_suspend default', ok=False)
                self.assertFalse(self.marker.exists())  # Verify never initializes.
                self.shell('configure_auto_suspend default; verify_auto_suspend default; verify_auto_suspend disabled')
                self.assertEqual(self.marker.read_bytes(), b'')
                self.assertEqual(self.marker.stat().st_mode & 0o777, 0o600)
                self.shell('configure_auto_suspend enabled', ok=False)
                self.shell('verify_auto_suspend enabled', ok=False)
                self.assertEqual(self.marker.read_bytes(), b'')
                self.assertEqual(proof.read_bytes(), b'result=passed\n')
                self.marker.unlink()
                path.write_bytes(original)
        self.battery.rmdir()
        self.shell('configure_auto_suspend default; verify_auto_suspend disabled')
        self.shell('configure_auto_suspend enabled', ok=False)

    def test_real_read_and_write_permissions_fail_closed(self):
        self.shell('auto_suspend_supported', unprivileged=True)
        for path, mode in ((self.cpu, 0), (self.wake, 0),
                           (self.state, 0o444), (self.state, 0o222)):
            with self.subTest(path=path, mode=mode):
                original = path.stat().st_mode & 0o777
                path.chmod(mode)
                try:
                    self.shell('auto_suspend_supported', ok=False, unprivileged=True)
                finally:
                    path.chmod(original)

    def test_all_existing_markers_preserved_until_explicit_enable(self):
        for contents in (b'', b'C1SETUP 1\n', b'explicit\x00arbitrary\n'):
            with self.subTest(contents=contents):
                self.marker.write_bytes(contents)
                inode = self.marker.stat().st_ino
                self.shell('configure_auto_suspend default; verify_auto_suspend default; configure_auto_suspend disabled')
                self.assertEqual(self.marker.read_bytes(), contents)
                self.assertEqual(self.marker.stat().st_ino, inode)
                self.shell('verify_auto_suspend enabled', ok=False)
                self.shell('configure_auto_suspend enabled; verify_auto_suspend enabled')
                self.assertFalse(self.marker.exists())

    def test_upgrade_preserves_preferences_on_supported_hardware(self):
        for installed in (self.enabled, self.enrolled):
            with self.subTest(installed=installed):
                installed.parent.mkdir(parents=True, exist_ok=True)
                installed.write_text('existing\n')
                self.shell('configure_auto_suspend default; verify_auto_suspend default')
                self.assertFalse(self.marker.exists())
                self.marker.write_bytes(b'C1SETUP 1\n')
                self.shell('configure_auto_suspend default; verify_auto_suspend default')
                self.assertEqual(self.marker.read_bytes(), b'C1SETUP 1\n')
                self.marker.unlink()
                installed.unlink()

    def test_unsupported_upgrade_and_retry_fail_closed(self):
        self.wake.write_text('disabled\n')
        for installed in (self.enabled, self.enrolled):
            with self.subTest(installed=installed):
                installed.parent.mkdir(parents=True, exist_ok=True)
                installed.write_text('existing\n')
                self.shell('verify_auto_suspend default', ok=False)
                self.assertFalse(self.marker.exists())
                self.shell('configure_auto_suspend default; verify_auto_suspend default')
                self.assertEqual(self.marker.read_bytes(), b'')
                self.shell('configure_auto_suspend default')
                self.assertEqual(self.marker.read_bytes(), b'')
                self.shell('verify_auto_suspend enabled', ok=False)
                self.marker.unlink()
                installed.unlink()

    def test_explicit_disable_then_default_preserves_it(self):
        self.shell('configure_auto_suspend disabled; verify_auto_suspend disabled; configure_auto_suspend default')
        self.assertTrue(self.marker.is_file())
        self.shell('verify_auto_suspend enabled', ok=False)

    def test_unsafe_markers_rejected_without_target_changes(self):
        target = self.root / 'outside'
        target.write_bytes(b'unchanged')
        for kind in ('symlink', 'dangling', 'directory', 'hardlink', 'fifo'):
            with self.subTest(kind=kind):
                if kind == 'symlink': self.marker.symlink_to(target)
                elif kind == 'dangling': self.marker.symlink_to(self.root / 'absent')
                elif kind == 'directory': self.marker.mkdir()
                elif kind == 'hardlink': os.link(target, self.marker)
                else: os.mkfifo(self.marker)
                for mode in ('default', 'enabled', 'disabled'):
                    self.shell('configure_auto_suspend ' + mode, ok=False)
                    self.shell('verify_auto_suspend ' + mode, ok=False)
                if kind == 'directory': self.marker.rmdir()
                else: self.marker.unlink()
                self.assertEqual(target.read_bytes(), b'unchanged')
        moved = self.root / 'moved-config'
        self.config.rename(moved)
        self.config.symlink_to(moved, target_is_directory=True)
        self.shell('configure_auto_suspend enabled', ok=False)
        self.assertEqual(list(moved.iterdir()), [])

    def test_explicit_enable_rejected_before_install_side_effects(self):
        self.wake.write_text('disabled\n')
        install = self.rewrite(function(self.legacy, 'install_default_app'))
        command = ('hash_file() { echo UNEXPECTED >&2; exit 99; }\n' + install +
                   'install_default_app ' + 'hash ' * 12 + 'enabled')
        result = self.shell(command, ok=False)
        self.assertNotIn('UNEXPECTED', result.stderr)
        self.assertFalse(self.marker.exists())

    def test_legacy_argument_default_and_validation(self):
        text = self.legacy
        self.assertIn('auto_suspend_mode=${14:-default}', text)
        self.assertIn('case "$auto_suspend_mode" in default|enabled|disabled)', text)
        self.shell('configure_auto_suspend invalid', ok=False)
        self.shell('verify_auto_suspend invalid', ok=False)
        self.assertFalse(self.marker.exists())

    def test_legacy_dispatch_passes_default_and_explicit_modes(self):
        # Exercise real argument parsing and dispatch with inert function doubles.
        dispatch = 'action=${1:-}' + self.legacy.split('action=${1:-}', 1)[1].split('\ntrap - 0 1 2 15', 1)[0]
        stubs = ('install_default_app() { printf "install:%s\\n" "${13}"; }\n'
                 'verify_default_app() { printf "verify:%s\\n" "${12}"; }\n'
                 'remove_original_software() { echo remove-original; }\n')
        for action in ('install', 'verify'):
            for option, expected in (('', 'default'), ('default', 'default'),
                                     ('enabled', 'enabled'), ('disabled', 'disabled')):
                with self.subTest(action=action, option=option):
                    command = 'set -- ' + action + ' ' + 'hash ' * 12 + option + '\n' + stubs + dispatch
                    result = self.shell(command)
                    self.assertEqual(result.stdout.strip(), action + ':' + expected)
        self.assertFalse(self.marker.exists())

    def test_powershell_option_guards_and_mode_routing_in_memory(self):
        powershell = shutil.which('powershell.exe') or shutil.which('pwsh')
        if powershell is None:
            candidate = Path('/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe')
            if candidate.is_file(): powershell = str(candidate)
        if powershell is None:
            self.skipTest('Requires host PowerShell for isolated parameter-prefix checks')
        # Execute only parameters and guards, stopping before paths, functions,
        # uploads, evidence creation or Resolve-Adb can enter the test program.
        prefix = HOST.read_text().split('$projectRoot =', 1)[0]
        self.assertNotIn('Invoke-', prefix)
        self.assertNotIn('New-Item', prefix)
        for args, expected in (('', 'default'), ('-EnableAutoSuspend', 'enabled'),
                               ('-DisableAutoSuspend', 'disabled'),
                               ('-Action Verify', 'default'),
                               ('-Action Verify -EnableAutoSuspend', 'enabled'),
                               ('-Action Verify -DisableAutoSuspend', 'disabled'),
                               ('-EnableAutoSuspend -DisableAutoSuspend', None),
                               ('-Action RemoveOriginal -EnableAutoSuspend', None),
                               ('-Action RemoveOriginal -DisableAutoSuspend', None)):
            with self.subTest(args=args):
                script = 'try { & {\n' + prefix + '\nWrite-Output $autoSuspendMode\n} ' + args + '\n} catch { Write-Output $_.Exception.Message; exit 17 }'
                encoded = base64.b64encode(script.encode('utf-16le')).decode('ascii')
                result = subprocess.run([powershell, '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded],
                                        capture_output=True, text=True, errors='replace', timeout=20)
                if expected is None:
                    self.assertEqual(result.returncode, 17, result.stdout + result.stderr)
                else:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(result.stdout.strip(), expected)
        text = HOST.read_text()
        self.assertEqual(text.count('$autoSuspendMode ='), 1)
        self.assertIn('if ($EnableAutoSuspend) { Assert-SuspendCapability $suspendCapability }', text)
        # Parse the full source without executing it. Compress only to stay below
        # the Windows command-line limit when passing UTF-16 encoded commands.
        compressed = base64.b64encode(gzip.compress(text.encode('utf-8'))).decode('ascii')
        script = ("$bytes=[Convert]::FromBase64String('" + compressed + "'); "
                  '$memory=[IO.MemoryStream]::new($bytes); '
                  '$zip=[IO.Compression.GzipStream]::new($memory,[IO.Compression.CompressionMode]::Decompress); '
                  '$reader=[IO.StreamReader]::new($zip); $tokens=$null; $parseErrors=$null; '
                  '[void][Management.Automation.Language.Parser]::ParseInput($reader.ReadToEnd(),[ref]$tokens,[ref]$parseErrors); '
                  'if ($parseErrors.Count) { $parseErrors | ForEach-Object { Write-Output $_.Message }; exit 1 }')
        encoded = base64.b64encode(script.encode('utf-16le')).decode('ascii')
        result = subprocess.run([powershell, '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded],
                                capture_output=True, text=True, errors='replace', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
