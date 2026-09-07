"""Host-only device helper tests. Run with Linux/WSL Python and /bin/sh.

All production writable paths are rewritten in a PRIVATE temporary script copy;
mounts, identity, chown, sync, process stops/starts and disk space are stubbed.
No ADB, real mounts, device deployment or production test bypass is used.
"""
from pathlib import Path
import hashlib
import os
import shutil
import socket
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'installer/device-setup.sh'
FIRMWARE = ROOT.parent / 'firmware-analysis/system-rootfs'
INIT_HASH = 'd35cdaa670636511c04d70b5e21b61e1938d2e93b33ae9379a01df1f340cafc2'
DAEMON_HASH = 'ceb56ddf2ff3c10f7c4c2cd6216b298da1cea799ca7170322f8229d5e9af6ee7'
FLAG = 'C1SETUP 1\n'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


@unittest.skipUnless(os.name == 'posix', 'Run under WSL or Linux; no Windows system paths are touched')
class InstallerDeviceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='c1-installer-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / 'usr/data'
        self.storage = self.root / 'storage'
        self.factory = self.root / 'usr/bin/d261'
        self.init = self.root / 'etc/init.d/S80app'
        self.daemon = self.root / 'etc/app_daemon'
        self.state = self.data / 'c1/installer'
        self.backup_data = self.data / 'c1/recovery/installer-factory-v1'
        self.backup_storage = self.storage / 'c1/recovery/installer-factory-v1'
        self.events = self.root / 'events'
        for path in (self.data, self.storage, self.factory, self.init.parent,
                     self.root / 'proc', self.root / 'etc/profile.d'):
            path.mkdir(parents=True, exist_ok=True)
        # Read the reviewed firmware baselines; execute only private fixtures.
        self.init.write_bytes((FIRMWARE / 'etc/init.d/S80app').read_bytes())
        self.daemon.write_bytes((FIRMWARE / 'etc/app_daemon').read_bytes())
        self.init.chmod(0o755)
        self.daemon.chmod(0o755)
        (self.factory / 'mpenMain').write_bytes(b'factory ELF fixture\x00\xff')
        (self.factory / 'mpenMain').chmod(0o755)
        (self.factory / 'empty').mkdir()
        (self.factory / 'assets').mkdir()
        (self.factory / 'assets/lesson.dat').write_bytes(b'factory packaged lessons')
        (self.root / 'etc/profile').write_text('export PATH="/bin:/sbin:/usr/bin:/usr/sbin"\nfor i in /etc/profile.d/*.sh ; do\n    [ ! -r "$i" ] || . $i\ndone\n'.replace('/etc', str(self.root / 'etc')))
        self.user_file = self.storage / 'mtp/Books/user.txt'
        self.user_file.parent.mkdir(parents=True)
        self.user_file.write_text('precious user book')
        self.app_file = self.data / 'apps/example/user.db'
        self.app_file.parent.mkdir(parents=True)
        self.app_file.write_text('ordinary app data')
        self.mounts()
        self.env = dict(os.environ, FIXTURE=str(self.root))
        text = SOURCE.read_text()
        # Rewrite longer tokens first, including paths in generated init/profile.
        for token in ('/usr/bin/d261', '/usr/data', '/storage', '/etc', '/proc', '/sys'):
            text = text.replace(token, str(self.root) + token)
        text = text.replace('ROOT_MOUNT=/\n', f'ROOT_MOUNT={self.root}\n')
        text = text.replace('auto_suspend_supported() {', 'fixture_auto_suspend_supported() {')
        overrides = r'''
# Test-only overrides, absent from the installed source.
id() { if [ "$1" = -u ]; then echo "${TEST_UID:-0}"; else command id "$@"; fi; }
chown() { :; }
sync() {
    if [ "${TEST_TRACE_SYNC:-0}" = 1 ]; then echo sync >>"$FIXTURE/events"; fi
    [ "${TEST_FAIL_SYNC:-0}" != 1 ]
}
auto_suspend_supported() {
    case "${TEST_SUSPEND_SUPPORTED:-fixture}" in
        1) return 0 ;; 0) return 1 ;; *) fixture_auto_suspend_supported ;;
    esac
}
sleep() { :; } # Readiness polling uses virtual time in the private fixture.
free_kb() {
    if [ "$1" = "$DATA" ]; then echo "${TEST_DATA_KB:-1048576}";
    elif [ "$1" = "$STORAGE" ]; then echo "${TEST_STORAGE_KB:-1048576}";
    else echo 1048576; fi
}
remount() {
    echo "mount:$1" >>"$FIXTURE/events"
    if [ "${TEST_FAIL_REMOUNT:-}" = "$1" ]; then return 1; fi
    printf 'root %s ext4 %s 0 0\nstorage %s vfat rw 0 0\ndata %s ext4 rw 0 0\n' "$ROOT_MOUNT" "$1" "$STORAGE" "$DATA" >"$MOUNTS"
}
bootstrap_pids() { [ ! -e "$FIXTURE/running" ] || echo 999999; }
stop_chain() {
    echo stop >>"$FIXTURE/events"; chain_stopped=1; command rm -f "$FIXTURE/running"
    case "${TEST_STOP_CHANGE:-}" in
        tree) ln -s "$FIXTURE/storage/mtp/Books/user.txt" "$FACTORY/late-link" ;;
        init) printf 'tampered\n' >"$INIT" ;;
        core) printf 'tampered\n' >"$DATA/c1/core/core.fixture" ;;
        mount) printf 'evil %s/assets ext4 rw 0 0\n' "$FACTORY" >>"$MOUNTS" ;;
    esac
}
start_chain() {
    root_ro || return 1
    echo start >>"$FIXTURE/events"
    if [ "${TEST_FAIL_START:-0}" = 1 ]; then return 1; fi
    : >"$FIXTURE/running"
    chain_stopped=0
}
rm() {
    if [ "${TEST_PARTIAL_DELETE:-0}" = 1 ] && [ "$*" = "-rf $FACTORY" ]; then
        command rm "$FACTORY/mpenMain"
        return 1
    fi
    command rm "$@"
}
'''
        # Remove the actual remount/start commands as a second isolation barrier.
        text = text.replace('/bin/mount -o "remount,$1" "$ROOT_MOUNT"', 'false')
        text = text.replace('/bin/busybox start-stop-daemon -S -b -x', 'false')
        text = text.replace('action=${1:-}', overrides + '\naction=${1:-}')
        self.script = self.root / 'helper.sh'
        self.script.write_text(text)
        self.assertNotIn('/bin/mount ', text)
        self.assertNotIn('/bin/busybox ', text)

    def mounts(self, root='ro', storage='rw', data='rw', extra=''):
        (self.root / 'proc/mounts').write_text(
            f'root {self.root} ext4 {root} 0 0\n'
            f'storage {self.storage} vfat {storage} 0 0\n'
            f'data {self.data} ext4 {data} 0 0\n' + extra)

    def run_helper(self, action, *args, ok=True, error=None, **env):
        absent_backups = [path for path in (self.backup_data.parent, self.backup_storage.parent)
                          if not path.exists()]
        if action in ('enable-suspend', 'verify-suspend') and not args and error != 'usage':
            args = getattr(self, 'suspend_identity', ('0' * 64, '0' * 64))
        result = subprocess.run(['/bin/sh', str(self.script), action, *map(str, args)],
                                env=dict(self.env, **env), capture_output=True,
                                text=True, timeout=25)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn(f'C1SETUP_OK {action}\n', result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertNotIn(f'C1SETUP_OK {action}\n', result.stdout)
            if error:
                self.assertIn(error, result.stderr)
        # Every operation, including failures/retries, must preserve external data
        # and must not create a factory recovery directory on either partition.
        self.assertEqual(self.user_file.read_text(), 'precious user book')
        self.assertEqual(self.app_file.read_text(), 'ordinary app data')
        for path in absent_backups:
            self.assertFalse(path.exists(), str(path))
        return result

    def enroll(self):
        # Stub only the signed verifier's contract. Cryptographic verification is
        # covered by the separate signed-core tests, not by this fixture.
        self.daemon.write_text('#!/bin/sh\n# authenticated core bootstrap fixture\n')
        core = self.data / 'c1/core/core.fixture'
        core.parent.mkdir(parents=True, exist_ok=True)
        core.write_bytes(b'validated signed core fixture')
        self.release = core.parent / 'releases/7-1.2.3+fixture'
        self.pkg = self.release / 'artifacts/c1pkg'
        self.pkg.parent.mkdir(parents=True, exist_ok=True)
        self.pkg.write_text('''#!/bin/sh
[ "$1" = power ] || exit 64
printf 'power:%s\\n' "$2" >>"$FIXTURE/events"
marker=$FIXTURE/usr/data/c1/disable-auto-suspend
case "$2" in
    enable)
        [ "${TEST_POWER_ENABLE_FAIL:-0}" != 1 ] || exit 71
        if [ "${TEST_POWER_NOOP:-0}" != 1 ]; then rm -f "$marker" || exit 72; fi
        echo 'automatic suspend: enabled'
        ;;
    status)
        if [ "${TEST_STATUS_CREATES_MARKER:-0}" = 1 ]; then : >"$marker"; fi
        if [ -n "${TEST_POWER_STATUS_FILE:-}" ]; then
            cat "$TEST_POWER_STATUS_FILE" || exit 74
        elif [ "${TEST_POWER_STATUS+x}" = x ]; then
            printf '%s' "$TEST_POWER_STATUS"
        elif [ -e "$marker" ] || [ -L "$marker" ]; then
            echo 'automatic suspend: disabled'
        else
            echo 'automatic suspend: enabled'
        fi
        [ "${TEST_POWER_STATUS_FAIL:-0}" != 1 ] || exit 73
        ;;
    *) exit 64 ;;
esac
''')
        self.pkg.chmod(0o700)
        manifest = self.release / 'manifest.v1'
        manifest.write_text('C1CORE 1\nS\t7\nV\t1.2.3+fixture\nE\t1\n')
        self.suspend_identity = (sha(manifest), sha(self.pkg))
        (core.parent / 'current').symlink_to('releases/' + self.release.name)
        slot = self.root / 'etc/c1updater'
        slot.mkdir(exist_ok=True)
        verifier = slot / 'recovery-verifier'
        verifier.write_text('#!/bin/sh\ncase "$1" in\n'
                            'verify-current)\n'
                            '    [ "${TEST_INVALID_CORE:-0}" != 1 ] || exit 1\n'
                            '    sum=$(sha256sum "$2/core.fixture") || exit 1\n'
                            f'    [ "${{sum%% *}}" = "{sha(core)}" ] || exit 1\n'
                            '    sum=$(sha256sum "$2/current/artifacts/c1pkg") || exit 1\n'
                            f'    [ "${{sum%% *}}" = "{sha(self.pkg)}" ] ;;\n'
                            'state) echo "state: generation=7 phase=${TEST_CORE_PHASE:-confirmed} sequence=7 security_epoch=1 release=1.2.3+fixture"\n'
                            '    [ "${TEST_CORE_STATE_FAIL:-0}" != 1 ] ;;\n'
                            '*) exit 64 ;;\nesac\n')
        verifier.chmod(0o700)
        key = slot / 'core.ed25519.pub'
        key.write_bytes(bytes(range(32)))
        (slot / 'bootstrap.version').write_text(f'1.1.0 {sha(self.daemon)}\n')
        marker = self.data / 'c1/update/enrolled.v1'
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text(f'C1CORE-ENROLLED 1\nbootstrap_sha256={sha(self.daemon)}\n'
                          f'recovery_verifier_sha256={sha(verifier)}\nkey_sha256={sha(key)}\n')
        (self.root / 'running').touch()

    def payload(self):
        payload = self.root / 'payload'
        payload.mkdir()
        for name in ('neofetch', 'neofetch.upstream', 'c1-config.conf', 'c1-logo.txt', 'LICENSE.md'):
            shutil.copyfile(ROOT / 'third_party/neofetch' / name, payload / name)
        (payload / 'wallpaper.raw').write_bytes(b'\x55' * 5624)
        self.payload_manifest(payload)
        return payload

    def payload_manifest(self, payload):
        names = ('neofetch', 'neofetch.upstream', 'c1-config.conf', 'c1-logo.txt', 'LICENSE.md', 'wallpaper.raw')
        (payload / 'SHA256SUMS').write_text(''.join(f'{sha(payload / name)}  {name}\n' for name in names))

    def event_lines(self):
        return self.events.read_text().splitlines() if self.events.exists() else []

    def assert_root_ro(self):
        self.assertIn(f'root {self.root} ext4 ro 0 0\n', (self.root / 'proc/mounts').read_text())

    def test_start_core_retries_stopped_confirmed_core_without_removal(self):
        self.enroll()
        (self.root / 'running').unlink()
        original_init = self.init.read_bytes()
        original_factory = (self.factory / 'mpenMain').read_bytes()
        marker = self.data / 'c1/update/enrolled.v1'
        original_marker = marker.read_bytes()
        self.run_helper('start-core')
        self.assertTrue((self.root / 'running').exists())
        self.assertEqual(self.init.read_bytes(), original_init)
        self.assertEqual((self.factory / 'mpenMain').read_bytes(), original_factory)
        self.assertEqual(marker.read_bytes(), original_marker)
        self.assertFalse((self.state / 'removing').exists())
        self.assertNotIn('stop', self.event_lines())
        self.assert_root_ro()

    def test_start_core_requires_valid_enrollment_and_reports_start_failure(self):
        self.run_helper('start-core', ok=False)
        self.assertNotIn('start', self.event_lines())
        self.enroll()
        self.run_helper('start-core', ok=False, TEST_INVALID_CORE='1')
        self.assertNotIn('start', self.event_lines())
        (self.root / 'running').unlink()
        self.run_helper('start-core', ok=False, error='bootstrap-start-failed', TEST_FAIL_START='1')
        self.assertTrue(self.factory.exists())
        self.assert_root_ro()

    def test_enable_suspend_clears_valid_preferences_and_is_repeatable(self):
        self.suspend_hardware()
        self.enroll()
        marker = self.data / 'c1/disable-auto-suspend'
        original_init = self.init.read_bytes()
        original_enrollment = (self.data / 'c1/update/enrolled.v1').read_bytes()
        for content in (b'user explicitly disabled\n', b'', FLAG.encode(),
                        b'legacy preference\x00arbitrary\n', None):
            with self.subTest(content=content):
                if content is not None:
                    marker.write_bytes(content)
                self.events.unlink(missing_ok=True)
                self.run_helper('enable-suspend', TEST_TRACE_SYNC='1')
                self.assertFalse(marker.exists())
                self.assertEqual(self.event_lines(), ['power:enable', 'sync', 'power:status'])
                self.run_helper('verify-suspend', TEST_TRACE_SYNC='1')
                self.assertEqual(self.event_lines(), ['power:enable', 'sync', 'power:status', 'power:status'])
                self.assertEqual(self.init.read_bytes(), original_init)
                self.assertEqual((self.data / 'c1/update/enrolled.v1').read_bytes(), original_enrollment)
                self.assertTrue(self.factory.exists())
                self.assertFalse((self.state / 'removing').exists())
                self.assertFalse((self.state / 'prepared').exists())
                self.assert_root_ro()

    def test_suspend_actions_reject_unsupported_hardware_without_command(self):
        self.enroll()
        marker = self.data / 'c1/disable-auto-suspend'
        for failure in ('machine', 'duplicate-machine', 'machine-field', 'battery',
                        'wake-missing', 'wake-disabled', 'state-missing', 'state-no-mem'):
            with self.subTest(failure=failure):
                wake, state = self.suspend_hardware()
                if failure == 'machine':
                    (self.root / 'proc/cpuinfo').write_text('machine : unknown\n')
                elif failure == 'duplicate-machine':
                    (self.root / 'proc/cpuinfo').write_text('machine : ingenic,halley6_v20\n' * 2)
                elif failure == 'machine-field':
                    (self.root / 'proc/cpuinfo').write_text('machine : ingenic,halley6_v20 : extra\n')
                elif failure == 'battery':
                    (self.root / 'sys/devices/platform/mpenbatt').rmdir()
                elif failure == 'wake-missing':
                    wake.unlink()
                elif failure == 'wake-disabled':
                    wake.write_text('disabled\n')
                elif failure == 'state-missing':
                    state.unlink()
                else:
                    state.write_text('freeze standby memory\n')
                for present in (False, True):
                    if present:
                        marker.write_bytes(b'preserved\n')
                    # A lost capability must fail closed, including final verify.
                    self.run_helper('verify-suspend', ok=False, error='automatic-suspend-unsupported-hardware')
                    self.assertTrue(marker.exists())
                    for action in ('enable-suspend', 'verify-suspend'):
                        self.run_helper(action, ok=False, error='automatic-suspend-unsupported-hardware')
                        self.assertTrue(marker.exists())
                        self.assertEqual(marker.read_bytes(), b'preserved\n' if present else FLAG.encode())
                    marker.unlink(missing_ok=True)
        self.assertEqual(self.event_lines(), [])

    def test_unsupported_final_verification_disables_if_capability_is_lost(self):
        self.suspend_hardware()
        self.enroll()
        self.run_helper('enable-suspend')
        marker = self.data / 'c1/disable-auto-suspend'
        self.assertFalse(marker.exists())
        (self.root / 'sys/devices/platform/gpio_keys/power/wakeup').write_text('disabled\n')
        self.events.unlink()
        self.run_helper('verify-suspend', ok=False, error='automatic-suspend-unsupported-hardware')
        self.assertEqual(marker.read_text(), FLAG)
        self.assertEqual(self.event_lines(), [])

    def test_suspend_requires_this_payload_hashes_before_power_commands(self):
        self.suspend_hardware()
        self.enroll()
        marker = self.data / 'c1/disable-auto-suspend'
        marker.write_bytes(FLAG.encode())
        manifest_hash, pkg_hash = self.suspend_identity
        for hashes in (('0' * 64, pkg_hash), (manifest_hash, '0' * 64),
                       ('invalid', pkg_hash), (manifest_hash, 'A' * 64)):
            for action in ('enable-suspend', 'verify-suspend'):
                with self.subTest(action=action, hashes=hashes):
                    self.run_helper(action, *hashes, ok=False)
                    self.assertEqual(marker.read_bytes(), FLAG.encode())
        self.assertEqual(self.event_lines(), [])

    def test_suspend_rejects_idle_and_mismatched_confirmed_identity(self):
        self.suspend_hardware()
        self.enroll()
        marker = self.data / 'c1/disable-auto-suspend'
        marker.write_bytes(FLAG.encode())
        for action in ('enable-suspend', 'verify-suspend'):
            self.run_helper(action, ok=False, error='suspend-core-not-confirmed', TEST_CORE_PHASE='idle')
        manifest = self.release / 'manifest.v1'
        # Even an expected, verified manifest must be the confirmed generation.
        for text in ('C1CORE 1\nS\t8\nV\t1.2.3+fixture\nE\t1\n',
                     'C1CORE 1\nS\t7\nV\tother-version\nE\t1\n',
                     'C1CORE 1\nS\t7\nV\t1.2.3+fixture\nE\t2\n'):
            manifest.write_text(text)
            for action in ('enable-suspend', 'verify-suspend'):
                self.run_helper(action, sha(manifest), self.suspend_identity[1], ok=False,
                                error='suspend-state-identity-mismatch')
        self.assertEqual(marker.read_bytes(), FLAG.encode())
        self.assertEqual(self.event_lines(), [])

    def test_prepare_then_confirmed_completion_alone_overrides_old_disable(self):
        self.suspend_hardware()
        marker = self.data / 'c1/disable-auto-suspend'
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_bytes(FLAG.encode())
        self.run_helper('prepare')
        self.assertEqual(marker.read_bytes(), FLAG.encode())
        self.assertEqual(self.event_lines(), [])
        self.enroll()
        self.run_helper('prepare')
        self.assertEqual(marker.read_bytes(), FLAG.encode())
        self.run_helper('enable-suspend')
        self.run_helper('verify-suspend')
        self.assertFalse(marker.exists())
        self.assertEqual(self.event_lines(), ['power:enable', 'power:status', 'power:status'])

    def test_suspend_actions_require_authenticated_running_core(self):
        self.suspend_hardware()
        for action in ('enable-suspend', 'verify-suspend'):
            self.run_helper(action, ok=False)
        self.enroll()
        marker = self.data / 'c1/disable-auto-suspend'
        marker.write_text('disabled\n')
        for env in ({'TEST_INVALID_CORE': '1'}, {'TEST_CORE_PHASE': 'pending'},
                    {'TEST_CORE_PHASE': 'confirmed-evil'}, {'TEST_CORE_STATE_FAIL': '1'}):
            for action in ('enable-suspend', 'verify-suspend'):
                self.run_helper(action, ok=False, **env)
        for path in (self.pkg, self.daemon, self.root / 'etc/c1updater/recovery-verifier',
                     self.root / 'etc/c1updater/core.ed25519.pub',
                     self.root / 'etc/c1updater/bootstrap.version',
                     self.data / 'c1/update/enrolled.v1'):
            with self.subTest(path=path):
                original = path.read_bytes()
                path.write_bytes(b'#!/bin/sh\nexit 0\n')
                for action in ('enable-suspend', 'verify-suspend'):
                    self.run_helper(action, ok=False)
                path.write_bytes(original)
        (self.root / 'running').unlink()
        for action in ('enable-suspend', 'verify-suspend'):
            self.run_helper(action, ok=False, error='bootstrap-not-running')
        self.assertEqual(marker.read_text(), 'disabled\n')
        self.assertEqual(self.event_lines(), [])

    def test_suspend_rejects_unsafe_release_pointers_and_artifact_paths(self):
        self.suspend_hardware()
        self.enroll()
        current = self.data / 'c1/core/current'
        expected = os.readlink(current)
        # The stub verifier accepts equal fixture bytes even via malicious
        # paths; the helper must independently reject these before execution.
        for target in (str(self.release), 'releases/../releases/' + self.release.name,
                       './' + expected, expected + '/', 'releases/.', 'releases/..'):
            with self.subTest(target=target):
                current.unlink()
                current.symlink_to(target)
                for action in ('enable-suspend', 'verify-suspend'):
                    self.run_helper(action, ok=False)
        current.unlink()
        shutil.copytree(self.release, current)
        self.run_helper('enable-suspend', ok=False, error='enrolled-core-pointer-invalid')
        shutil.rmtree(current)
        current.symlink_to(expected)
        for path in (self.pkg, self.pkg.parent, self.release, self.release.parent):
            with self.subTest(path=path):
                moved = self.root / 'moved-release-part'
                path.rename(moved)
                path.symlink_to(moved)
                for action in ('enable-suspend', 'verify-suspend'):
                    self.run_helper(action, ok=False, error='symlink')
                path.unlink()
                moved.rename(path)
        self.pkg.chmod(0o600)
        self.run_helper('enable-suspend', ok=False, error='enrolled-c1pkg-not-executable')
        self.pkg.chmod(0o700)
        os.link(self.pkg, self.root / 'linked-pkg')
        self.run_helper('enable-suspend', ok=False, error='not-single-link-file')
        self.assertEqual(self.event_lines(), [])

    def test_suspend_actions_reject_dangerous_markers_before_power_commands(self):
        self.suspend_hardware()
        self.enroll()
        marker = self.data / 'c1/disable-auto-suspend'
        outside = self.root / 'outside-preference'
        outside.write_bytes(b'preserve')
        for kind in ('symlink', 'dangling', 'directory', 'hardlink', 'fifo', 'socket'):
            with self.subTest(kind=kind):
                sock = None
                if kind == 'symlink': marker.symlink_to(outside)
                elif kind == 'dangling': marker.symlink_to(self.root / 'missing')
                elif kind == 'directory': marker.mkdir()
                elif kind == 'hardlink': os.link(outside, marker)
                elif kind == 'fifo': os.mkfifo(marker)
                else:
                    sock = socket.socket(socket.AF_UNIX)
                    sock.bind(str(marker))
                try:
                    before = marker.lstat()
                    for action in ('enable-suspend', 'verify-suspend'):
                        self.run_helper(action, ok=False)
                        self.assertEqual(marker.lstat().st_ino, before.st_ino)
                    self.assertEqual(outside.read_bytes(), b'preserve')
                finally:
                    if sock: sock.close()
                    if kind == 'directory': marker.rmdir()
                    else: marker.unlink()
        self.assertEqual(self.event_lines(), [])

    def test_suspend_enable_command_sync_and_false_success_failures(self):
        self.suspend_hardware()
        self.enroll()
        marker = self.data / 'c1/disable-auto-suspend'
        for env, error, removed, events in (
                ({'TEST_POWER_ENABLE_FAIL': '1'}, 'automatic-suspend-enable-failed', False, ['power:enable']),
                ({'TEST_POWER_NOOP': '1'}, 'hash-rejected', False, ['power:enable', 'sync', 'power:status']),
                ({'TEST_POWER_NOOP': '1', 'TEST_POWER_STATUS': 'automatic suspend: enabled\n'},
                 'automatic-suspend-still-disabled', False, ['power:enable', 'sync', 'power:status']),
                ({'TEST_FAIL_SYNC': '1'}, 'automatic-suspend-sync-failed', True, ['power:enable', 'sync']),
                ({'TEST_POWER_STATUS_FAIL': '1'}, 'automatic-suspend-status-failed', True,
                 ['power:enable', 'sync', 'power:status'])):
            with self.subTest(env=env):
                marker.write_text('disabled\n')
                self.events.unlink(missing_ok=True)
                self.run_helper('enable-suspend', ok=False, error=error, TEST_TRACE_SYNC='1', **env)
                self.assertEqual(marker.exists(), not removed)
                if not removed:
                    self.assertEqual(marker.read_text(), 'disabled\n')
                self.assertEqual(self.event_lines(), events)
                self.assert_root_ro()

    def test_suspend_status_requires_exact_complete_enabled_output(self):
        self.suspend_hardware()
        self.enroll()
        for status in ('', 'automatic suspend: disabled\n', 'automatic suspend: enabled',
                       'automatic suspend: enabled\n\n', 'automatic suspend: enabled\r\n',
                       ' automatic suspend: enabled\n', 'automatic suspend: enabled trailing\n',
                       'log\nautomatic suspend: enabled\n',
                       'automatic suspend: enabled\nautomatic suspend: disabled\n'):
            for action in ('enable-suspend', 'verify-suspend'):
                with self.subTest(status=status, action=action):
                    self.run_helper(action, ok=False, error='hash-rejected', TEST_POWER_STATUS=status)
        raw_status = self.root / 'raw-power-status'
        for status in (b'automatic suspend: enabled\x00\n', b'automatic suspend: enabled\n\x00'):
            raw_status.write_bytes(status)
            for action in ('enable-suspend', 'verify-suspend'):
                self.run_helper(action, ok=False, error='hash-rejected', TEST_POWER_STATUS_FILE=str(raw_status))
        self.run_helper('verify-suspend', ok=False, error='automatic-suspend-status-failed',
                        TEST_POWER_STATUS_FAIL='1')
        self.run_helper('verify-suspend')

    def test_verify_suspend_never_repairs_disabled_preference(self):
        self.suspend_hardware()
        self.enroll()
        marker = self.data / 'c1/disable-auto-suspend'
        for content in (b'', FLAG.encode(), b'explicit preference\n'):
            marker.write_bytes(content)
            marker.chmod(0o640)
            before = marker.stat()
            for env in ({}, {'TEST_POWER_STATUS': 'automatic suspend: enabled\n'}):
                self.run_helper('verify-suspend', ok=False, **env)
                after = marker.stat()
                self.assertEqual(marker.read_bytes(), content)
                self.assertEqual((after.st_ino, after.st_mode, after.st_mtime_ns),
                                 (before.st_ino, before.st_mode, before.st_mtime_ns))
        self.assertNotIn('power:enable', self.event_lines())
        marker.unlink()
        self.run_helper('verify-suspend', ok=False, error='automatic-suspend-still-disabled',
                        TEST_POWER_STATUS='automatic suspend: enabled\n', TEST_STATUS_CREATES_MARKER='1')

    def test_full_workflow_suspend_gate_and_final_preference_regression(self):
        self.suspend_hardware()
        marker = self.data / 'c1/disable-auto-suspend'
        marker.parent.mkdir(parents=True)
        marker.write_text(FLAG)
        self.run_helper('prepare')
        self.assertEqual(marker.read_text(), FLAG)
        self.enroll()
        self.run_helper('start-core')
        self.run_helper('enable-suspend')
        self.run_helper('verify-suspend')
        self.run_helper('accessories', self.payload())
        self.run_helper('remove-factory')
        self.run_helper('verify')
        self.run_helper('verify-suspend')
        self.assertFalse(marker.exists())
        marker.write_text('disabled after later configuration or reboot\n')
        self.run_helper('verify-suspend', ok=False)
        self.assertEqual(marker.read_text(), 'disabled after later configuration or reboot\n')
        self.assertEqual(self.event_lines().count('power:enable'), 1)
        self.assert_root_ro()

    def test_reviewed_factory_baselines(self):
        self.assertEqual(sha(self.init), INIT_HASH)
        self.assertEqual(sha(self.daemon), DAEMON_HASH)
        source = SOURCE.read_text()
        self.assertIn(f'FACTORY_INIT_SHA256={INIT_HASH}', source)
        self.assertIn(f'FACTORY_DAEMON_SHA256={DAEMON_HASH}', source)
        self.assertIn('FACTORY=/usr/bin/d261\n', source)
        for forbidden in ('C1_SETUP_ROOT', 'BACKUP_', 'inventory()', 'backup_factory()',
                          'restore_factory()', 'remove-factory-no-backup', 'verify-no-backup'):
            self.assertNotIn(forbidden, source)
        subprocess.run(['/bin/sh', '-n', str(SOURCE)], check=True)

    def test_unknown_operations_and_optional_flags_rejected_before_mutations(self):
        commands = [('unknown',), ('',), ('backup-factory',), ('restore-factory',),
                    ('verify-no-backup',), ('remove-factory-no-backup', 'CONFIRM_NO_FACTORY_BACKUP')]
        commands += [(action, flag) for action in ('preflight', 'prepare', 'start-core', 'enable-suspend', 'verify-suspend', 'remove-factory', 'verify')
                     for flag in ('--no-backup', 'CONFIRM_NO_FACTORY_BACKUP', 'extra')]
        commands += [('accessories',), ('accessories', 'payload', 'extra')]
        for args in commands:
            with self.subTest(args=args):
                self.run_helper(*args, ok=False, error='usage')
                self.assertFalse((self.data / 'c1').exists())
                self.assertFalse((self.storage / 'mtp/Pic').exists())
                self.assertEqual(sha(self.init), INIT_HASH)
                self.assertTrue((self.factory / 'mpenMain').exists())
                self.assertEqual(self.event_lines(), [])

    def test_preflight_and_prepare_safe_default_once(self):
        self.run_helper('preflight')
        self.run_helper('prepare')
        for name in ('Pic', 'Music', 'Book'):
            self.assertTrue((self.storage / 'mtp' / name).is_dir())
        disabled = self.data / 'c1/disable-auto-suspend'
        self.assertTrue(disabled.is_file())
        disabled.unlink()  # User explicitly enabled automatic suspend later.
        self.run_helper('prepare', TEST_SUSPEND_SUPPORTED='1')
        self.assertFalse(disabled.exists())
        self.assertEqual(self.event_lines(), [])

    def suspend_hardware(self):
        (self.root / 'proc/cpuinfo').write_text('processor : 0\nmachine\t\t: ingenic,halley6_v20\n')
        (self.root / 'sys/devices/platform/mpenbatt').mkdir(parents=True, exist_ok=True)
        wake = self.root / 'sys/devices/platform/gpio_keys/power/wakeup'
        wake.parent.mkdir(parents=True, exist_ok=True)
        wake.write_text('enabled\n')
        state = self.root / 'sys/power/state'
        state.parent.mkdir(parents=True, exist_ok=True)
        state.write_text('freeze standby mem\n')
        return wake, state

    def test_fresh_supported_prepare_enables_without_probe_and_repeats(self):
        self.suspend_hardware()
        self.run_helper('prepare')
        marker = self.data / 'c1/disable-auto-suspend'
        self.assertFalse(marker.exists())
        self.assertFalse((self.data / 'c1/suspend-probe-passed').exists())
        self.run_helper('prepare')
        self.assertFalse(marker.exists())
        marker.write_bytes(b'user explicitly disabled\n')
        self.run_helper('prepare')
        self.assertEqual(marker.read_bytes(), b'user explicitly disabled\n')
        self.assertEqual(self.event_lines(), [])

    def test_fresh_unsupported_hardware_disables_even_with_old_proof(self):
        for failure in ('machine', 'duplicate-machine', 'battery', 'wake-missing',
                        'wake-disabled', 'state-missing', 'state-no-mem'):
            with self.subTest(failure=failure):
                wake, state = self.suspend_hardware()
                if failure == 'machine':
                    (self.root / 'proc/cpuinfo').write_text('machine : ingenic,halley6_v20-other\n')
                elif failure == 'duplicate-machine':
                    (self.root / 'proc/cpuinfo').write_text('machine : ingenic,halley6_v20\n' * 2)
                elif failure == 'battery':
                    (self.root / 'sys/devices/platform/mpenbatt').rmdir()
                elif failure == 'wake-missing':
                    wake.unlink()
                elif failure == 'wake-disabled':
                    wake.write_text('disabled\n')
                elif failure == 'state-missing':
                    state.unlink()
                else:
                    state.write_text('freeze standby memory\n')
                (self.data / 'c1').mkdir(exist_ok=True)
                proof = self.data / 'c1/suspend-probe-passed'
                proof.write_bytes(b'result=passed\n')
                self.run_helper('prepare')
                marker = self.data / 'c1/disable-auto-suspend'
                self.assertEqual(marker.read_text(), FLAG)
                self.assertEqual(marker.stat().st_mode & 0o777, 0o600)
                self.assertEqual(proof.read_bytes(), b'result=passed\n')
                self.run_helper('prepare')
                self.assertEqual(marker.read_text(), FLAG)
                marker.unlink()
                (self.state / 'prepared').unlink()

    def test_prepare_preserves_all_existing_disable_marker_contents(self):
        self.suspend_hardware()
        (self.data / 'c1').mkdir(exist_ok=True)
        marker = self.data / 'c1/disable-auto-suspend'
        for content in (b'', FLAG.encode(), b'explicit preference\x00arbitrary\n'):
            with self.subTest(content=content):
                marker.write_bytes(content)
                self.run_helper('prepare')
                self.assertEqual(marker.read_bytes(), content)
                (self.state / 'prepared').unlink()

    def test_enrolled_and_legacy_unsupported_hardware_fails_closed(self):
        for relative in ('c1/update/enrolled.v1', 'c1/enabled'):
            with self.subTest(relative=relative):
                existing = self.data / relative
                existing.parent.mkdir(parents=True, exist_ok=True)
                existing.write_text('existing installation\n')
                self.run_helper('prepare')
                marker = self.data / 'c1/disable-auto-suspend'
                self.assertEqual(marker.read_text(), FLAG)
                marker.unlink()
                existing.unlink()
                (self.state / 'prepared').unlink()

    def test_prepare_rejects_unsafe_disable_markers_on_first_and_repeat(self):
        self.suspend_hardware()
        self.run_helper('prepare')
        marker = self.data / 'c1/disable-auto-suspend'
        outside = self.root / 'outside-preference'
        outside.write_bytes(b'preserve')
        for repeat in (True, False):
            if not repeat:
                (self.state / 'prepared').unlink()
            for kind in ('symlink', 'dangling', 'directory', 'hardlink', 'fifo'):
                with self.subTest(repeat=repeat, kind=kind):
                    if kind == 'symlink': marker.symlink_to(outside)
                    elif kind == 'dangling': marker.symlink_to(self.root / 'missing')
                    elif kind == 'directory': marker.mkdir()
                    elif kind == 'hardlink': os.link(outside, marker)
                    else: os.mkfifo(marker)
                    self.run_helper('prepare', ok=False)
                    if kind == 'directory': marker.rmdir()
                    else: marker.unlink()
                    self.assertEqual(outside.read_bytes(), b'preserve')

    def test_supported_first_install_enables_suspend(self):
        result = self.run_helper('prepare', TEST_SUSPEND_SUPPORTED='1')
        self.assertIn('automatic_suspend=enabled default=supported-hardware', result.stdout)
        self.assertFalse((self.data / 'c1/disable-auto-suspend').exists())
        self.run_helper('prepare', TEST_SUSPEND_SUPPORTED='1')
        self.assertFalse((self.data / 'c1/disable-auto-suspend').exists())

    def test_unsupported_retry_fails_closed(self):
        self.run_helper('prepare', TEST_SUSPEND_SUPPORTED='1')
        self.run_helper('prepare', TEST_SUSPEND_SUPPORTED='0')
        self.assertTrue((self.data / 'c1/disable-auto-suspend').is_file())

    def test_existing_disable_preference_preserved(self):
        marker = self.data / 'c1/disable-auto-suspend'
        marker.parent.mkdir(parents=True, exist_ok=True)
        for contents in ('', FLAG, 'user explicitly disabled\n'):
            for supported in ('0', '1'):
                with self.subTest(contents=contents, supported=supported):
                    marker.write_text(contents)
                    marker.chmod(0o640)
                    before = marker.stat()
                    self.run_helper('prepare', TEST_SUSPEND_SUPPORTED=supported)
                    self.assertEqual(marker.read_text(), contents)
                    self.assertEqual(marker.stat().st_ino, before.st_ino)
                    self.assertEqual(marker.stat().st_mode, before.st_mode)
                    self.assertEqual(marker.stat().st_mtime_ns, before.st_mtime_ns)

    def test_existing_enrollment_preserves_suspend_preference(self):
        self.enroll()
        self.run_helper('prepare', TEST_SUSPEND_SUPPORTED='1')
        self.assertFalse((self.data / 'c1/disable-auto-suspend').exists())
        self.run_helper('prepare', TEST_SUSPEND_SUPPORTED='0')
        self.assertTrue((self.data / 'c1/disable-auto-suspend').is_file())

    def test_nonroot_readwrite_root_and_readonly_data_storage_rejected(self):
        self.run_helper('preflight', ok=False, error='root-required', TEST_UID='1000')
        for setting, error in [({'root': 'rw'}, 'root-must-enter-read-only'),
                               ({'storage': 'ro'}, 'storage-must-be-mounted-rw'),
                               ({'data': 'ro'}, 'data-must-be-mounted-rw')]:
            self.mounts(**setting)
            self.run_helper('preflight', ok=False, error=error)
        self.assertEqual(self.event_lines(), [])

    def test_low_space_rejected_without_mutation(self):
        self.enroll()
        self.run_helper('remove-factory', ok=False, error='insufficient-space', TEST_DATA_KB='65535')
        self.run_helper('remove-factory', ok=False, error='insufficient-space', TEST_STORAGE_KB='1')
        self.assertTrue((self.factory / 'mpenMain').exists())
        self.assertFalse(self.state.exists())
        self.assertEqual(self.event_lines(), [])

    def test_unknown_factory_scripts_rejected(self):
        self.init.write_text('#!/bin/sh\nreboot\n')
        self.run_helper('remove-factory', ok=False, error='unknown-S80app-sha256')
        self.init.write_bytes((FIRMWARE / 'etc/init.d/S80app').read_bytes())
        self.daemon.write_text('#!/bin/sh\nunknown\n')
        self.run_helper('remove-factory', ok=False, error='not-single-link-file')
        self.assertEqual(self.event_lines(), [])

    def test_symlink_ancestor_and_leaf_rejected(self):
        c1 = self.data / 'c1'
        outside = self.root / 'outside'
        outside.mkdir()
        c1.symlink_to(outside, target_is_directory=True)
        self.run_helper('prepare', ok=False, error='symlink')
        c1.unlink()
        self.init.unlink()
        self.init.symlink_to(self.daemon)
        self.run_helper('preflight', ok=False, error='symlink')
        self.assertEqual(self.event_lines(), [])

    def test_factory_symlink_hardlink_fifo_and_socket_rejected(self):
        self.enroll()
        evil = self.factory / '中文\n危险'
        for kind in ('symlink', 'dangling-link', 'directory-link', 'hardlink', 'fifo', 'socket'):
            with self.subTest(kind=kind):
                sock = None
                if kind == 'symlink':
                    evil.symlink_to(self.user_file)
                elif kind == 'dangling-link':
                    evil.symlink_to(self.root / 'absent')
                elif kind == 'directory-link':
                    evil.symlink_to(self.user_file.parent, target_is_directory=True)
                elif kind == 'hardlink':
                    os.link(self.user_file, evil)
                elif kind == 'fifo':
                    os.mkfifo(evil)
                else:
                    sock = socket.socket(socket.AF_UNIX)
                    sock.bind(str(evil))
                try:
                    self.run_helper('remove-factory', ok=False, error='tree-symlink-special-or-hardlink')
                finally:
                    if sock:
                        sock.close()
                    evil.unlink()
                self.assertEqual(self.event_lines(), [])
                self.assertFalse((self.state / 'removing').exists())
        self.assertTrue((self.factory / 'mpenMain').exists())

    def test_factory_root_symlink_regular_file_and_ancestor_rejected(self):
        self.enroll()
        shutil.rmtree(self.factory)
        self.factory.symlink_to(self.user_file.parent, target_is_directory=True)
        self.run_helper('remove-factory', ok=False, error='symlink')
        self.factory.unlink()
        self.factory.write_text('not a directory')
        self.run_helper('remove-factory', ok=False, error='factory-tree-not-directory')
        self.factory.unlink()
        self.factory.parent.rmdir()
        self.factory.parent.symlink_to(self.user_file.parent, target_is_directory=True)
        self.run_helper('remove-factory', ok=False, error='symlink')
        self.assertEqual(self.event_lines(), [])

    def test_factory_and_nested_mounts_rejected(self):
        self.enroll()
        for target in (self.factory, self.factory / 'assets'):
            self.mounts(extra=f'evil {target} ext4 rw 0 0\n')
            self.run_helper('remove-factory', ok=False, error='nested-mount')
        self.assertEqual(self.event_lines(), [])
        self.assertTrue((self.factory / 'mpenMain').exists())

    def test_unenrolled_and_invalid_core_state_rejected(self):
        self.run_helper('remove-factory', ok=False, error='not-single-link-file')
        self.enroll()
        self.run_helper('remove-factory', ok=False, error='enrolled-core-invalid', TEST_INVALID_CORE='1')
        self.run_helper('remove-factory', ok=False, error='enrolled-state-invalid', TEST_CORE_PHASE='pending')
        (self.data / 'c1/core/core.fixture').write_bytes(b'tampered core')
        self.run_helper('remove-factory', ok=False, error='enrolled-core-invalid')
        self.assertTrue((self.factory / 'mpenMain').exists())
        self.assertEqual(self.event_lines(), [])
        self.assertFalse((self.state / 'no-factory-backup').exists())

    def test_enrollment_components_tamper_prevents_removal(self):
        self.enroll()
        for path, error in ((self.daemon, 'hash-rejected'),
                            (self.root / 'etc/c1updater/recovery-verifier', 'hash-rejected'),
                            (self.root / 'etc/c1updater/core.ed25519.pub', 'hash-rejected'),
                            (self.root / 'etc/c1updater/bootstrap.version', 'bootstrap-version-invalid'),
                            (self.data / 'c1/update/enrolled.v1', 'enrollment-marker-invalid')):
            with self.subTest(path=path):
                original = path.read_bytes()
                path.write_bytes(b'tampered')
                self.run_helper('remove-factory', ok=False, error=error)
                path.write_bytes(original)
        self.assertTrue(self.factory.exists())
        self.assertEqual(self.event_lines(), [])

    def test_removal_rechecks_gates_after_stopping_chain(self):
        self.enroll()
        init = self.init.read_bytes()
        core = self.data / 'c1/core/core.fixture'
        original_core = core.read_bytes()
        for change, error in (('tree', 'tree-symlink-special-or-hardlink'),
                              ('init', 'unknown-S80app-sha256'),
                              ('core', 'enrolled-core-invalid'), ('mount', 'nested-mount')):
            with self.subTest(change=change):
                self.run_helper('remove-factory', ok=False, error=error, TEST_STOP_CHANGE=change)
                self.assertNotIn('mount:rw', self.event_lines())
                self.assertFalse((self.state / 'removing').exists())
                self.assertTrue((self.factory / 'mpenMain').exists())
                self.assert_root_ro()
                (self.factory / 'late-link').unlink(missing_ok=True)
                self.init.write_bytes(init)
                core.write_bytes(original_core)
                self.mounts()

    def test_chinese_space_newline_names_removed_without_backups_and_idempotent(self):
        (self.factory / '中文 目录').mkdir()
        for name in ('原厂\n文件.dat', 'space file', '-leading-option'):
            (self.factory / '中文 目录' / name).write_bytes(b'vendor')
        (self.factory / '空\n目录').mkdir()
        self.enroll()
        bootstrap = self.daemon.read_bytes()
        self.run_helper('remove-factory')
        self.assertFalse(self.factory.exists())
        for name in ('no-factory-backup', 'removing', 'removed'):
            self.assertEqual((self.state / name).read_text(), FLAG)
        for forbidden in ('mpenMain', '/usr/bin/d261', 'force-original', 'killall', 'reboot'):
            self.assertNotIn(forbidden, self.init.read_text())
        self.assertIn('app_daemon', self.init.read_text())
        self.assertEqual(self.daemon.read_bytes(), bootstrap)
        self.assertEqual(self.event_lines(), ['stop', 'mount:rw', 'mount:ro', 'start'])
        self.assert_root_ro()
        self.run_helper('remove-factory')
        self.assertEqual(self.event_lines().count('mount:rw'), 1)

    def test_partial_delete_returns_ro_and_retry_validates_markers_and_init(self):
        self.enroll()
        self.run_helper('remove-factory', ok=False, TEST_PARTIAL_DELETE='1')
        self.assertFalse((self.factory / 'mpenMain').exists())
        self.assertTrue((self.factory / 'assets/lesson.dat').exists())
        self.assertFalse((self.state / 'removed').exists())
        self.assertEqual(self.event_lines(), ['stop', 'mount:rw', 'mount:ro', 'start'])
        self.assert_root_ro()
        approved_init = self.init.read_bytes()
        self.init.write_text('#!/bin/sh\nunknown after interruption\n')
        self.run_helper('remove-factory', ok=False, error='unknown-S80app-sha256')
        self.init.write_bytes(approved_init)
        for name in ('no-factory-backup', 'removing'):
            marker = self.state / name
            marker.write_text('invalid')
            self.run_helper('remove-factory', ok=False, error='hash-rejected')
            marker.unlink()
            self.run_helper('remove-factory', ok=False, error='not-single-link-file')
            marker.write_text(FLAG)
        self.assertEqual(self.event_lines().count('mount:rw'), 1)
        self.run_helper('remove-factory')
        self.assertFalse(self.factory.exists())
        self.assertEqual(self.event_lines().count('mount:rw'), 2)
        self.assert_root_ro()

    def test_root_rw_failure_cleans_up_without_removal_and_can_retry(self):
        self.enroll()
        self.run_helper('remove-factory', ok=False, error='root-remount-rw-failed', TEST_FAIL_REMOUNT='rw')
        self.assertTrue((self.factory / 'mpenMain').exists())
        self.assertEqual(sha(self.init), INIT_HASH)
        self.assertFalse((self.state / 'removed').exists())
        self.assertEqual(self.event_lines(), ['stop', 'mount:rw', 'mount:ro', 'start'])
        self.assert_root_ro()
        self.run_helper('remove-factory')
        self.assertFalse(self.factory.exists())

    def test_root_ro_failure_never_starts_and_can_retry(self):
        self.enroll()
        self.run_helper('remove-factory', ok=False, error='root-remount-ro-failed', TEST_FAIL_REMOUNT='ro')
        self.assertFalse(self.factory.exists())
        self.assertFalse((self.state / 'removed').exists())
        self.assertNotIn('start', self.event_lines())
        self.run_helper('remove-factory', ok=False, error='root-must-enter-read-only')
        self.mounts()  # Simulate independently restored read-only root after failure.
        self.run_helper('remove-factory')
        self.assertTrue((self.state / 'removed').exists())
        self.assertEqual(self.event_lines().count('mount:rw'), 1)
        self.assertEqual(self.event_lines()[-1], 'start')

    def test_start_failure_is_reported_and_idempotent_retry_succeeds(self):
        self.enroll()
        self.run_helper('remove-factory', ok=False, TEST_FAIL_START='1')
        self.assertFalse(self.factory.exists())
        self.assert_root_ro()
        self.run_helper('remove-factory')
        self.assertEqual(self.event_lines().count('mount:rw'), 1)
        self.assertTrue((self.root / 'running').exists())

    def test_missing_tree_without_transaction_rejected(self):
        self.enroll()
        shutil.rmtree(self.factory)
        self.run_helper('remove-factory', ok=False, error='not-single-link-file')
        self.assertEqual(sha(self.init), INIT_HASH)
        self.assertEqual(self.event_lines(), [])

    def test_restored_or_invalid_transaction_flags_prevent_removal(self):
        self.enroll()
        self.run_helper('preflight')
        for name in ('no-factory-backup', 'removing', 'removed', 'restored'):
            marker = self.state / name
            marker.write_text('invalid')
            self.run_helper('remove-factory', ok=False,
                            error='factory-restored' if name == 'restored' else 'hash-rejected')
            marker.unlink()
            marker.symlink_to(self.user_file)
            self.run_helper('remove-factory', ok=False, error='symlink')
            marker.unlink()
        self.assertTrue(self.factory.exists())
        self.assertEqual(self.event_lines(), [])

    def test_historical_backups_are_untouched_and_not_required(self):
        # Deliberately incomplete/invalid historical backups must not be read,
        # modified or deleted, including an interrupted old .new directory.
        before = {}
        for backup in (self.backup_data, self.backup_storage,
                       self.backup_data.with_name(self.backup_data.name + '.new')):
            backup.mkdir(parents=True)
            path = backup / '历史\n备份'
            path.write_bytes(b'leave historical bytes alone')
            before[path] = (path.read_bytes(), path.stat().st_mode)
        self.enroll()
        self.run_helper('accessories', self.payload())
        self.run_helper('remove-factory')
        self.run_helper('verify')
        for path, expected in before.items():
            self.assertEqual((path.read_bytes(), path.stat().st_mode), expected)
            self.assertEqual(list(path.parent.iterdir()), [path])
        self.run_helper('restore-factory', ok=False, error='usage')

    def test_missing_wallpaper_restored_and_neofetch_complete(self):
        payload = self.payload()
        self.run_helper('accessories', payload)
        wallpaper = self.storage / 'mtp/Pic/wallpaper.raw'
        self.assertEqual(wallpaper.read_bytes(), b'\x55' * 5624)
        self.assertFalse((self.storage / 'mtp/pic').exists())
        for name in ('Pic', 'Music', 'Book'):
            self.assertTrue((self.storage / 'mtp' / name).is_dir())
        wallpaper.unlink()
        self.run_helper('accessories', payload)
        self.assertEqual(wallpaper.read_bytes(), b'\x55' * 5624)
        for name in ('neofetch.upstream', 'c1-config.conf', 'c1-logo.txt', 'LICENSE.md'):
            self.assertEqual(sha(self.data / 'c1/neofetch' / name), sha(payload / name))
        profile = self.root / 'etc/profile.d/90-c1-path.sh'
        self.assertIn(str(self.data / 'c1/bin'), profile.read_text())
        located = subprocess.run(['/bin/sh', '-c', '. "$1"; command -v neofetch', 'test', str(profile)],
                                 capture_output=True, text=True, check=True)
        self.assertEqual(located.stdout.strip(), str(self.data / 'c1/bin/neofetch'))
        self.assertEqual(self.event_lines().count('mount:rw'), 1)

    def test_existing_wallpaper_is_never_overwritten(self):
        wallpaper = self.storage / 'mtp/Pic/wallpaper.raw'
        wallpaper.parent.mkdir(parents=True, exist_ok=True)
        wallpaper.write_bytes(b'user wallpaper')
        self.run_helper('accessories', self.payload())
        self.assertEqual(wallpaper.read_bytes(), b'user wallpaper')

    def test_payload_tamper_and_malformed_manifest_rejected(self):
        payload = self.payload()
        (payload / 'neofetch').write_bytes(b'bad')
        self.run_helper('accessories', payload, ok=False, error='hash-rejected')
        self.assertFalse((self.data / 'c1/bin/neofetch').exists())
        self.payload_manifest(payload)
        with (payload / 'SHA256SUMS').open('a') as output:
            output.write('0' * 64 + '  ../evil\n')
        self.run_helper('accessories', payload, ok=False, error='payload-manifest-line-count')
        self.assertEqual(self.event_lines(), [])

    def test_profile_and_wallpaper_symlinks_rejected(self):
        payload = self.payload()
        profile = self.root / 'etc/profile.d/90-c1-path.sh'
        profile.symlink_to(self.user_file)
        self.run_helper('accessories', payload, ok=False, error='symlink')
        profile.unlink()
        wallpaper = self.storage / 'mtp/Pic/wallpaper.raw'
        wallpaper.parent.mkdir(parents=True, exist_ok=True)
        wallpaper.symlink_to(self.user_file)
        self.run_helper('accessories', payload, ok=False, error='symlink')
        self.assertEqual(self.event_lines(), [])

    def test_lock_is_not_stolen(self):
        lock = self.data / 'c1/installer.lock'
        lock.mkdir(parents=True)
        (lock / 'owner').write_text('other operation')
        self.run_helper('prepare', ok=False, error='installer-busy-or-stale-lock')
        self.assertEqual((lock / 'owner').read_text(), 'other operation')

    def test_existing_enrollment_restores_missing_default_wallpaper(self):
        self.enroll()
        self.run_helper('accessories', self.payload(), TEST_SUSPEND_SUPPORTED='1')
        self.assertFalse((self.storage / 'mtp/pic').exists())
        self.assertEqual((self.storage / 'mtp/Pic/wallpaper.raw').read_bytes(), b'\x55' * 5624)
        self.assertFalse((self.data / 'c1/disable-auto-suspend').exists())

    def test_payload_filename_is_exact_and_wallpaper_size_is_checked(self):
        payload = self.payload()
        manifest = payload / 'SHA256SUMS'
        manifest.write_text(manifest.read_text().replace('neofetch.upstream', 'neofetchXupstream'))
        self.run_helper('accessories', payload, ok=False, error='payload-manifest-invalid')
        (payload / 'wallpaper.raw').write_bytes(b'wrong size')
        self.payload_manifest(payload)
        self.run_helper('accessories', payload, ok=False, error='wallpaper-size-invalid')
        self.assertEqual(self.event_lines(), [])

    def test_verify_requires_valid_flags_and_all_installed_checks(self):
        self.enroll()
        self.run_helper('accessories', self.payload())
        self.run_helper('remove-factory')
        self.run_helper('verify')
        for name in ('no-factory-backup', 'removing', 'removed', 'prepared', 'accessories', 'wallpaper-handled'):
            marker = self.state / name
            marker.unlink()
            self.run_helper('verify', ok=False, error='not-single-link-file')
            marker.write_text('invalid')
            self.run_helper('verify', ok=False, error='hash-rejected')
            marker.unlink()
            marker.symlink_to(self.user_file)
            self.run_helper('verify', ok=False, error='symlink')
            marker.unlink()
            marker.write_text(FLAG)
        self.run_helper('verify', ok=False, error='enrolled-core-invalid', TEST_INVALID_CORE='1')
        self.run_helper('verify', ok=False, error='enrolled-state-invalid', TEST_CORE_PHASE='pending')
        (self.root / 'running').unlink()
        self.run_helper('verify', ok=False, error='bootstrap-not-running')
        (self.root / 'running').touch()
        self.factory.mkdir()
        self.run_helper('verify', ok=False, error='factory-still-present')
        self.factory.rmdir()
        (self.state / 'restored').write_text(FLAG)
        self.run_helper('verify', ok=False, error='factory-was-restored')
        (self.state / 'restored').unlink()
        for path in (self.data / 'c1/neofetch/c1-logo.txt', self.root / 'etc/profile.d/90-c1-path.sh'):
            original = path.read_bytes()
            path.write_text('tampered')
            self.run_helper('verify', ok=False, error='hash-rejected')
            path.write_bytes(original)
        self.run_helper('verify')
        self.assertEqual(self.event_lines().count('mount:rw'), 2)

    def test_full_workflow_verify_and_removed_wallpaper_allowed(self):
        self.run_helper('preflight')
        self.run_helper('prepare')
        self.enroll()
        self.run_helper('accessories', self.payload())
        self.run_helper('remove-factory')
        self.run_helper('verify')
        (self.storage / 'mtp/Pic/wallpaper.raw').unlink()
        (self.data / 'c1/disable-auto-suspend').unlink()
        self.run_helper('verify')
        (self.data / 'c1/neofetch/c1-logo.txt').write_text('tampered')
        self.run_helper('verify', ok=False, error='hash-rejected')
        self.assert_root_ro()


if __name__ == '__main__':
    unittest.main(verbosity=2)
