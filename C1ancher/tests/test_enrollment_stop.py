"""Offline maintenance stop regressions: real shell policy, virtual processes/time.

Only extracted production functions execute, never the script's CLI or traps.
/proc is a private directory of regular executable-target records; only the
symlink predicate and readlink boundary are replaced (also works on Windows
without symlink privileges). Production executable matching remains unchanged.
Signals, service stop, root remount and the first protected write are recording
stubs. No real sleep, signal, mount, signing, ADB or network operation is used.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'scripts/device-core-enroll.sh'


def function(text, name):
    match = re.search(r'^' + re.escape(name) + r'\(\) \{\n.*?^\}', text, re.M | re.S)
    if match is None:
        raise AssertionError(f'production function missing: {name}')
    return match.group()


def shell_path():
    configured = os.environ.get('C1_TEST_SH')
    if configured:
        return configured
    shell = shutil.which('sh')
    if shell:
        return shell
    if os.name == 'nt':
        for relative in ('Git/bin/sh.exe', 'Git/usr/bin/sh.exe'):
            candidate = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / relative
            if candidate.is_file():
                return str(candidate)
    return None


SHELL = shell_path()


@unittest.skipUnless(SHELL, 'Requires a POSIX shell (Linux/WSL or Git for Windows)')
class EnrollmentStopTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='c1-enrollment-stop-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = SOURCE.read_text(encoding='utf-8')
        # Flat record files avoid privileged symlink creation on Windows.
        entry = self.root / 'proc/42001'
        entry.mkdir(parents=True)
        self.executable = entry / 'exe'
        for name in ('enrolled', 'key', 'daemon'):
            (self.root / name).write_text('fixture\n', encoding='utf-8')

        running = function(self.source, 'app_daemon_running')
        self.assertEqual(running.count('/proc/[0-9]*/exe'), 1)
        running = running.replace('/proc/[0-9]*/exe', 'proc/[0-9]*/exe')
        self.assertEqual(running.count('[ -L "$executable" ]'), 1)
        running = running.replace('[ -L "$executable" ]', '[ -f "$executable" ]')
        stop = function(self.source, 'stop_chain')
        self.assertEqual(stop.count('/etc/init.d/S80app stop'), 1)
        stop = stop.replace('/etc/init.d/S80app stop', 'fixture_service_stop')
        self.assertNotIn('/proc/', running.replace('# /proc/', '# '))

        # Exercise the actual maintenance prefix through its first protected
        # write. Later installation commands are deliberately NOT executable
        # in this fixture. Do not manually recreate stop/remount/write order.
        maintenance = function(self.source, 'maintenance_bundle')
        match = re.search(r'^    copy_atomic .*$', maintenance, re.M)
        self.assertIsNotNone(match, 'maintenance first protected write missing')
        maintenance = maintenance[:match.end()] + '\n}\n'
        self.functions = '\n'.join((running, stop, maintenance))
        self.prefix = r'''set -eu
now=0
chain_stopped=0
core_root=/usr/data/c1/core
slot_root=/etc/c1updater
marker=enrolled
key_target=key
root_target=daemon
key_hash=fixture-hash
updater_hash=fixture-hash
record() { printf '%s:%s\n' "$now" "$*" >>events; }
fail() { record "fail:$*"; printf '%s\n' "$*" >&2; exit 41; }
startup_script_pids() {
    if [ "$now" -lt "$BOOT_UNTIL" ]; then printf '41001\n'; fi
}
sleep() {
    [ "$#" -eq 1 ] && [ "$1" = 1 ] || exit 90
    now=$((now + 1))
    record tick
}
kill() {
    [ "$#" -eq 2 ] && [ "$1" = -TERM ] && [ "$2" = 41001 ] || exit 91
    record term:41001
}
readlink() {
    [ "$#" -eq 1 ] && [ "$1" = proc/42001/exe ] || exit 92
    if [ "$now" -lt "$APP_UNTIL" ]; then
        IFS= read -r target <"$1"
        printf '%s\n' "$target"
    fi
}
fixture_service_stop() { record service-stop; }
verify_bundle() { record verify-bundle; }
hash_file() { printf 'fixture-hash\n'; }
baseline_allowed() { return 0; }
remount_root_rw() { record mount-rw; }
copy_atomic() { record slot-write; printf 'new-slot\n' >slot-write; }
'''

    def run_case(self, *, bootstrap_until=0, executable=None, app_until=1000):
        if executable is not None:
            self.executable.write_text(executable + '\n', encoding='utf-8')
        else:
            self.executable.unlink(missing_ok=True)
        for name in ('events', 'slot-write'):
            (self.root / name).unlink(missing_ok=True)
        script = self.prefix + self.functions + '\nmaintenance_bundle fixture-bundle\nrecord completed\n'
        # LF bytes are important when Windows Python invokes Git's POSIX shell.
        (self.root / 'probe.sh').write_bytes(script.encode('utf-8'))
        result = subprocess.run([SHELL, 'probe.sh'], cwd=self.root,
                                env=dict(os.environ, BOOT_UNTIL=str(bootstrap_until),
                                         APP_UNTIL=str(app_until)),
                                capture_output=True, text=True, timeout=10)
        events = (self.root / 'events').read_text(encoding='utf-8').splitlines()
        return result, events

    def assert_stopped_before_write(self, result, events, stopped_at):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(events[-3:], [f'{stopped_at}:mount-rw',
                                      f'{stopped_at}:slot-write',
                                      f'{stopped_at}:completed'])
        self.assertEqual((self.root / 'slot-write').read_text(), 'new-slot\n')

    def assert_no_write(self, result, events, error):
        self.assertEqual(result.returncode, 41, result.stdout + result.stderr)
        self.assertIn(error, result.stderr)
        self.assertFalse((self.root / 'slot-write').exists())
        for event in events:
            self.assertNotIn(event.split(':', 1)[1], ('mount-rw', 'slot-write', 'completed'))

    def test_legacy_logger_eight_second_stop_precedes_service_and_slot_write(self):
        result, events = self.run_case(bootstrap_until=8)
        self.assert_stopped_before_write(result, events, 8)
        self.assertEqual(events[1], '0:term:41001')
        self.assertIn('8:service-stop', events)
        self.assertEqual(sum(event.endswith(':tick') for event in events), 8)
        self.assertLess(events.index('8:service-stop'), events.index('8:mount-rw'))

    def test_bootstrap_alive_after_twenty_seconds_fails_before_service(self):
        result, events = self.run_case(bootstrap_until=1000)
        self.assert_no_write(result, events, 'startup scripts did not stop')
        self.assertEqual(events[-1], '20:fail:startup scripts did not stop')
        self.assertEqual(sum(event.endswith(':tick') for event in events), 20)
        self.assertFalse(any(event.endswith(':service-stop') for event in events))

    def test_bootstrap_exit_at_budget_boundary_is_accepted(self):
        result, events = self.run_case(bootstrap_until=20)
        self.assert_stopped_before_write(result, events, 20)
        self.assertIn('20:service-stop', events)

    def test_real_executable_selectors_block_lingering_chain_members(self):
        targets = (
            '/usr/data/c1/core/releases/8-1.3.6/artifacts/C1ancher',
            '/usr/data/c1/core/releases/8-1.3.6/artifacts/C1ancher-launcher',
            '/usr/data/c1/core/releases/8-1.3.6/artifacts/c1pkg',
            '/usr/data/c1/core/releases/8-1.3.6/artifacts/c1updater',
            '/etc/c1updater/slot-a/c1updater',
            '/etc/c1updater/slot-b/c1updater',
            '/etc/c1updater/recovery-verifier',
            '/usr/data/c1/bin/app_daemon',
            '/usr/bin/d261/mpenMain',
        )
        for target in targets:
            with self.subTest(executable=target):
                result, events = self.run_case(executable=target)
                self.assert_no_write(result, events, 'application chain did not stop')
                self.assertIn('0:service-stop', events)
                self.assertEqual(events[-1], '20:fail:application chain did not stop')
                self.assertEqual(sum(event.endswith(':tick') for event in events), 20)
                self.assertFalse(any(':term:' in event for event in events))

    def test_bootstrap_and_ui_have_separate_bounded_stop_budgets(self):
        result, events = self.run_case(
            bootstrap_until=8, app_until=28,
            executable='/usr/data/c1/core/releases/8-1.3.6/artifacts/C1ancher')
        self.assert_stopped_before_write(result, events, 28)
        self.assertIn('8:service-stop', events)
        self.assertEqual(sum(event.endswith(':tick') for event in events), 28)

    def test_lingering_ui_after_bootstrap_stop_never_reaches_root_write(self):
        result, events = self.run_case(
            bootstrap_until=8,
            executable='/usr/data/c1/core/releases/8-1.3.6/artifacts/C1ancher')
        self.assert_no_write(result, events, 'application chain did not stop')
        self.assertIn('8:service-stop', events)
        self.assertEqual(events[-1], '28:fail:application chain did not stop')

    def test_unrelated_executables_and_name_prefixes_do_not_block_or_get_signalled(self):
        for target in ('/usr/bin/unrelated', '/other/C1ancher',
                       '/usr/data/c1/core/releases/8-1.3.6/artifacts/C1ancher-other',
                       '/etc/c1updater/recovery-verifier-other'):
            with self.subTest(executable=target):
                result, events = self.run_case(executable=target)
                self.assert_stopped_before_write(result, events, 0)
                self.assertIn('0:service-stop', events)
                self.assertFalse(any(':term:' in event or event.endswith(':tick') for event in events))

    def test_already_stopped_chain_needs_no_delay_or_signal(self):
        result, events = self.run_case()
        self.assert_stopped_before_write(result, events, 0)
        self.assertEqual(events, ['0:verify-bundle', '0:service-stop', '0:mount-rw',
                                  '0:slot-write', '0:completed'])


if __name__ == '__main__':
    unittest.main()
