"""Enrollment ordering/restart integration, run as root on Linux (WSL is OK).

Runs private copies of the production scripts, including the entire apply body
and its real hash/metadata checks. Device paths are rewritten below /fixture,
inside a second, private chroot boundary. No host mount, service, ADB, device,
network, or global sync command can run. Only host shell/coreutils executables
and their shared libraries are copied into the disposable root.

The updater is an explicit protocol/state stub: signature, manifest and MIPS
execution are NOT cryptographic proof. It enforces the actual installed script
hash/capability/minimum-version gate and models prepared/pending/confirmed state.
Independent native-verifier and signed-bundle tests cover cryptography.
"""
from pathlib import Path
import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

UPDATER = r'''#!/bin/sh
set -eu
f=/fixture
slot=$f/etc/c1updater
core=$f/usr/data/c1/core
state=$f/usr/data/c1/update/state/phase
marker=$f/usr/data/c1/update/enrolled.v1
event() { echo "$*" >>$f/events; }
reject() { echo "stub rejected: $*" >&2; exit 43; }
mode() { grep -q ' / .* ro,' $f/proc/mounts; }
case "$1" in
    --self-test) event "self-test:$0"; exit 0 ;;
    --recovery-version) event "recovery-version:$0"; echo 'C1RECOVERY-VERIFIER 1.1.0'; exit 0 ;;
    --log-version) exit 1 ;;
    verify-signature|verify-manifest) event "$1:$0"; exit 0 ;;
    state)
        if [ -f $f/override-state ]; then cat $f/override-state; else
            echo "state: generation=3 phase=$(cat "$state" 2>/dev/null || echo idle) sequence=17 security_epoch=4 release=1.2.3"
        fi
        exit 0
        ;;
    prepare-local)
        event prepare
        mode || reject 'prepare requires read-only root'
        [ -f "$slot/bootstrap.version" ] || reject 'missing bootstrap capability'
        read -r version bound <$slot/bootstrap.version
        actual=$(sha256sum $f/etc/app_daemon); actual=${actual%% *}
        expected=$(cat $f/expected-bootstrap)
        [ "$actual" = "$bound" ] && [ "$actual" = "$expected" ] || reject 'installed bootstrap hash binding'
        minimum=$(sed -n 's/^B\t//p' "$2/manifest.v1")
        [ "$version" = 1.1.0 ] && [ "$minimum" = 1.1.0 ] || reject 'minimum bootstrap version'
        [ "$(cat "$state" 2>/dev/null || echo idle)" != pending-boot ] || reject 'prepare during pending activation'
        [ ! -f $f/fail-prepare ] || reject 'injected prepare failure'
        mkdir -p "$core/releases"
        rm -rf "$core/releases/17-1.2.3"
        cp -R "$2" "$core/releases/17-1.2.3"
        chmod 700 "$core/releases/17-1.2.3/artifacts/"*
        echo prepared >"$state"
        event prepared
        ;;
    activate-updater-slot)
        echo "$3" >"$2/updater-slot"
        event slot-pointer
        ;;
    bootstrap-activate)
        mode || reject 'activation requires read-only root'
        phase=$(cat "$state")
        case "$phase" in prepared|pending-boot) ;; *) reject 'activation without prepare';; esac
        event "activate:$phase"
        for name in current previous; do
            [ -L "$core/$name" ] || ln -s releases/17-1.2.3 "$core/$name"
        done
        echo pending-boot >"$state"
        if [ -f $f/interrupt-activation ]; then
            event activation-interrupted
            # Terminate the actual production apply shell at the pending boundary.
            kill -KILL "$PPID"
            exit 99
        fi
        echo confirmed >"$state"
        event confirmed
        ;;
    verify-current)
        [ "$(cat "$state")" = confirmed ] || reject 'current not confirmed'
        [ -L "$core/current" ] && [ -L "$core/previous" ] || reject 'missing pointers'
        event verify-current
        [ ! -f $f/fail-verify-current ] || reject 'injected verify-current failure'
        ;;
    supervise|recovery-supervise)
        [ -f "$marker" ] || reject 'signed service before enrollment'
        event signed-startup
        ;;
    *) reject "unsupported command $1" ;;
esac
'''

MOUNT = r'''#!/bin/sh
set -eu
[ "$#" = 3 ] && [ "$1" = -o ] && [ "$3" = / ] || exit 90
case "$2" in remount,rw) mode=rw;; remount,ro) mode=ro;; *) exit 90;; esac
echo "fake / fake $mode,relatime 0 0" >/fixture/proc/mounts
echo "mount:$mode" >>/fixture/events
'''

