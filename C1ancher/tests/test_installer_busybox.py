"""Actual device BusyBox compatibility tests, entirely in private host fixtures.

Run from WSL as root (no device connection is used)::

    python3 -m unittest discover -s tests -p test_installer_busybox.py -v

The reviewed MIPS binary and firmware libraries are read-only inputs. Each
external helper applet is dispatched through qemu-mipsel-static; host /bin/sh
only dispatches wrappers and the explicitly stubbed enrollment verifier.
Mounts, ownership, disk space, process control and sync use the existing fixture
stubs. No chroot, real mount, ADB, reboot, or production bypass is required.
"""
import os
import shlex
import shutil
import subprocess
import unittest

import test_installer_device as fixture

BUSYBOX = fixture.ROOT / 'build/busybox-device-20260906/busybox'
BUSYBOX_SHA256 = '69cd474401a9c47e8911cb352faebbf5cd731db63d30f035e5a0afcb82f74179'
QEMU = shutil.which('qemu-mipsel-static')
APPLETS = ('awk', 'cat', 'chmod', 'cp', 'false', 'find', 'grep', 'ln', 'mkdir', 'mv',
           'readlink', 'rm', 'sed', 'sh', 'sha256sum', 'stat', 'tr', 'wc')


@unittest.skipUnless(os.name == 'posix' and QEMU and BUSYBOX.is_file(),
                     'Requires Linux/WSL, qemu-mipsel-static and reviewed device BusyBox')
