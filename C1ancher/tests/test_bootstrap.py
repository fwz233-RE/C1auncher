"""Isolated bootstrap fault matrix. No mount, deployment, rootfs or device access.
Production paths and the trusted owner are rewritten in a private script copy;
no runtime test bypass is added to the installed bootstrap. The current test
user stands in for root, while modes, links and hashes are checked unchanged.
Sleep records virtual seconds.
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
FAKE = r'''#!/usr/bin/env python3
from pathlib import Path
import json, os, sys, time
root = Path(os.environ['FIXTURE_ROOT'])
args = sys.argv[1:]
if args[0] == 'supervise':
    role = Path(sys.argv[0]).parent.name[-1]
elif args[0] == 'recovery-supervise':
    role = args[4]
elif args[0] == 'sleep':
    role = 'sleep'
else:
    role = args[0]
counts_file = root / 'counts'
counts = json.loads(counts_file.read_text()) if counts_file.exists() else {}
n = counts.get(role, 0)
counts[role] = n + 1
counts_file.write_text(json.dumps(counts))
with (root / 'events').open('a') as f:
    f.write(json.dumps([role, args]) + '\n')
config = json.loads((root / 'config').read_text())
values = config.get(role, [0])
value = values[min(n, len(values)-1)]
if value == 'wait':
    time.sleep(60)
    value = 0
sys.exit(value)
'''


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='c1-bootstrap-')
        self.root = Path(self.temp.name)
        self.env = dict(os.environ, FIXTURE_ROOT=str(self.root))
        self.slot_root = self.root / 'slots'
        for slot in ('a', 'b'):
            path = self.slot_root / f'slot-{slot}' / 'c1updater'
            path.parent.mkdir(parents=True)
            path.write_text(FAKE)
            path.chmod(0o700)
        self.helper = self.slot_root / 'recovery-verifier'
        self.helper.write_text(FAKE)
        self.helper.chmod(0o700)
        self.script = self.root / 'bootstrap.sh'
        text = (ROOT / 'scripts/app-daemon-bootstrap.sh').read_text()
        replacements = {
            'SLOT_ROOT=/etc/c1updater': f'SLOT_ROOT={self.slot_root}',
            'SLOT_FILE=/usr/data/c1/update/updater-slot': f'SLOT_FILE={self.root}/updater-slot',
            'RECOVERY_VERIFIER=/etc/c1updater/recovery-verifier': f'RECOVERY_VERIFIER={self.helper}',
            'UPDATE_ROOT=/usr/data/c1/update': f'UPDATE_ROOT={self.root}',
            'STATE_ROOT=/usr/data/c1/update/state': f'STATE_ROOT={self.root}/state',
            'CORE_ROOT=/usr/data/c1/core': f'CORE_ROOT={self.root}/core',
            'KEY=/etc/c1updater/core.ed25519.pub': f'KEY={self.root}/key',
            'READY_FILE=/usr/data/c1/update/ready': f'READY_FILE={self.root}/ready',
        }
        for source, destination in replacements.items():
            self.assertIn(source, text)
            text = text.replace(source, destination)
        # Fixtures are owned by the invoking user, not necessarily root. Only
        # the private copy uses that identity as its trusted owner. Assert all
        # production comparisons still require root; keep every other field.
        self.trusted_uid = self.slot_root.stat().st_uid
        self.trusted_gid = self.slot_root.stat().st_gid
        for metadata in ('700:0:0', '700:0:0:1', '600:0:0:1:65'):
            source = f'= {metadata} ]'
            self.assertEqual(text.count(source), 1)
            destination = metadata.replace(':0:0', f':{self.trusted_uid}:{self.trusted_gid}', 1)
            text = text.replace(source, f'= {destination} ]')
        overrides = f'''prepare_log() {{ return 1; }}
ensure_root_ro() {{ return 0; }}
remount_root_ro() {{ return 0; }}
remount_root_rw() {{ return 0; }}
sleep() {{ "{self.helper}" sleep "$@"; }}
'''
        text = text.replace('trap stop_bootstrap HUP INT TERM', overrides + '\ntrap stop_bootstrap HUP INT TERM')
        self.script.write_text(text)

    def tearDown(self):
        self.temp.cleanup()

    def events(self):
        return [json.loads(line) for line in (self.root / 'events').read_text().splitlines()]

    def run_case(self, config):
        (self.root / 'config').write_text(json.dumps(config))
        result = subprocess.run(['sh', str(self.script)], env=self.env,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return [role for role, _ in self.events()]

    def test_missing_active_does_not_require_pointer_writer(self):
        (self.slot_root / 'slot-a/c1updater').unlink()
        roles = self.run_case({'b': [0], 'activate-updater-slot': [1]})
        self.assertIn('b', roles)
        self.assertNotIn('previous', roles)

    def test_exec_failure_and_ordinary_crashes_are_bounded(self):
        for status in (1, 126, 127, 139):
            with self.subTest(status=status):
                for name in ('counts', 'events'):
                    (self.root / name).unlink(missing_ok=True)
                roles = self.run_case({'a': [status], 'b': [0]})
                self.assertEqual(roles.count('a'), 3)
                self.assertEqual(roles.count('b'), 1)

    def test_corrupt_executable_active_tries_backup(self):
        active = self.slot_root / 'slot-a/c1updater'
        active.write_bytes(b'\x7fELF' + b'\x00' * 60)
        active.chmod(0o700)
        roles = self.run_case({'b': [0]})
        self.assertIn('b', roles)
        self.assertEqual(roles.count('sleep'), 2)

    def test_nonexecutable_active_tries_good_backup(self):
        (self.slot_root / 'slot-a/c1updater').chmod(0o600)
        roles = self.run_case({'b': [0]})
        self.assertIn('b', roles)

    def test_backup_slot_switch_reenters_parent_loop(self):
        roles = self.run_case({'a': [71, 0], 'b': [72]})
        self.assertEqual([r for r in roles if r in ('a', 'b')], ['a', 'b', 'a'])
        self.assertIn('install-prepared-slot', roles)

    def test_busy_exceeds_network_budget_without_failover(self):
        roles = self.run_case({'a': [75] * 35 + [0]})
        self.assertNotIn('b', roles)
        self.assertNotIn('previous', roles)
        self.assertGreaterEqual(sum(int(args[1]) for role, args in self.events() if role == 'sleep'), 600)

    def test_backup_busy_is_not_fatal(self):
        roles = self.run_case({'a': [71], 'b': [75] * 12 + [0]})
        self.assertNotIn('previous', roles)
        self.assertEqual(roles.count('b'), 13)

    def test_both_fatal_use_independent_recovery(self):
        roles = self.run_case({'a': [71], 'b': [71], 'previous': [71], 'current': [0]})
        self.assertEqual([r for r in roles if r in ('a', 'b', 'previous', 'current')],
                         ['a', 'b', 'previous', 'current'])

    def test_generation_exit_codes_keep_bootstrap_parent(self):
        roles = self.run_case({'a': [71, 0], 'b': [71], 'previous': [75, 72]})
        self.assertEqual(roles.count('previous'), 2)
        self.assertIn('install-prepared-slot', roles)
        self.assertEqual(roles.count('a'), 2)

    def test_busy_install_does_not_exhaust_switch_budget(self):
        roles = self.run_case({'a': [72], 'b': [0], 'install-prepared-slot': [75] * 10 + [0]})
        self.assertEqual(roles.count('install-prepared-slot'), 11)
        self.assertNotIn('previous', roles)

    def test_normal_stop_cancels_busy_child(self):
        (self.root / 'config').write_text(json.dumps({'a': ['wait']}))
        process = subprocess.Popen(['sh', str(self.script)], env=self.env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 5
            while not (self.root / 'events').exists() and time.monotonic() < deadline:
                time.sleep(.02)
            process.terminate()
            self.assertEqual(process.wait(timeout=3), 0)
            self.assertNotIn('b', [role for role, _ in self.events()])
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    def initial_startup(self):
        self.slot_root.chmod(0o700)
        startup = self.slot_root / 'enrollment-startup'
        startup.write_text('#!/bin/sh\nprintf original > "' + str(self.root / 'startup-ran') + '"\n')
        startup.chmod(0o700)
        record = self.slot_root / 'enrollment-startup.sha256'
        record.write_text(hashlib.sha256(startup.read_bytes()).hexdigest() + '\n')
        record.chmod(0o600)
        return startup, record

    def test_initial_interruption_uses_only_approved_startup(self):
        self.initial_startup()
        result = subprocess.run(['sh', str(self.script)], env=self.env,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / 'startup-ran').read_text(), 'original')
        self.assertFalse((self.root / 'events').exists())

    def test_initial_startup_survives_partly_activated_pointers(self):
        self.initial_startup()
        (self.root / 'core').mkdir()
        (self.root / 'core/current').symlink_to('releases/not-yet-complete')
        result = subprocess.run(['sh', str(self.script)], env=self.env,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / 'startup-ran').exists())

    def test_completed_enrollment_never_returns_to_vendor(self):
        self.initial_startup()
        (self.root / 'enrolled.v1').write_text('completed')
        roles = self.run_case({'a': [0]})
        # The fake helper records this probe; the real logging helper handles
        # it before event recording. Require the same startup path in both.
        self.assertEqual([role for role in roles if role != '--log-version'], ['a'])
        self.assertFalse((self.root / 'startup-ran').exists())

    def test_dangling_marker_disables_initial_startup(self):
        self.initial_startup()
        (self.root / 'enrolled.v1').symlink_to('missing-marker')
        self.run_case({'a': [0]})
        self.assertFalse((self.root / 'startup-ran').exists())

    def test_corrupt_or_unsafe_initial_startup_fails_closed(self):
        for damage in ('bytes', 'mode', 'record-mode', 'record-bytes', 'directory-mode',
                       'symlink', 'hardlink', 'record-symlink', 'record-newline'):
            with self.subTest(damage=damage):
                for name in ('enrollment-startup', 'enrollment-startup.sha256', 'other'):
                    (self.slot_root / name).unlink(missing_ok=True)
                startup, record = self.initial_startup()
                if damage == 'bytes':
                    startup.write_text('#!/bin/sh\nexit 0\n')
                elif damage == 'mode':
                    startup.chmod(0o777)
                elif damage == 'record-mode':
                    record.chmod(0o666)
                elif damage == 'record-bytes':
                    record.write_text('x' * 64 + '\n')
                elif damage == 'directory-mode':
                    self.slot_root.chmod(0o777)
                elif damage == 'symlink':
                    other = self.slot_root / 'other'
                    startup.rename(other)
                    startup.symlink_to(other)
                elif damage == 'hardlink':
                    os.link(startup, self.slot_root / 'other')
                elif damage == 'record-symlink':
                    other = self.slot_root / 'other'
                    record.rename(other)
                    record.symlink_to(other)
                else:
                    record.write_text(record.read_text().rstrip('\n') + 'x')
                result = subprocess.run(['sh', str(self.script)], env=self.env,
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 71, result.stderr)
                self.assertFalse((self.root / 'startup-ran').exists())
                self.assertFalse((self.root / 'events').exists())

    def test_initial_startup_rejects_untrusted_owner_and_group(self):
        startup, record = self.initial_startup()
        original = self.script.read_text()
        for entry, suffix in ((self.slot_root, ''), (startup, ':%h'), (record, ':%h:%s')):
            for identity in ('owner', 'group'):
                with self.subTest(entry=entry.name, identity=identity):
                    uid = self.trusted_uid + (identity == 'owner')
                    gid = self.trusted_gid + (identity == 'group')
                    # Inject only the stat ownership fields for one entry;
                    # real modes/link counts/sizes and all other checks remain.
                    # This exercises rejection without requiring chown privileges.
                    override = f'''stat() {{
    if [ "$3" = "{entry}" ]; then
        command stat -c '%a:{uid}:{gid}{suffix}' "$3"
    else
        command stat "$@"
    fi
}}
'''
                    self.script.write_text(original.replace(
                        'trap stop_bootstrap HUP INT TERM',
                        override + '\ntrap stop_bootstrap HUP INT TERM'))
                    result = subprocess.run(['sh', str(self.script)], env=self.env,
                                            capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 71, result.stderr)
                    self.assertFalse((self.root / 'startup-ran').exists())
                    self.assertFalse((self.root / 'events').exists())

    def test_initial_startup_requires_read_only_root(self):
        self.initial_startup()
        self.script.write_text(self.script.read_text().replace(
            'ensure_root_ro() { return 0; }', 'ensure_root_ro() { return 1; }'))
        result = subprocess.run(['sh', str(self.script)], env=self.env,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 71, result.stderr)
        self.assertFalse((self.root / 'startup-ran').exists())

    def test_installed_bootstrap_never_execs_generation_directly(self):
        text = (ROOT / 'scripts/app-daemon-bootstrap.sh').read_text()
        self.assertNotIn('exec "$recovery_updater"', text)
        self.assertIn('"$RECOVERY_VERIFIER" recovery-supervise', text)


if __name__ == '__main__':
    unittest.main()