BUSYBOX = r'''#!/bin/sh
set -eu
[ "$*" = 'start-stop-daemon -S -b -x /fixture/etc/app_daemon' ] || exit 90
echo restart >>/fixture/events
# Synchronous execution makes the restarted real bootstrap observable.
exec /fixture/etc/app_daemon
'''

SYNC = r'''#!/bin/sh
set -eu
slot=/fixture/etc/c1updater
if [ -x "$slot/recovery-verifier" ] && [ ! -e "$slot/core.ed25519.pub" ]; then
    echo sync:verifier-before-key >>/fixture/events
fi
if [ -f /fixture/usr/data/c1/update/enrolled.v1 ]; then
    echo sync:marker >>/fixture/events
else
    echo sync >>/fixture/events
    if [ -f /fixture/etc/c1updater/enrollment-startup.sha256 ]; then
        original=$(cat /fixture/etc/c1updater/enrollment-startup.sha256)
        installed=$(sha256sum /fixture/etc/app_daemon)
        if [ "${installed%% *}" = "$original" ]; then
            echo sync:startup-before-replace >>/fixture/events
        fi
    fi
fi
'''

RM = r'''#!/bin/sh
set -eu
for argument in "$@"; do
    case "$argument" in
        /fixture/etc/c1updater/enrollment-startup|/fixture/etc/c1updater/enrollment-startup.sha256)
            echo cleanup-startup >>/fixture/events
            [ ! -f /fixture/fail-cleanup ] || exit 44
            break
            ;;
    esac
done
exec /bin/real-rm "$@"
'''


MV = r'''#!/bin/sh
set -eu
/bin/real-mv "$@"
for argument in "$@"; do
    if [ "$argument" = /fixture/etc/c1updater/core.ed25519.pub ]; then
        echo key-committed >>/fixture/events
        if [ -f /fixture/interrupt-key ]; then
            # The only real signal here targets this wrapper's own apply parent.
            kill -KILL "$PPID"
            exit 99
        fi
    fi
done
'''

FAKE_KILL = r'''#!/bin/sh
set -eu
[ "$#" = 2 ] && [ "$1" = -TERM ] || exit 90
case "$2" in ''|*[!0-9]*) exit 90;; esac
[ -f "/fixture/proc/$2/cmdline" ] || exit 90
echo "term:$2" >>/fixture/events
: >"/fixture/proc/$2/terminated"
'''

SLEEP = r'''#!/bin/sh
set -eu
echo "sleep:$*" >>/fixture/events
[ ! -f /fixture/hold-startup ] || exit 0
for entry in /fixture/proc/[0-9]*/terminated; do
    [ -f "$entry" ] || continue
    echo "reaped:${entry%/terminated}" >>/fixture/events
    rm -rf "${entry%/terminated}"
done
'''

SERVICE = r'''#!/bin/sh
set -eu
[ "$*" = stop ] || exit 90
echo stop >>/fixture/events
for executable in /fixture/proc/[0-9]*/exe; do
    [ -L "$executable" ] || continue
    if [ "$(readlink "$executable")" = /fixture/usr/bin/d261/mpenMain ]; then
        rm -rf "${executable%/exe}"
        echo vendor-stopped >>/fixture/events
    fi
done
'''


@unittest.skipUnless(os.name == 'posix' and hasattr(os, 'geteuid') and os.geteuid() == 0,
                     'requires Linux root for a private chroot; use WSL Ubuntu-22.04 -u root')
class EnrollmentOrderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.chroot = shutil.which('chroot')
        if not cls.chroot:
            raise unittest.SkipTest('chroot is required; there is no unisolated fallback')
        cls.runtime_tmp = tempfile.TemporaryDirectory(prefix='c1-enrollment-runtime-')
        cls.runtime = Path(cls.runtime_tmp.name)
        cls.addClassCleanup(cls.runtime_tmp.cleanup)
        # Fixed allowlist: notably excludes mount, busybox, service and sync.
        commands = ('sh', 'cat', 'chmod', 'chown', 'cp', 'find', 'grep', 'ln',
                    'ls', 'mkdir', 'mv', 'readlink', 'rm', 'sed', 'sha256sum',
                    'stat', 'wc', 'tr')
        for command in commands:
            source = shutil.which(command)
            if source is None:
                raise unittest.SkipTest(f'missing host utility: {command}')
            target = cls.runtime / 'bin' / ('real-' + command if command in ('rm', 'mv') else command)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            result = subprocess.run(['ldd', source], capture_output=True, text=True, check=True)
            for library in re.findall(r'(?:=>\s+)?(/[^\s()]+)', result.stdout):
                library_path = Path(library)
                destination = cls.runtime / library_path.relative_to('/')
                destination.parent.mkdir(parents=True, exist_ok=True)
                if not destination.exists():
                    shutil.copy2(library_path, destination)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='c1-enrollment-order-')
        self.addCleanup(self.temp.cleanup)
        self.jail = Path(self.temp.name)
        shutil.copytree(self.runtime, self.jail, dirs_exist_ok=True)
        self.root = self.jail / 'fixture'
        for name in ('etc/init.d', 'usr/data/c1/bin', 'storage', 'dev/shm', 'proc',
                     'bundle/release/artifacts'):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        (self.jail / 'dev').mkdir()
        # A regular private file suffices for redirections; no host device node.
        (self.jail / 'dev/null').touch()
        self.write('proc/mounts', 'fake / fake ro,relatime 0 0\n')
        self.write('proc/uptime', '100.0 100.0\n')
        self.write('events', '')
        for name, text in [('mount', MOUNT), ('busybox', BUSYBOX), ('sync', SYNC),
                           ('rm', RM), ('mv', MV), ('fake-kill', FAKE_KILL), ('sleep', SLEEP)]:
            self.write_jail('bin/' + name, text, 0o700)
        self.write('etc/init.d/S80app', SERVICE, 0o700)
        self.original = '#!/bin/sh\necho approved-startup >>/fixture/events\n'
        self.write('etc/app_daemon', self.original, 0o755)
        self.write('usr/data/c1/bin/app_daemon', '#!/bin/sh\nexit 0\n', 0o700)
        self.enrollment = self.rewrite((ROOT / 'scripts/device-core-enroll.sh').read_text())
        self.bootstrap = self.rewrite((ROOT / 'scripts/app-daemon-bootstrap.sh').read_text())
        self.write('enroll.sh', self.enrollment, 0o700)
        self.write('bundle/app-daemon-bootstrap.sh', self.bootstrap)
        self.write('bundle/device-core-enroll.sh', self.enrollment)
        self.write('bundle/enroll.sh', '# fixture host entry; never executed\n')
        self.write('bundle/core.ed25519.pub', 'k' * 32)
        self.write('bundle/core.ed25519.pem', 'test-only signature protocol stub\n')
        self.write('bundle/bootstrap.v1.sig', 's' * 64)
        self.write('bundle/release/manifest.v1.sig', 's' * 64)
        self.write('bundle/release/manifest.v1',
                   'C1CORE-MANIFEST 1\nS\t17\nV\t1.2.3\nE\t4\n'
                   'T\tmips32r2-little-o32-hard-float-double-static\n'
                   'B\t1.1.0\nU\t1.1.0\nC\tc1-core-v1\nR\tfixture\nD\t1\n')
        for name in ('C1ancher', 'c1pkg', 'C1ancher-launcher'):
            self.write('bundle/release/artifacts/' + name, '#!/bin/sh\nexit 0\n')
        self.write('bundle/release/artifacts/c1updater', UPDATER)
        manifest = self.root / 'bundle/release/manifest.v1'
        text = manifest.read_text()
        for role, name in (('c1ancher', 'C1ancher'), ('c1pkg', 'c1pkg'),
                           ('launcher', 'C1ancher-launcher'), ('updater', 'c1updater')):
            payload = (self.root / 'bundle/release/artifacts' / name).read_bytes()
            text += (f'F\t{role}\tartifacts/{name}\t{hashlib.sha256(payload).hexdigest()}'
                     f'\t{len(payload)}\t700\n')
        manifest.write_text(text)
        self.write('expected-bootstrap', self.digest(self.bootstrap) + '\n')
        self.refresh_manifest()

    @staticmethod
    def digest(text):
        return hashlib.sha256(text.encode()).hexdigest()

    def write_jail(self, relative, text, mode=0o600):
        path = self.jail / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        path.chmod(mode)
        return path

    def write(self, relative, text, mode=0o600):
        return self.write_jail('fixture/' + relative, text, mode)

    def rewrite(self, text):
        # One regex pass avoids accidental repeated-prefix replacements. The
        # sole remaining /proc-like access is in these rewritten private paths.
        text = re.sub(r'/(?:etc|usr/data|usr/bin/d261|storage|dev/shm|proc)(?=/|\b)',
                      lambda match: '/fixture' + match.group(), text)
        # chroot does not isolate PIDs: never let synthetic /proc entries reach
        # the shell's real kill builtin. Only the exact production stop call is
        # redirected; the test interruption wrappers signal their own parent.
        text = text.replace('kill -TERM "$process"', '/bin/fake-kill -TERM "$process"')
        return text

    def refresh_manifest(self):
        fields = [('K', 'core.ed25519.pub'), ('P', 'core.ed25519.pem'),
                  ('B', 'app-daemon-bootstrap.sh'), ('D', 'device-core-enroll.sh'),
                  ('L', 'enroll.sh'), ('M', 'release/manifest.v1'),
                  ('G', 'release/manifest.v1.sig'), ('U', 'release/artifacts/c1updater')]
        text = 'C1CORE-BOOTSTRAP 1\nV\t1.1.0\n'
        for tag, name in fields:
            digest = hashlib.sha256((self.root / 'bundle' / name).read_bytes()).hexdigest()
            text += f'{tag}\t{digest}\n'
        text += 'H\t' + self.digest(self.original) + '\n'
        self.write('bundle/bootstrap.v1', text)

    def run_script(self, action='apply', script='/fixture/enroll.sh'):
        args = [self.chroot, str(self.jail), '/bin/sh', script]
        if action is not None:
            args.extend([action, '/fixture/bundle'])
        # No shell=True and no inherited PATH, preload, shell startup or device
        # configuration. chroot is mandatory even though paths are rewritten.
        return subprocess.run(args, env={'PATH': '/bin', 'LC_ALL': 'C'},
                              cwd=self.jail, capture_output=True, text=True, timeout=20)

    def events(self):
        return (self.root / 'events').read_text().splitlines()

    def assert_ro(self):
        self.assertIn(' ro,', (self.root / 'proc/mounts').read_text())

    @property
    def marker(self):
        return self.root / 'usr/data/c1/update/enrolled.v1'

    @property
    def recovery(self):
        return self.root / 'etc/c1updater/enrollment-startup'

    def assert_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('core enrollment completed', result.stdout)
        self.assertTrue(self.marker.is_file())
        self.assertFalse(self.recovery.exists())
        self.assertFalse(self.recovery.with_suffix('.sha256').exists())
        self.assert_ro()

    def fail_prepare(self):
        self.write('fail-prepare', '')
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('injected prepare failure', result.stderr)
        return result

    def test_installs_real_bootstrap_and_capability_before_prepare(self):
        self.assert_success(self.run_script())
        events = self.events()
        self.assertLess(events.index('sync:startup-before-replace'), events.index('mount:ro'))
        self.assertLess(events.index('mount:ro'), events.index('prepare'))
        self.assertLess(events.index('prepared'), events.index('confirmed'))
        self.assertLess(events.index('confirmed'), events.index('verify-current'))
        self.assertLess(events.index('verify-current'), events.index('sync:marker'))
        self.assertLess(events.index('sync:marker'), events.index('cleanup-startup'))
        self.assertLess(events.index('cleanup-startup'), events.index('signed-startup'))
        self.assertEqual((self.root / 'etc/app_daemon').read_text(), self.bootstrap)
        capability = (self.root / 'etc/c1updater/bootstrap.version').read_text()
        self.assertEqual(capability, f'1.1.0 {self.digest(self.bootstrap)}\n')
        verified = self.run_script('verify')
        self.assertEqual(verified.returncode, 0, verified.stderr)
        self.assertIn('core enrollment verified', verified.stdout)

    def old_order_mutant(self):
        # Execute a negative-control copy, not merely a source-string assertion.
        prepare = ('        "$verifier" prepare-local "$bundle/release" "$staging_root" '
                   '"$core_root" "$state_root" \\\n            "$bundle/core.ed25519.pub" >/dev/null\n')
        self.assertEqual(self.enrollment.count(prepare), 1)
        mutant = self.enrollment.replace(prepare, '        : # prepare moved to reproduce the old ordering\n')
        anchor = '    # Compatibility is checked against the ACTUAL installed bootstrap below,'
        self.assertIn(anchor, mutant)
        return mutant.replace(anchor, prepare + anchor, 1)

    def test_old_ordering_mutation_is_rejected(self):
        self.write('old-order.sh', self.old_order_mutant(), 0o700)
        result = self.run_script(script='/fixture/old-order.sh')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing bootstrap capability', result.stderr)
        self.assertEqual((self.root / 'etc/app_daemon').read_text(), self.original)
        self.assertFalse(self.marker.exists())
        self.assert_ro()

    def test_spoofed_new_capability_cannot_authorize_the_old_startup(self):
        self.write('old-order.sh', self.old_order_mutant(), 0o700)
        self.write('etc/c1updater/bootstrap.version',
                   f'1.1.0 {self.digest(self.original)}\n')
        result = self.run_script(script='/fixture/old-order.sh')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('installed bootstrap hash binding', result.stderr)
        self.assertFalse(self.marker.exists())
        self.assertNotIn('prepared', self.events())
        self.assert_ro()

    def test_higher_release_minimum_is_rejected(self):
        manifest = self.root / 'bundle/release/manifest.v1'
        manifest.write_text(manifest.read_text().replace('B\t1.1.0\n', 'B\t1.2.0\n'))
        self.refresh_manifest()
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('minimum bootstrap version', result.stderr)
        self.assertFalse(self.marker.exists())
        self.assertIn('approved-startup', self.events())
        self.assert_ro()

    def test_prepare_failure_retains_protected_approved_startup_and_read_only_root(self):
        self.fail_prepare()
        self.assertFalse(self.marker.exists())
        self.assertEqual(self.recovery.read_text(), self.original)
        self.assertEqual(self.recovery.stat().st_mode & 0o777, 0o700)
        record = self.recovery.with_suffix('.sha256')
        self.assertEqual(record.read_text(), self.digest(self.original) + '\n')
        self.assertEqual(record.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.recovery.stat().st_uid, 0)
        self.assertEqual(self.recovery.stat().st_nlink, 1)
        self.assertEqual((self.root / 'usr/data/c1/bin/app_daemon').read_text(), '#!/bin/sh\nexit 0\n')
        self.assertIn('approved-startup', self.events())
        self.assertNotIn('confirmed', self.events())
        self.assert_ro()

    def test_restart_then_retry_after_prepare_failure(self):
        self.fail_prepare()
        restarted = self.run_script(None, '/fixture/etc/app_daemon')
        self.assertEqual(restarted.returncode, 0, restarted.stderr)
        self.assertEqual(self.events().count('approved-startup'), 2)
        (self.root / 'fail-prepare').unlink()
        self.assert_success(self.run_script())
        self.assertEqual(self.events().count('prepare'), 2)
        self.assertEqual(self.events().count('confirmed'), 1)

    def test_tampered_startup_rejected_on_boot_and_retry(self):
        self.fail_prepare()
        self.recovery.write_text('#!/bin/sh\necho tampered-executed >>/fixture/events\n')
        restarted = self.run_script(None, '/fixture/etc/app_daemon')
        self.assertEqual(restarted.returncode, 71, restarted.stderr)
        retry = self.run_script()
        self.assertNotEqual(retry.returncode, 0)
        self.assertIn('file digest rejected', retry.stderr)
        self.assertNotIn('tampered-executed', self.events())
        self.assertFalse(self.marker.exists())
        self.assert_ro()

    def test_recovery_metadata_and_hash_record_rejected(self):
        self.fail_prepare()
        record = self.recovery.with_suffix('.sha256')
        original_record = record.read_text()
        cases = ('script-mode', 'record-mode', 'directory-mode', 'record-digest',
                 'script-hardlink', 'script-symlink', 'record-symlink')
        for case in cases:
            with self.subTest(case=case):
                self.recovery.unlink()
                self.write('etc/c1updater/enrollment-startup', self.original, 0o700)
                record.unlink()
                self.write('etc/c1updater/enrollment-startup.sha256', original_record, 0o600)
                self.recovery.parent.chmod(0o700)
                extra = self.root / 'extra-link'
                extra.unlink(missing_ok=True)
                if case == 'script-mode':
                    self.recovery.chmod(0o755)
                elif case == 'record-mode':
                    record.chmod(0o644)
                elif case == 'directory-mode':
                    self.recovery.parent.chmod(0o755)
                elif case == 'record-digest':
                    record.write_text('0' * 64 + '\n')
                elif case == 'script-hardlink':
                    os.link(self.recovery, extra)
                elif case == 'script-symlink':
                    self.recovery.rename(extra)
                    self.recovery.symlink_to('/fixture/extra-link')
                elif case == 'record-symlink':
                    record.rename(extra)
                    record.symlink_to('/fixture/extra-link')
                before = self.events().count('approved-startup')
                result = self.run_script(None, '/fixture/etc/app_daemon')
                self.assertEqual(result.returncode, 71, result.stderr)
                self.assertNotEqual(self.run_script().returncode, 0)
                self.assertEqual(self.events().count('approved-startup'), before)
                self.assertFalse(self.marker.exists())
                self.assert_ro()

    def test_pending_activation_interruption_resumes_without_prepare(self):
        self.write('interrupt-activation', '')
        interrupted = self.run_script()
        self.assertNotEqual(interrupted.returncode, 0)
        self.assertIn('activation-interrupted', self.events())
        self.assertFalse(self.marker.exists())
        self.assertTrue((self.root / 'usr/data/c1/core/current').is_symlink())
        self.assert_ro()
        restarted = self.run_script(None, '/fixture/etc/app_daemon')
        self.assertEqual(restarted.returncode, 0, restarted.stderr)
        self.assertIn('approved-startup', self.events())
        (self.root / 'interrupt-activation').unlink()
        self.assert_success(self.run_script())
        self.assertEqual(self.events().count('prepare'), 1)
        self.assertIn('activate:pending-boot', self.events())

    def test_verify_current_failure_cannot_commit_marker_or_remove_startup(self):
        self.write('fail-verify-current', '')
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('injected verify-current failure', result.stderr)
        self.assertFalse(self.marker.exists())
        self.assertTrue(self.recovery.is_file())
        self.assertNotIn('cleanup-startup', self.events())
        self.assertIn('approved-startup', self.events())
        self.assert_ro()
        (self.root / 'fail-verify-current').unlink()
        self.assert_success(self.run_script())
        self.assertEqual(self.events().count('prepare'), 1)

    def test_cleanup_failure_blocks_success_and_verification_until_retry(self):
        self.write('fail-cleanup', '')
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('core enrollment completed', result.stdout)
        self.assertTrue(self.marker.is_file())
        self.assertTrue(self.recovery.is_file())
        self.assert_ro()
        # A durable marker disables fallback even while cleanup is incomplete.
        restarted = self.run_script(None, '/fixture/etc/app_daemon')
        self.assertEqual(restarted.returncode, 0, restarted.stderr)
        self.assertNotIn('approved-startup', self.events())
        verified = self.run_script('verify')
        self.assertNotEqual(verified.returncode, 0,
                            'verify must reject leftover temporary startup authorization')
        self.assertNotIn('core enrollment verified', verified.stdout)
        (self.root / 'fail-cleanup').unlink()
        self.assert_success(self.run_script())
        self.assertEqual(self.run_script('verify').returncode, 0)
        # Historical backups are deliberately retained; only temporary files go.
        for prefix in ('usr/data', 'storage'):
            backup = self.root / prefix / 'c1/recovery/core-enrollment/app_daemon.pre-enrollment'
            self.assertEqual(backup.read_text(), self.original)

    def root_snapshot(self):
        """Include content, links, ownership and inode changes, excluding reads."""
        result = {}
        for prefix in ('etc', 'usr/data', 'storage'):
            base = self.root / prefix
            for path in [base, *base.rglob('*')]:
                stat = path.lstat()
                content = (os.readlink(path) if path.is_symlink() else
                           path.read_bytes() if path.is_file() else None)
                result[str(path.relative_to(self.root))] = (
                    stat.st_mode, stat.st_uid, stat.st_gid, stat.st_ino,
                    stat.st_mtime_ns, content)
        return result

    def assert_rejected_without_root_mutations(self):
        before = self.root_snapshot()
        event_count = len(self.events())
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.root_snapshot(), before)
        fresh = self.events()[event_count:]
        for forbidden in ('mount:rw', 'stop', 'prepare', 'key-committed', 'cleanup-startup'):
            self.assertNotIn(forbidden, fresh)
        self.assert_ro()
        return result

    def pending_fixture(self):
        self.write('interrupt-activation', '')
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('activation-interrupted', self.events())
        (self.root / 'interrupt-activation').unlink()
        self.assertFalse(self.marker.exists())

    def confirmed_fixture(self):
        self.write('fail-verify-current', '')
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('injected verify-current failure', result.stderr)
        (self.root / 'fail-verify-current').unlink()
        self.assertFalse(self.marker.exists())

    def mismatched_identity_cases(self, phase):
        original = (f'state: generation=3 phase={phase} sequence=17 '
                    'security_epoch=4 release=1.2.3\n')
        for old, new in (('sequence=17', 'sequence=18'),
                         ('security_epoch=4', 'security_epoch=5'),
                         ('release=1.2.3', 'release=1.2.4')):
            with self.subTest(phase=phase, field=old):
                self.write('override-state', original.replace(old, new))
                result = self.assert_rejected_without_root_mutations()
                self.assertIn('activation identity differs', result.stderr)
        (self.root / 'override-state').unlink()

    def test_pending_identity_must_match_signed_candidate_without_root_writes(self):
        self.pending_fixture()
        self.mismatched_identity_cases('pending-boot')
        self.assert_success(self.run_script())

    def test_confirmed_identity_must_match_signed_candidate_without_root_writes(self):
        self.confirmed_fixture()
        self.mismatched_identity_cases('confirmed')
        self.assert_success(self.run_script())

    def malformed_pointer_cases(self):
        core = self.root / 'usr/data/c1/core'
        for pointer in ('current', 'previous'):
            for target in ('releases/99-9.9.9', './releases/17-1.2.3',
                           '/fixture/usr/data/c1/core/releases/17-1.2.3',
                           '../outside', 'regular-file', 'directory'):
                with self.subTest(pointer=pointer, target=target):
                    path = core / pointer
                    path.unlink()
                    if target == 'regular-file':
                        path.write_text('not a generation pointer\n')
                    elif target == 'directory':
                        path.mkdir()
                    else:
                        path.symlink_to(target)
                    result = self.assert_rejected_without_root_mutations()
                    self.assertRegex(result.stderr, 'activation pointer identity rejected|existing enrollment requires')
                    if path.is_dir() and not path.is_symlink():
                        path.rmdir()
                    else:
                        path.unlink()
                    path.symlink_to('releases/17-1.2.3')

    def test_pending_malformed_pointers_rejected_before_root_mutations(self):
        self.pending_fixture()
        self.malformed_pointer_cases()
        self.assert_success(self.run_script())

    def test_confirmed_malformed_pointers_rejected_before_root_mutations(self):
        self.confirmed_fixture()
        self.malformed_pointer_cases()
        self.assert_success(self.run_script())

    def test_key_commit_interruption_has_durable_protected_verifier_and_retries(self):
        self.write('interrupt-key', '')
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('core enrollment completed', result.stdout)
        slot = self.root / 'etc/c1updater'
        protected = slot / 'recovery-verifier'
        expected = (self.root / 'bundle/release/artifacts/c1updater').read_bytes()
        self.assertEqual(protected.read_bytes(), expected)
        self.assertEqual(protected.stat().st_mode & 0o777, 0o700)
        events = self.events()
        durable = events.index('sync:verifier-before-key')
        self.assertLess(durable, events.index('key-committed'))
        for name in ('slot-a', 'slot-b'):
            self.assertEqual((slot / name / 'c1updater').read_bytes(), expected)
            self.assertLess(events.index(f'self-test:/fixture/etc/c1updater/{name}/c1updater'), durable)
        self.assertLess(events.index('recovery-version:/fixture/etc/c1updater/recovery-verifier'), durable)
        self.assertTrue((slot / 'core.ed25519.pub').is_file())
        self.assertFalse(self.marker.exists())
        self.assertFalse(self.recovery.exists())
        self.assertEqual((self.root / 'etc/app_daemon').read_text(), self.original)
        # SIGKILL models lost power, so EXIT cleanup cannot run. A new boot
        # mounts root read-only before executing the still-original startup.
        self.write('proc/mounts', 'fake / fake ro,relatime 0 0\n')
        self.assertEqual(self.run_script(None, '/fixture/etc/app_daemon').returncode, 0)
        (self.root / 'interrupt-key').unlink()
        before = len(self.events())
        self.assert_success(self.run_script())
        fresh = self.events()[before:]
        protected_check = 'verify-signature:/fixture/etc/c1updater/recovery-verifier'
        self.assertIn(protected_check, fresh)
        self.assertLess(fresh.index(protected_check), fresh.index('stop'))

    def proc_entry(self, pid, arguments, executable='/bin/sh'):
        entry = self.root / 'proc' / str(pid)
        entry.mkdir()
        (entry / 'cmdline').write_bytes(b'\0'.join(arg.encode() for arg in arguments) + b'\0')
        (entry / 'exe').symlink_to(executable)

    def function_probe(self, commands):
        # Keep real function definitions and traps; replace only CLI dispatch.
        prefix, separator, _ = self.enrollment.partition('action=${1:-}')
        self.assertTrue(separator)
        self.write('probe.sh', prefix + commands + '\ntrap - 0 1 2 15\n', 0o700)
        return self.run_script(None, '/fixture/probe.sh')

    def populate_startup_processes(self):
        accepted = {
            41001: ['/bin/sh', '/fixture/etc/app_daemon'],
            41002: ['/fixture/etc/app_daemon'],
            41003: ['/bin/sh', '/fixture/etc/c1updater/enrollment-startup'],
            41004: ['/fixture/etc/c1updater/enrollment-startup'],
        }
        unrelated = {
            42001: ['/bin/sh', '/fixture/etc/app_daemon', 'extra'],
            42002: ['/bin/sh', '-c', '/fixture/etc/app_daemon'],
            42003: ['/bin/sh', '/fixture/etc/c1updater/enrollment-startup', 'extra'],
            42004: ['/bin/sh', '/fixture/enroll.sh', 'apply', '/fixture/bundle'],
            42005: ['/bin/sh', '/fixture/etc/app_daemon-other'],
            42006: ['/bin/sh'],
        }
        for pid, arguments in {**accepted, **unrelated}.items():
            self.proc_entry(pid, arguments)
        return accepted, unrelated

    def test_private_proc_matches_only_exact_startup_commands_and_stops_before_service(self):
        accepted, unrelated = self.populate_startup_processes()
        probe = self.function_probe('startup_script_pids')
        self.assertEqual(probe.returncode, 0, probe.stderr)
        self.assertEqual(set(map(int, probe.stdout.split())), set(accepted))
        self.proc_entry(43001, ['/fixture/usr/bin/d261/mpenMain'], '/fixture/usr/bin/d261/mpenMain')
        self.assert_success(self.run_script())
        events = self.events()
        stop = events.index('stop')
        first_sleep = events.index('sleep:1')
        for pid in accepted:
            self.assertLess(events.index(f'term:{pid}'), first_sleep)
            self.assertLess(events.index(f'reaped:/fixture/proc/{pid}'), stop)
        for pid in unrelated:
            self.assertNotIn(f'term:{pid}', events)
            self.assertTrue((self.root / 'proc' / str(pid)).is_dir())
        self.assertGreater(events.index('vendor-stopped'), stop)
        self.assertLess(events.index('vendor-stopped'), events.index('mount:rw'))

    def test_vendor_executable_is_detected_without_a_startup_shell(self):
        self.proc_entry(43001, ['/fixture/usr/bin/d261/mpenMain'], '/fixture/usr/bin/d261/mpenMain')
        result = self.function_probe('if app_daemon_running; then echo detected; else exit 91; fi')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), 'detected')

    def test_unstopped_startup_fails_before_service_or_root_mutation(self):
        accepted, _ = self.populate_startup_processes()
        self.write('hold-startup', '')
        before = {key: value for key, value in self.root_snapshot().items()
                  if key == 'etc' or key.startswith('etc/')}
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        after = {key: value for key, value in self.root_snapshot().items()
                 if key == 'etc' or key.startswith('etc/')}
        # Data/storage backups legitimately precede stop_chain; the protected
        # root filesystem must remain unchanged and must never be remounted rw.
        self.assertEqual(after, before)
        self.assertNotIn('mount:rw', self.events())
        self.assert_ro()
        self.assertIn('startup scripts did not stop', result.stderr)
        self.assertEqual(self.events().count('sleep:1'), 20)
        self.assertNotIn('stop', self.events())
        for pid in accepted:
            self.assertIn(f'term:{pid}', self.events())

    def test_invalid_fallback_restores_read_only_before_rejecting_metadata(self):
        self.fail_prepare()
        self.recovery.chmod(0o755)
        self.write('proc/mounts', 'fake / fake rw,relatime 0 0\n')
        before = self.events().count('approved-startup')
        result = self.run_script(None, '/fixture/etc/app_daemon')
        self.assertEqual(result.returncode, 71, result.stderr)
        self.assertEqual(self.events().count('approved-startup'), before)
        self.assert_ro()

    def test_marker_forbids_fallback_even_if_temporary_startup_is_reintroduced(self):
        self.assert_success(self.run_script())
        self.write('etc/c1updater/enrollment-startup', self.original, 0o700)
        self.write('etc/c1updater/enrollment-startup.sha256', self.digest(self.original) + '\n', 0o600)
        result = self.run_script(None, '/fixture/etc/app_daemon')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('approved-startup', self.events())
        self.assertIn('signed-startup', self.events())
        self.assertNotEqual(self.run_script('verify').returncode, 0)


if __name__ == '__main__':
    unittest.main()