class InstallerBusyBoxTests(unittest.TestCase):
    # Reuse fixture construction and assertions, not its host-shell test suite.
    mounts = fixture.InstallerDeviceTests.mounts
    payload = fixture.InstallerDeviceTests.payload
    payload_manifest = fixture.InstallerDeviceTests.payload_manifest
    enroll = fixture.InstallerDeviceTests.enroll
    event_lines = fixture.InstallerDeviceTests.event_lines
    assert_root_ro = fixture.InstallerDeviceTests.assert_root_ro
    suspend_hardware = fixture.InstallerDeviceTests.suspend_hardware
    test_fresh_supported_prepare_enables_without_probe_and_repeats = fixture.InstallerDeviceTests.test_fresh_supported_prepare_enables_without_probe_and_repeats
    test_fresh_unsupported_hardware_disables_even_with_old_proof = fixture.InstallerDeviceTests.test_fresh_unsupported_hardware_disables_even_with_old_proof
    test_prepare_preserves_all_existing_disable_marker_contents = fixture.InstallerDeviceTests.test_prepare_preserves_all_existing_disable_marker_contents
    test_enrolled_and_legacy_unsupported_hardware_fails_closed = fixture.InstallerDeviceTests.test_enrolled_and_legacy_unsupported_hardware_fails_closed
    test_enable_suspend_clears_valid_preferences_and_is_repeatable = fixture.InstallerDeviceTests.test_enable_suspend_clears_valid_preferences_and_is_repeatable
    test_suspend_requires_this_payload_hashes_before_power_commands = fixture.InstallerDeviceTests.test_suspend_requires_this_payload_hashes_before_power_commands
    test_suspend_rejects_idle_and_mismatched_confirmed_identity = fixture.InstallerDeviceTests.test_suspend_rejects_idle_and_mismatched_confirmed_identity

    @classmethod
    def setUpClass(cls):
        if fixture.sha(BUSYBOX) != BUSYBOX_SHA256:
            raise AssertionError('Device BusyBox SHA256 differs from reviewed input')
        cls.command = [QEMU, '-L', str(fixture.FIRMWARE), str(BUSYBOX)]
        result = subprocess.run(cls.command, capture_output=True, text=True, timeout=10)
        if 'BusyBox v1.36.1' not in result.stdout + result.stderr:
            raise AssertionError(result.stdout + result.stderr)

    def setUp(self):
        fixture.InstallerDeviceTests.setUp(self)
        self.bin = self.root / 'test-bin'
        self.bin.mkdir()
        self.audit = self.root / 'applets.log'
        self.env = dict(FIXTURE=str(self.root), QEMU=QEMU, BB=str(BUSYBOX),
                        BB_LIB=str(fixture.FIRMWARE), LC_ALL='C.UTF-8')
        # A closed PATH prevents silent fallback to GNU utilities. Wrappers use
        # only host shell builtins before executing the exact target applet.
        for applet in APPLETS:
            wrapper = self.bin / applet
            wrapper.write_text('#!/bin/sh\n'
                               f'applet={shlex.quote(applet)}\n' + r'''
printf '%s\n' "$applet" >>"$FIXTURE/applets.log"
tree=0
for arg do
    case "$arg" in "$FIXTURE/usr/bin/d261/"*) tree=1 ;; esac
done
if [ "$applet" = sh ] && [ "$tree" = 1 ] && [ "${TEST_SHELL_FAIL:-0}" = 1 ]; then
    exec "$FIXTURE/missing-shell-executable"
fi
if [ "$applet" = false ] && [ "${TEST_PROBE_FAIL:-}" = negative ]; then exit 0; fi
if [ "$applet" = sh ] && [ "$tree" = 0 ] && [ "${TEST_PROBE_FAIL:-}" = positive ]; then exit 73; fi
if [ "$applet" = stat ]; then
    # Only the old-batch negative control bounds the target find's stack.
    if [ "${TEST_SMALL_BATCHES:-0}" = 1 ]; then ulimit -s 8192; fi
    if [ "$tree" = 1 ]; then
        printf '%s\n' "$(($# - 2))" >>"$FIXTURE/stat-batches"
        count=0
        if [ -e "$FIXTURE/stat-count" ]; then read -r count <"$FIXTURE/stat-count"; fi
        count=$((count + 1))
        printf '%s\n' "$count" >"$FIXTURE/stat-count"
        failure=${TEST_STAT_FAIL:-}
        if [ -n "${TEST_STAT_AT:-}" ] && [ "$count" != "$TEST_STAT_AT" ]; then failure=; fi
        case "$failure" in
            before) exit 71 ;;
            first-within)
                if [ ! -e "$FIXTURE/first-batch-failed" ]; then
                    : >"$FIXTURE/first-batch-failed"
                    set -- "$@" "$FIXTURE/nonexistent-stat-operand"
                fi ;;
            within)
                # Preserve evidence that actual stat emitted 1 before its
                # missing operand returned nonzero. The predicate must reject
                # the failed command even when its captured output looks safe.
                "$QEMU" -L "$BB_LIB" "$BB" stat "$@" "$FIXTURE/nonexistent-stat-operand" >"$FIXTURE/failed-stat-output"
                result=$?
                "$QEMU" -L "$BB_LIB" "$BB" cat "$FIXTURE/failed-stat-output"
                exit "$result" ;;
        esac
    fi
fi
if [ "$applet" = find ]; then
    # Bound argv only in the multi-batch stress fixture. Long paths keep the
    # 32-bit/64-bit argv pointer overhead below BusyBox's safety margin.
    if [ "${TEST_SMALL_BATCHES:-0}" = 1 ]; then ulimit -s 512; fi
    scan=unsafe
    for arg do
        case "$arg" in -exec) scan=links ;; esac
    done
    "$QEMU" -L "$BB_LIB" "$BB" find "$@"
    result=$?
    if [ "$scan" = links ] && [ "$1" = "$FIXTURE/usr/bin/d261" ]; then
        "$QEMU" -L "$BB_LIB" "$BB" cp \
            "$FIXTURE/usr/data/c1/installer.lock/tree-links" "$FIXTURE/link-counts"
    fi
    if [ "${TEST_FIND_FAIL:-}" = "$scan" ] && [ "$1" = "$FIXTURE/usr/bin/d261" ]; then exit 72; fi
    exit "$result"
fi
exec "$QEMU" -L "$BB_LIB" "$BB" "$applet" "$@"
''')
            wrapper.chmod(0o700)
        text = self.script.read_text()
        old = 'PATH=/bin:/sbin:/usr/bin:/usr/sbin\n'
        self.assertEqual(text.count(old), 1)
        text = text.replace(old, f'PATH={self.bin}\n')
        self.script.write_text(text)
        self.history = {}
        for backup in (self.backup_data, self.backup_storage,
                       self.backup_data.with_name(self.backup_data.name + '.new')):
            backup.mkdir(parents=True)
            path = backup / '历史\n备份'
            path.write_bytes(b'preserve historical recovery bytes')
            self.history[path] = (path.read_bytes(), path.stat().st_mode)

    def bb(self, *args, **kwargs):
        return subprocess.run(self.command + list(map(str, args)),
                              capture_output=True, text=True, timeout=120,
                              env=dict(self.env, PATH=str(self.bin)), cwd=self.root, **kwargs)

    def run_helper(self, action, *args, ok=True, error=None, **env):
        if action in ('enable-suspend', 'verify-suspend') and not args and error != 'usage':
            args = getattr(self, 'suspend_identity', ('0' * 64, '0' * 64))
        result = subprocess.run(self.command + ['sh', str(self.script), action,
                                               *map(str, args)],
                                env=dict(self.env, **env), capture_output=True,
                                text=True, timeout=180, cwd=self.root)
        if ok is True:
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn(f'C1SETUP_OK {action}\n', result.stdout)
        elif ok is False:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertNotIn(f'C1SETUP_OK {action}\n', result.stdout)
            if error:
                self.assertIn(error, result.stderr)
        self.assertEqual(self.user_file.read_text(), 'precious user book')
        self.assertEqual(self.app_file.read_text(), 'ordinary app data')
        for path, expected in self.history.items():
            self.assertEqual((path.read_bytes(), path.stat().st_mode), expected)
            self.assertEqual(list(path.parent.iterdir()), [path])
        return result

    def assert_not_deleted(self):
        self.assertTrue((self.factory / 'mpenMain').is_file())
        self.assertTrue((self.factory / 'assets/lesson.dat').is_file())
        self.assertEqual(fixture.sha(self.init), fixture.INIT_HASH)
        self.assertNotIn('mount:rw', self.event_lines())
        self.assertFalse((self.state / 'removing').exists())
        self.assert_root_ro()

    test_supported_first_install_enables_suspend = fixture.InstallerDeviceTests.test_supported_first_install_enables_suspend
    test_unsupported_retry_fails_closed = fixture.InstallerDeviceTests.test_unsupported_retry_fails_closed
    test_existing_disable_preference_preserved = fixture.InstallerDeviceTests.test_existing_disable_preference_preserved
    test_existing_enrollment_preserves_suspend_preference = fixture.InstallerDeviceTests.test_existing_enrollment_preserves_suspend_preference

    def test_exact_binary_rejects_gnu_links_and_propagates_exec_errors(self):
        # Negative control: this was accepted by the old GNU-only host tests.
        result = self.bb('find', self.factory, '-type', 'f', '-links', '+1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('-links', result.stderr)
        for executable in ('/bin/false', str(self.root / 'missing-command')):
            with self.subTest(executable=executable):
                result = self.bb('find', self.factory, '-type', 'f',
                                 '-exec', executable, '{}', '+')
                self.assertEqual(result.returncode, 1, result.stderr)
        self.assert_not_deleted()

    def test_complete_sequential_workflow_and_repeats(self):
        names = ('中文 目录/原厂\n文件.dat', 'space file', '-leading-option',
                 'line\n2\n1', '$(touch INJECTED)', '`touch INJECTED`',
                 'semi;touch INJECTED', 'quote\'"\\name',
                 '$(printf compromised>INJECTED)', '`printf compromised>INJECTED`')
        for name in names:
            path = self.factory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'vendor fixture')
        (self.factory / '空\n目录').mkdir()
        payload = self.payload()
        self.run_helper('preflight')
        self.run_helper('prepare')
        self.run_helper('accessories', payload)
        self.enroll()  # Explicit verifier-contract fixture, not cryptography.
        bootstrap = self.daemon.read_bytes()
        result = self.run_helper('remove-factory')
        self.assertFalse(self.factory.exists())
        self.assertEqual(self.daemon.read_bytes(), bootstrap)
        self.assertEqual((self.root / 'link-counts').read_bytes(), b'')
        for name in names:
            self.assertNotIn(name, result.stdout + result.stderr)
        self.assertFalse((self.root / 'INJECTED').exists())
        self.run_helper('verify')
        # Check the generated startup with the device parser; never execute it.
        result = self.bb('sh', '-n', self.init)
        self.assertEqual(result.returncode, 0, result.stderr)
        (self.data / 'c1/disable-auto-suspend').unlink()
        for action in ('preflight', 'prepare', 'accessories', 'remove-factory', 'verify'):
            self.run_helper(action, *([payload] if action == 'accessories' else []),
                            TEST_SUSPEND_SUPPORTED='1')
        self.assertFalse((self.data / 'c1/disable-auto-suspend').exists())
        self.assertEqual(self.event_lines().count('mount:rw'), 2)
        self.assert_root_ro()
        called = set(self.audit.read_text().splitlines())
        self.assertTrue({'find', 'stat', 'sh', 'awk', 'cp', 'mv', 'rm', 'grep', 'sed'} <= called,
                        f'Actual target applets missing: {called}')
        for flag in ('no-factory-backup', 'removing', 'removed'):
            self.assertEqual((self.state / flag).read_text(), fixture.FLAG)

    def test_original_helper_and_generated_startup_syntax(self):
        source = fixture.SOURCE.read_text()
        generated = source.split("<<'C1_INIT'\n", 1)[1].split('\nC1_INIT\n', 1)[0]
        startup = self.root / 'original-startup.sh'
        startup.write_text(generated + '\n')
        for path in (fixture.SOURCE, startup):
            result = self.bb('sh', '-n', path)
            self.assertEqual(result.returncode, 0, result.stderr)

    test_unsafe_tree_types = fixture.InstallerDeviceTests.test_factory_symlink_hardlink_fifo_and_socket_rejected
    test_nested_mounts = fixture.InstallerDeviceTests.test_factory_and_nested_mounts_rejected
    test_recheck_after_stop = fixture.InstallerDeviceTests.test_removal_rechecks_gates_after_stopping_chain

    def test_stat_and_find_failures_prevent_any_deletion(self):
        self.enroll()
        for env in ({'TEST_STAT_FAIL': 'before'}, {'TEST_STAT_FAIL': 'within'},
                    {'TEST_SHELL_FAIL': '1'}, {'TEST_FIND_FAIL': 'unsafe'},
                    {'TEST_FIND_FAIL': 'links'}):
            with self.subTest(env=env):
                error = ('tree-enumeration-failed' if 'TEST_FIND_FAIL' in env
                         else 'tree-symlink-special-or-hardlink')
                self.run_helper('remove-factory', ok=False, error=error, **env)
                self.assert_not_deleted()
                self.assertEqual(self.event_lines(), [])
                if env.get('TEST_STAT_FAIL') == 'within':
                    # Rejection output is an opaque path, never numeric counts.
                    self.assertTrue((self.root / 'link-counts').read_bytes())

    def test_first_middle_last_stat_failure_with_and_without_output(self):
        for index in range(6):
            (self.factory / f'中文\nfixture {index}').write_bytes(b'vendor')
        self.enroll()
        for ordinal in (1, 4, 8):
            for mode in ('before', 'within'):
                with self.subTest(ordinal=ordinal, mode=mode):
                    (self.root / 'stat-count').unlink(missing_ok=True)
                    result = self.run_helper('remove-factory', ok=False,
                                             error='tree-symlink-special-or-hardlink',
                                             TEST_STAT_AT=str(ordinal), TEST_STAT_FAIL=mode)
                    self.assertEqual(int((self.root / 'stat-count').read_text()), ordinal)
                    if mode == 'within':
                        self.assertEqual((self.root / 'failed-stat-output').read_bytes(), b'1\n')
                    self.assert_not_deleted()
                    self.assertEqual(self.event_lines(), [])
                    rejected = (self.root / 'link-counts').read_bytes()
                    self.assertTrue(rejected)
                    self.assertNotIn(rejected.decode().rstrip('\n'), result.stdout)

    def test_preflight_positive_and_negative_predicate_controls(self):
        for mode, error in (('positive', 'tree-scan-tools-unsupported'),
                            ('negative', 'tree-scan-child-errors-not-detected')):
            with self.subTest(mode=mode):
                self.run_helper('preflight', ok=False, error=error, TEST_PROBE_FAIL=mode)
                self.assert_not_deleted()
                self.assertEqual(self.event_lines(), [])

    def test_start_core_reuses_enrollment_without_removal(self):
        self.enroll()
        before = {path: path.read_bytes() for path in
                  (self.daemon, self.data / 'c1/core/core.fixture',
                   self.data / 'c1/update/enrolled.v1')}
        (self.root / 'running').unlink()
        self.run_helper('start-core')
        self.run_helper('start-core')
        self.assertTrue((self.root / 'running').exists())
        for path, contents in before.items():
            self.assertEqual(path.read_bytes(), contents)
        self.assert_not_deleted()
        self.assertNotIn('stop', self.event_lines())
        self.run_helper('start-core', ok=False, error='enrolled-core-invalid',
                        TEST_INVALID_CORE='1')

    def populate_batched_tree(self):
        # Long filenames plus a private child stack limit exercise several
        # real BusyBox batches with only 600 files, rather than thousands.
        directory = self.factory
        for index in range(4):
            directory /= f'd{index}' + 'x' * 150
        directory.mkdir(parents=True)
        for index in range(600):
            (directory / (f'{index:05d} 中文 ' + 'x' * 190)).write_bytes(b'x')

    def test_old_batch_scan_negative_control_loses_nonfinal_failure(self):
        self.populate_batched_tree()
        self.enroll()
        # Deliberate regression ONLY in this private script copy. This proves
        # why the old implementation must never return to production. A real
        # stat emits good counts and fails in the first of several batches;
        # this BusyBox loses that status, and the MUTATED fixture is deleted.
        text = self.script.read_text()
        start = text.index('scan_factory_for_removal() {')
        end = text.index('\ncheck_tree_scan_tools() {', start)
        old_scan = r'''scan_factory_for_removal() {
    safe_path "$FACTORY"
    no_submounts "$FACTORY"
    [ -e "$FACTORY" ] || return 0
    [ -d "$FACTORY" ] || fail factory-tree-not-directory
    find "$FACTORY" ! -type d ! -type f -print -quit >"$LOCK/tree-unsafe" || fail tree-enumeration-failed
    [ ! -s "$LOCK/tree-unsafe" ] || fail tree-symlink-special-or-hardlink
    find "$FACTORY" -type f -exec stat -c %h {} + >"$LOCK/tree-links" || fail tree-enumeration-failed
    awk '$0 != "1" {bad=1} END {exit bad}' "$LOCK/tree-links" || fail tree-symlink-special-or-hardlink
}
'''
        self.script.write_text(text[:start] + old_scan + text[end:])
        result = self.run_helper('remove-factory', TEST_SMALL_BATCHES='1',
                                 TEST_STAT_FAIL='first-within')
        self.assertIn('nonexistent-stat-operand', result.stderr)
        self.assertFalse(self.factory.exists())
        batches = [int(n) for n in (self.root / 'stat-batches').read_text().splitlines()]
        self.assertGreater(len(batches), 2)
        self.assertGreater(max(batches), 1)
        self.assertEqual(sum(batches), 602 * 2)
        self.assert_root_ro()

    def test_large_tree_per_file_scan_removes_all_600_fixtures(self):
        self.populate_batched_tree()
        self.enroll()
        result = self.run_helper('remove-factory')
        counts = [int(n) for n in (self.root / 'stat-batches').read_text().splitlines()]
        self.assertEqual(counts, [1] * (602 * 2))  # Before and after stopping.
        self.assertEqual((self.root / 'link-counts').read_bytes(), b'')
        self.assertEqual(result.stderr, '')
        self.assertFalse(self.factory.exists())
        self.assert_root_ro()


if __name__ == '__main__':
    unittest.main(verbosity=2)
