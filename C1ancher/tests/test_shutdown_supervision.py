"""Real shutdown IPC and process supervision, with unsigned disposable storage.
Only update verification/storage is stubbed; real supervisor, launcher, health
policy and shutdown client run. Never invokes poweroff, signing or a device.
"""
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
APP = r'''#!/usr/bin/python3
import ctypes, os, pathlib, socket, struct, subprocess, time
root = pathlib.Path(os.environ['FIXTURE_ROOT'])
lib = ctypes.CDLL(os.environ['FIXTURE_SHUTDOWN_LIB'])
inherited_fd = int(os.environ.get('C1_SHUTDOWN_FD', '-1'))
initialized = lib.c1_shutdown_init()
with (root / 'starts').open('a') as stream:
    stream.write(str(os.getpid()) + ' ' + str(os.getppid()) + '\n')
fd = int(os.environ['C1_UI_HEARTBEAT_FD'])
ready = root / 'ready'
ready.write_text('test-only-digest')
active = False
last = ''
while True:
    command = (root / 'command').read_text() if (root / 'command').exists() else ''
    if command and command != last:
        last = command
        if command == 'begin' or command == 'failed-command':
            result = initialized or lib.c1_shutdown_begin()
            (root / 'begin-result').write_text(str(result))
            if result == 0:
                active = True
                if command == 'failed-command':
                    # A failed local helper, never the host poweroff command.
                    assert subprocess.run(['/bin/sh', '-c', 'exit 1']).returncode == 1
                    (root / 'cancel-result').write_text(str(lib.c1_shutdown_cancel()))
                    active = False
        elif command == 'cancel':
            (root / 'cancel-result').write_text(str(lib.c1_shutdown_cancel()))
            active = False
        elif command == 'fork-spoof':
            child = os.fork()
            if child == 0:
                (root / 'spoof-result').write_text(str(lib.c1_shutdown_begin()))
                os._exit(0)
            os.waitpid(child, 0)
        elif command == 'raw-fork-spoof':
            child = os.fork()
            if child == 0:
                # Bypass client-side owner check; server must reject kernel PID.
                channel = socket.socket(fileno=inherited_fd)
                channel.send(struct.pack('=IIIII', 0x43315344, 1, 1, 99, 0))
                channel.detach()
                os._exit(0)
            os.waitpid(child, 0)
            time.sleep(.2)
            (root / 'spoof-result').write_text('sent')
    if not (root / 'no-beats').exists():
        os.write(fd, b'H')
    if active and not (root / 'no-renew').exists():
        lib.c1_shutdown_keepalive()
    time.sleep(.03)
'''


class ShutdownSupervisionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='c1-shutdown-build-')
        cls.output = Path(cls.build.name)
        common = ['cc', '-D_POSIX_C_SOURCE=200809L', '-std=c11', '-Wall', '-Wextra',
                  '-Wpedantic', '-Werror', '-I' + str(ROOT / 'src')]
        def compile_to(name, sources, extra=()):
            subprocess.run(common + list(extra) + [str(ROOT / p) for p in sources] +
                           ['-o', str(cls.output / name)], check=True)
        compile_to('C1ancher-launcher', ['src/launcher/main.c', 'src/launcher/cleanup.c',
                   'src/update/state.c', 'src/security/secure_file.c', 'src/launcher/policy.c',
                   'src/platform/liveness.c', 'src/platform/shutdown.c'])
        compile_to('supervisor', ['tests/shutdown_supervise_host.c', 'src/update/supervise.c',
                   'src/update/supervise_policy.c', 'src/launcher/policy.c',
                   'src/security/secure_file.c', 'src/platform/shutdown.c'])
        compile_to('shutdown.so', ['src/platform/shutdown.c'], ['-fPIC', '-shared'])

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='c1-shutdown-chain-')
        self.root = Path(self.temp.name)
        self.artifacts = self.root / 'current/artifacts'
        self.artifacts.mkdir(parents=True)
        shutil.copyfile(self.output / 'C1ancher-launcher', self.artifacts / 'C1ancher-launcher')
        (self.artifacts / 'C1ancher-launcher').chmod(0o700)
        (self.artifacts / 'C1ancher').write_text(APP)
        (self.artifacts / 'C1ancher').chmod(0o700)
        self.process = None

    def tearDown(self):
        if self.process:
            # A fresh session belongs exclusively to this fixture.
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.process.wait(timeout=5)
        self.temp.cleanup()

    def wait_for(self, predicate, timeout=5):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if predicate():
                return
            time.sleep(.02)
        self.fail('shutdown fixture timed out')

    def read(self, name):
        path = self.root / name
        return path.read_text() if path.exists() else ''

    def command(self, value):
        (self.root / 'command').write_text(value)

    def start(self, pending=True, legacy=False):
        env = dict(os.environ, FIXTURE_ROOT=str(self.root),
                   FIXTURE_SHUTDOWN_LIB=str(self.output / 'shutdown.so'),
                   C1_HEARTBEAT_STARTUP_MS='1800', C1_HEARTBEAT_TIMEOUT_MS='1600')
        if legacy:
            # Old launcher keeps its heartbeat format and ignores new socket.
            (self.artifacts / 'C1ancher-launcher').write_text(
                '#!/usr/bin/python3\nimport os,time,struct\nfrom pathlib import Path\n'
                f"Path({str(self.root / 'ready')!r}).touch()\n"
                'while True:\n'
                " os.write(int(os.environ['C1_SUPERVISOR_HEARTBEAT_FD']),struct.pack('=qq',int(time.monotonic()*1000),os.getpid()))\n"
                ' time.sleep(.05)\n')
        self.process = subprocess.Popen([str(self.output / 'supervisor'), str(self.root),
                                        str(self.root / 'ready'), str(self.root / 'events'),
                                        'pending' if pending else 'confirmed'],
                                       env=env, start_new_session=True)
        self.wait_for(lambda: bool(self.read('ready')) if not legacy else (self.root / 'ready').exists())
        if not legacy:
            self.wait_for(lambda: len(self.read('starts').split()) >= 2)
            self.ui, self.launcher = map(int, self.read('starts').splitlines()[0].split())

    def begin(self):
        self.command('begin')
        self.wait_for(lambda: bool(self.read('begin-result')))
        self.assertEqual(self.read('begin-result'), '0')

    def test_pending_shutdown_kill_launcher_never_restarts_or_rolls_back(self):
        self.start(pending=True)
        self.begin()
        (self.root / 'no-beats').touch()
        time.sleep(2)
        self.assertEqual(len(self.read('starts').splitlines()), 1)
        os.kill(self.launcher, signal.SIGKILL)
        self.assertEqual(self.process.wait(timeout=4), 0)
        self.assertEqual(self.read('events'), '')
        self.assertEqual(len(self.read('starts').splitlines()), 1)

    def test_confirmed_shutdown_kill_launcher_never_restarts(self):
        self.start(pending=False)
        self.begin()
        os.kill(self.launcher, signal.SIGKILL)
        self.assertEqual(self.process.wait(timeout=4), 0)
        self.assertEqual(len(self.read('starts').splitlines()), 1)
        self.assertEqual(self.read('events'), '')

    def test_shutdown_ui_sigkill_is_propagated(self):
        self.start(pending=True)
        self.begin()
        os.kill(self.ui, signal.SIGKILL)
        self.assertEqual(self.process.wait(timeout=4), 0)
        self.assertEqual(self.read('events'), '')
        self.assertEqual(len(self.read('starts').splitlines()), 1)

    def test_pending_cancel_then_kill_rolls_back(self):
        self.start(pending=True)
        self.command('failed-command')
        self.wait_for(lambda: bool(self.read('cancel-result')))
        self.assertEqual(self.read('begin-result'), '0')
        self.assertEqual(self.read('cancel-result'), '0')
        os.kill(self.launcher, signal.SIGKILL)
        self.assertEqual(self.process.wait(timeout=4), 71)
        self.assertEqual(self.read('events'), 'rollback\n')

    def test_confirmed_cancel_restores_watchdog_and_outer_restart(self):
        self.start(pending=False)
        self.command('failed-command')
        self.wait_for(lambda: bool(self.read('cancel-result')))
        self.assertEqual(self.read('cancel-result'), '0')
        self.command('')
        (self.root / 'no-beats').touch()
        self.wait_for(lambda: len(self.read('starts').splitlines()) >= 2, timeout=6)
        (self.root / 'no-beats').unlink()
        os.kill(self.launcher, signal.SIGKILL)
        self.wait_for(lambda: len(self.read('starts').splitlines()) >= 3, timeout=6)
        self.assertIsNone(self.process.poll())
        self.assertEqual(self.read('events'), '')

    def test_normal_pending_launcher_crash_rolls_back(self):
        self.start(pending=True)
        os.kill(self.launcher, signal.SIGKILL)
        self.assertEqual(self.process.wait(timeout=4), 71)
        self.assertEqual(self.read('events'), 'rollback\n')

    def test_normal_confirmed_launcher_crash_restarts(self):
        self.start(pending=False)
        os.kill(self.launcher, signal.SIGKILL)
        self.wait_for(lambda: len(self.read('starts').splitlines()) >= 2)
        self.assertIsNone(self.process.poll())

    def test_ack_waits_for_outer_supervisor(self):
        self.start(pending=True)
        self.process.send_signal(signal.SIGSTOP)
        try:
            self.command('begin')
            time.sleep(.35)
            self.assertEqual(self.read('begin-result'), '')
        finally:
            self.process.send_signal(signal.SIGCONT)
        self.wait_for(lambda: bool(self.read('begin-result')))
        self.assertEqual(self.read('begin-result'), '0')
        os.kill(self.launcher, signal.SIGKILL)
        self.assertEqual(self.process.wait(timeout=4), 0)

    def test_forked_worker_cannot_request_shutdown(self):
        self.start(pending=True)
        self.command('fork-spoof')
        self.wait_for(lambda: bool(self.read('spoof-result')))
        self.assertNotEqual(self.read('spoof-result'), '0')
        os.kill(self.launcher, signal.SIGKILL)
        self.assertEqual(self.process.wait(timeout=4), 71)
        self.assertEqual(self.read('events'), 'rollback\n')

    def test_kernel_credentials_reject_forked_raw_packets(self):
        self.start(pending=True)
        self.command('raw-fork-spoof')
        self.wait_for(lambda: self.read('spoof-result') == 'sent')
        os.kill(self.launcher, signal.SIGKILL)
        self.assertEqual(self.process.wait(timeout=4), 71)
        self.assertEqual(self.read('events'), 'rollback\n')

    def test_unrenewed_intent_expires_and_restores_watchdog(self):
        self.start(pending=False)
        self.begin()
        self.command('')
        (self.root / 'no-renew').touch()
        (self.root / 'no-beats').touch()
        self.wait_for(lambda: len(self.read('starts').splitlines()) >= 2, timeout=35)
        self.assertIsNone(self.process.poll())
        self.assertEqual(self.read('events'), '')

    def test_missing_invalid_and_self_created_channels_fail_closed(self):
        probe = r'''
import ctypes, os, socket
lib = ctypes.CDLL(os.environ['FIXTURE_SHUTDOWN_LIB'])
os.environ.pop('C1_SHUTDOWN_FD', None)
assert lib.c1_shutdown_init() != 0
assert lib.c1_shutdown_begin() != 0
for value in ('1', '999999999999999999999', 'true', ''):
    os.environ['C1_SHUTDOWN_FD'] = value
    assert lib.c1_shutdown_init() != 0
    assert lib.c1_shutdown_begin() != 0
r, w = os.pipe()
os.environ['C1_SHUTDOWN_FD'] = str(w)
assert lib.c1_shutdown_init() != 0
os.close(r); os.close(w)
a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
os.environ['C1_SHUTDOWN_FD'] = str(b.fileno())
assert lib.c1_shutdown_init() != 0  # Created by self, not inherited from parent.
'''
        subprocess.run(['/usr/bin/python3', '-c', probe],
                       env=dict(os.environ, FIXTURE_SHUTDOWN_LIB=str(self.output / 'shutdown.so')),
                       check=True, timeout=4)

    def test_unknown_protocol_missing_ack_and_stale_ack_are_rejected(self):
        probe = r'''
import ctypes, os
lib = ctypes.CDLL(os.environ['FIXTURE_SHUTDOWN_LIB'])
assert lib.c1_shutdown_init() == 0
print(lib.c1_shutdown_begin(), flush=True)
'''
        for mode in ('unknown', 'missing', 'stale'):
            with self.subTest(mode=mode):
                a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                try:
                    env = dict(os.environ, FIXTURE_SHUTDOWN_LIB=str(self.output / 'shutdown.so'),
                               C1_SHUTDOWN_FD=str(b.fileno()))
                    client = subprocess.Popen(['/usr/bin/python3', '-c', probe], env=env,
                                              pass_fds=(b.fileno(),), stdout=subprocess.PIPE, text=True)
                    try:
                        a.settimeout(3)
                        packet = list(struct.unpack('=IIIII', a.recv(128)))
                        if mode == 'unknown':
                            packet[1] += 1
                            packet[2] |= 0x100
                            a.send(struct.pack('=IIIII', *packet))
                        elif mode == 'stale':
                            packet[2] |= 0x100
                            packet[3] += 42
                            a.send(struct.pack('=IIIII', *packet))
                        output, _ = client.communicate(timeout=4)
                        self.assertEqual(client.returncode, 0)
                        self.assertNotEqual(output.strip(), '0')
                    finally:
                        if client.poll() is None:
                            client.kill(); client.wait(timeout=3)
                finally:
                    a.close(); b.close()

    def test_no_ack_timeout_is_followed_by_cancel_and_restores_recovery(self):
        self.start(pending=True)
        self.process.send_signal(signal.SIGSTOP)
        try:
            self.command('begin')
            time.sleep(1.15)
        finally:
            self.process.send_signal(signal.SIGCONT)
        self.wait_for(lambda: bool(self.read('begin-result')))
        self.assertNotEqual(self.read('begin-result'), '0')
        time.sleep(.2)
        os.kill(self.launcher, signal.SIGKILL)
        self.assertEqual(self.process.wait(timeout=4), 71)
        self.assertEqual(self.read('events'), 'rollback\n')

    def test_old_launcher_health_protocol_still_confirms(self):
        self.start(pending=True, legacy=True)
        self.wait_for(lambda: self.read('events') == 'confirmed\n', timeout=34)
        self.assertIsNone(self.process.poll())

    def test_shutdown_packets_and_renewals_cannot_confirm_pending(self):
        self.start(pending=True)
        self.begin()
        time.sleep(32)
        self.assertEqual(self.read('events'), '')
        os.kill(self.launcher, signal.SIGKILL)
        self.assertEqual(self.process.wait(timeout=4), 0)

    def test_cancel_requires_a_fresh_full_health_window(self):
        self.start(pending=True)
        time.sleep(1)
        self.begin()
        time.sleep(2)
        self.command('cancel')
        self.wait_for(lambda: bool(self.read('cancel-result')))
        self.assertEqual(self.read('cancel-result'), '0')
        time.sleep(28)
        self.assertEqual(self.read('events'), '')
        self.wait_for(lambda: self.read('events') == 'confirmed\n', timeout=5)


if __name__ == '__main__':
    unittest.main()
