"""Local runtime log tests; temporary paths only, no keys, mounts or devices.
Run: python3 tests/test_runtime_log.py
Builds an isolated full updater, without changing the project's Makefile.
"""
from pathlib import Path
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
import unittest

from test_bootstrap import BootstrapTests, FAKE, ROOT

BUILD = Path(os.environ.get('C1_UPDATE_TEST_BUILD_DIR', str(ROOT / 'build/lifecycle-fixes')))
if not BUILD.is_absolute():
    BUILD = ROOT / BUILD
UPDATER = BUILD / 'host-runtime-log-updater'
FAULTS = BUILD / 'runtime-log-faults.so'


def build():
    BUILD.mkdir(parents=True, exist_ok=True)
    makefile = (ROOT / 'Makefile').read_text()
    sources = []
    for name in ('UPDATE_SOURCES', 'ED25519_VERIFY_SOURCES'):
        block = re.search(r'^' + name + r' := \\\n(.*?)(?=\n\n)', makefile, re.M | re.S).group(1)
        sources.extend(block.replace('\\', '').split())
    sources = list(dict.fromkeys(sources + ['src/update/log.c']))
    subprocess.run(['cc', '-D_POSIX_C_SOURCE=200809L', '-DED25519_NO_SEED',
                    '-Isrc', '-Ithird_party/ed25519', '-std=c11', '-Os',
                    '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                    *sources, '-o', str(UPDATER)], cwd=ROOT, check=True)
    subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                    'tests/runtime_log_faults.c', '-ldl', '-o', str(FAULTS)], cwd=ROOT, check=True)


def await_file(path):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if path.exists() and path.stat().st_size:
            return
        time.sleep(.01)
    raise AssertionError(f'Timed out waiting for {path}')


class RuntimeLogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='c1-runtime-log-')
        self.root = Path(self.temp.name)
        self.directory = self.root / 'log'
        self.directory.mkdir(mode=0o700)
        self.log = self.directory / 'supervisor.log'

    def tearDown(self):
        self.temp.cleanup()

    def command(self, code, limit=65536, rotations=4):
        return [str(UPDATER), 'run-logged', str(self.log), str(limit), str(rotations),
                sys.executable, '-c', code]

    def run_code(self, code, **kwargs):
        return subprocess.run(self.command(code), capture_output=True, timeout=10, **kwargs)

    def bounded(self, limit=65536, rotations=4):
        files = list(self.directory.iterdir())
        self.assertLessEqual(len(files), rotations + 1)
        for path in files:
            st = path.stat()
            self.assertLessEqual(st.st_size, limit)
            self.assertEqual(st.st_mode & 0o777, 0o600)
        return files

    def history(self, rotations=4):
        names = [self.directory / f'supervisor.log.{n}' for n in range(rotations, 0, -1)] + [self.log]
        return b''.join(path.read_bytes() for path in names if path.exists())

    def test_real_full_updater_dispatch(self):
        self.assertEqual(subprocess.check_output([str(UPDATER), '--log-version']), b'C1RUNLOG-1\n')

    def test_multimegabyte_single_line_exact_boundaries(self):
        size = 5 * 1024 * 1024 + 123
        code = f'import os; data=bytes(range(256))*20480 + bytes(range(123)); os.write(1,data)'
        result = self.run_code(code)
        self.assertEqual(result.returncode, 0, result.stderr)
        files = self.bounded()
        self.assertEqual(len(files), 5)
        self.assertEqual(self.log.stat().st_size, 123)
        for n in range(1, 5):
            self.assertEqual((self.directory / f'supervisor.log.{n}').stat().st_size, 65536)
        expected = bytes(range(256)) * 20480 + bytes(range(123))
        self.assertEqual(len(expected), size)
        self.assertEqual(self.history(), expected[-(4 * 65536 + 123):])

    def test_continuous_runtime_rotation_before_child_exit(self):
        code = 'import os,time\nfor i in range(160):\n os.write(1,bytes([i])*4096); time.sleep(.015)'
        process = subprocess.Popen(self.command(code), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        observations = 0
        try:
            while process.poll() is None:
                # Rotation renames names, so a concurrent stat may race with a rename.
                try:
                    self.bounded()
                    if (self.directory / 'supervisor.log.4').exists():
                        observations += 1
                except FileNotFoundError:
                    pass
                time.sleep(.01)
            self.assertEqual(process.returncode, 0, process.communicate()[1])
            self.assertGreater(observations, 20)
            self.bounded()
            expected = b''.join(bytes([i]) * 4096 for i in range(160))
            self.assertEqual(self.history(), expected[-5 * 65536:])
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate()

    def test_stdout_stderr_and_real_exit_codes(self):
        for status in (0, 71, 72, 75, 126, 127):
            with self.subTest(status=status):
                self.log.unlink(missing_ok=True)
                result = self.run_code(f'import os; os.write(1,b"out"); os.write(2,b"err"); raise SystemExit({status})')
                self.assertEqual(result.returncode, status)
                self.assertEqual(self.log.read_bytes(), b'outerr')
        result = self.run_code('import os,signal; os.kill(os.getpid(),signal.SIGKILL)')
        self.assertEqual(result.returncode, 128 + signal.SIGKILL)

    def test_short_write_then_enospc_still_drains_and_exits(self):
        env = dict(os.environ, LD_PRELOAD=str(FAULTS), C1_TEST_FAIL_LOG=str(self.log))
        result = self.run_code('import os; os.write(1,b"x"*3000000); raise SystemExit(75)', env=env)
        self.assertEqual(result.returncode, 75, result.stderr)
        self.assertEqual(self.log.read_bytes(), b'x' * 7)
        self.assertEqual(result.stdout + result.stderr, b'')

    def test_removed_directory_keeps_draining(self):
        ready, go = self.root / 'ready', self.root / 'go'
        code = (f'import os,time,pathlib; os.write(1,b"first"); pathlib.Path({str(ready)!r}).write_text("1")\n'
                f'while not pathlib.Path({str(go)!r}).exists(): time.sleep(.01)\n'
                'os.write(1,b"x"*3000000); raise SystemExit(72)')
        process = subprocess.Popen(self.command(code), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            await_file(ready)
            await_file(self.log)
            self.log.unlink()
            self.directory.rmdir()
            go.write_text('1')
            self.assertEqual(process.communicate(timeout=5), (b'', b''))
            self.assertEqual(process.returncode, 72)
            self.assertFalse(self.directory.exists())
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    def test_reject_unsafe_paths_without_touching_target(self):
        target = self.root / 'untouched'
        target.write_bytes(b'keep')
        target.chmod(0o600)
        for kind in ('symlink', 'hardlink', 'fifo', 'public-file', 'public-dir', 'ancestor-link'):
            with self.subTest(kind=kind):
                if kind == 'symlink':
                    self.log.symlink_to(target)
                elif kind == 'hardlink':
                    os.link(target, self.log)
                elif kind == 'fifo':
                    os.mkfifo(self.log, 0o600)
                elif kind == 'public-file':
                    self.log.write_bytes(b'keep')
                    self.log.chmod(0o644)
                elif kind == 'public-dir':
                    self.directory.chmod(0o755)
                else:
                    self.directory.rmdir()
                    self.directory.symlink_to(self.root, target_is_directory=True)
                result = self.run_code('import os; os.write(1,b"x"*1000000)')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(target.read_bytes(), b'keep')
                if kind == 'public-file':
                    self.assertEqual(self.log.read_bytes(), b'keep')
                if kind == 'ancestor-link':
                    self.directory.unlink()
                    self.directory.mkdir(mode=0o700)
                else:
                    self.log.unlink(missing_ok=True)
                    self.directory.chmod(0o700)

    def test_unsafe_rotation_target_and_oversized_old_files(self):
        target = self.root / 'untouched'
        target.write_bytes(b'keep')
        (self.directory / 'supervisor.log.4').symlink_to(target)
        result = self.run_code('import os; os.write(1,b"x"*1000000)')
        self.assertEqual(result.returncode, 0)
        self.assertFalse(self.log.exists())
        self.assertEqual(target.read_bytes(), b'keep')
        (self.directory / 'supervisor.log.4').unlink()
        for n in range(5):
            path = self.log if n == 0 else self.directory / f'supervisor.log.{n}'
            path.write_bytes(b'o' * 70000)
            path.chmod(0o600)
        result = self.run_code('import os; os.write(1,b"new")')
        self.assertEqual(result.returncode, 0)
        self.bounded()
        self.assertEqual(self.log.read_bytes(), b'new')

    def test_pipe_setup_failure_execs_original_once(self):
        env = dict(os.environ, LD_PRELOAD=str(FAULTS), C1_TEST_FAIL_PIPE='1')
        result = self.run_code('import os; os.write(1,b"once"); raise SystemExit(71)', env=env)
        self.assertEqual(result.returncode, 71)
        self.assertEqual(result.stdout, b'once')
        self.assertFalse(self.log.exists())

    def test_existing_partial_log_appends_and_rotates_exactly(self):
        self.log.write_bytes(b'a' * 65530)
        self.log.chmod(0o600)
        result = self.run_code('import os; os.write(1,b"b"*20)')
        self.assertEqual(result.returncode, 0)
        self.assertEqual((self.directory / 'supervisor.log.1').read_bytes(), b'a' * 65530 + b'b' * 6)
        self.assertEqual(self.log.read_bytes(), b'b' * 14)
        self.bounded()

    def test_missing_directory_created_privately(self):
        self.directory.rmdir()
        result = self.run_code('import os; os.write(2,b"started")')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.directory.stat().st_mode & 0o777, 0o700)
        self.assertEqual(self.log.read_bytes(), b'started')
        self.bounded()

    def test_second_collector_discards_instead_of_racing_rotation(self):
        import fcntl
        descriptor = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_code('import os; os.write(1,b"x"*3000000); raise SystemExit(72)')
            self.assertEqual(result.returncode, 72)
            self.assertFalse(self.log.exists())
        finally:
            os.close(descriptor)

    def test_zero_rotations_and_one_byte_limit(self):
        result = subprocess.run(self.command('import os; os.write(1,b"abcdef")', 1, 0),
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.bounded(1, 0)
        self.assertEqual(self.log.read_bytes(), b'f')

    def test_term_and_orphaned_session_descendants_are_reaped(self):
        for stop in (False, True):
            with self.subTest(stop=stop):
                pids = self.root / 'pids'
                ready = self.root / 'ready'
                pids.unlink(missing_ok=True)
                ready.unlink(missing_ok=True)
                code = f'''import os,signal,time,pathlib
signal.signal(signal.SIGTERM,signal.SIG_IGN)
child=os.fork()
if child == 0:
 os.setsid()
 grandchild=os.fork()
 if grandchild == 0:
  pathlib.Path({str(ready)!r}).write_text(str(os.getpid()))
  while True: os.write(1,b'x'*4096)
 while True: time.sleep(.01)
pathlib.Path({str(pids)!r}).write_text(str(os.getpid())+' '+str(child))
while not pathlib.Path({str(ready)!r}).exists(): time.sleep(.01)
{'while True: time.sleep(.01)' if stop else 'raise SystemExit(72)'}
'''
                process = subprocess.Popen(self.command(code), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                tracked = []
                try:
                    await_file(pids)
                    await_file(ready)
                    tracked = [int(n) for n in pids.read_text().split()] + [int(ready.read_text())]
                    started = time.monotonic()
                    if stop:
                        process.terminate()
                    out, err = process.communicate(timeout=10)
                    self.assertLess(time.monotonic() - started, 9)
                    self.assertEqual(process.returncode, 143 if stop else 72, err)
                    self.assertEqual(out + err, b'')
                    for pid in tracked:
                        self.assertFalse(Path(f'/proc/{pid}').exists(), f'descendant {pid} leaked')
                    self.bounded()
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                    for pid in tracked:
                        try:
                            os.kill(pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass


class LoggedBootstrapTests(BootstrapTests):
    """Run the existing entire bootstrap fault matrix through the real wrapper."""
    def setUp(self):
        super().setUp()
        helper = FAKE.replace("args = sys.argv[1:]", """args = sys.argv[1:]
if args[0] in ('--log-version', 'run-logged'):
    os.execv(%r, [%r] + args)
""" % (str(UPDATER), str(UPDATER)))
        self.helper.write_text(helper)
        text = self.script.read_text().replace('LOG_DIR=/storage/c1/update/log',
                                               f'LOG_DIR={self.root}/log')
        self.script.write_text(text)

    def test_absent_old_and_corrupt_helper_do_not_block_good_slot(self):
        for kind in ('absent', 'old', 'corrupt', 'not-executable', 'linked'):
            with self.subTest(kind=kind):
                for name in ('counts', 'events'):
                    (self.root / name).unlink(missing_ok=True)
                self.helper.unlink(missing_ok=True)
                if kind == 'old':
                    self.helper.write_text(FAKE)
                elif kind in ('corrupt', 'not-executable'):
                    self.helper.write_bytes(b'\x7fELF' + b'\0' * 60)
                elif kind == 'linked':
                    self.helper.symlink_to(UPDATER)
                if kind not in ('absent', 'linked'):
                    self.helper.chmod(0o600 if kind == 'not-executable' else 0o700)
                roles = self.run_case({'a': [0]})
                self.assertEqual(roles.count('a'), 1)
                self.assertFalse((self.root / 'log').exists())


if __name__ == '__main__':
    build()
    unittest.main()
