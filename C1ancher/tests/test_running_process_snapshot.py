"""Exercise the shipped readonly process probe using the previously captured BusyBox.
Private proc-shaped fixtures only: no ADB, devices, mounts or service actions.
Run in WSL: python3 -m unittest discover -s tests -p test_running_process_snapshot.py -v
"""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest
from compiled_process_probe import compiled_probe

ROOT = Path(__file__).resolve().parents[1]
BB = ROOT / 'build/busybox-device-20260906/busybox'
FIRMWARE = ROOT.parent / 'firmware-analysis/system-rootfs'
QEMU = shutil.which('qemu-mipsel-static')
PIN = '69cd474401a9c47e8911cb352faebbf5cd731db63d30f035e5a0afcb82f74179'

@unittest.skipUnless(os.name == 'posix' and QEMU and BB.is_file(), 'Requires WSL/Linux and pinned BusyBox')
class ProcessSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.assertEqual(hashlib.sha256(BB.read_bytes()).hexdigest(), PIN)
        self.temp = tempfile.TemporaryDirectory(prefix='c1-process-snapshot-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.core = self.root / 'usr/data/c1/core'
        self.artifacts = self.core / 'releases/7-1.2.3-test/artifacts'
        self.artifacts.mkdir(parents=True)
        (self.core / 'current').symlink_to('releases/7-1.2.3-test')
        for name in ('C1ancher', 'C1ancher-launcher'):
            (self.artifacts / name).write_bytes(('private fixture ' + name).encode())
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        for applet in ('readlink', 'cat', 'sha256sum'):
            wrapper = self.bin / applet
            wrapper.write_text('#!/bin/sh\n'
                + f'exec {shlex.quote(QEMU)} -L {shlex.quote(str(FIRMWARE))} {shlex.quote(str(BB))} {applet} "$@"\n')
            wrapper.chmod(0o700)
        self.command = compiled_probe().replace('/usr/data/c1/core', str(self.core)).replace('/proc/', str(self.root / 'proc') + '/')
        self.node(90, 80, 100, 'C1ancher-launcher')
        self.node(100, 90, 110)

    def node(self, pid, ppid, start, name='C1ancher', comm='C1ancher ) name (x)'):
        path = self.root / 'proc' / str(pid)
        path.mkdir(parents=True)
        (path / 'exe').symlink_to(self.artifacts / name)
        # fields 3..22: state, ppid, 17 unused fields, starttime.
        (path / 'stat').write_text(f'{pid} ({comm}) S {ppid} ' + '0 ' * 17 + f'{start} 0\n')

    def probe(self):
        result = subprocess.run([QEMU, '-L', str(FIRMWARE), str(BB), 'sh', '-c', self.command],
            env=dict(os.environ, PATH=str(self.bin)), capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = result.stdout.splitlines()
        self.assertEqual(rows[-1], 'C1RUN_END')
        return rows

    def test_compiled_probe_and_real_exit_wrapper_parse_on_device_busybox(self):
        command = compiled_probe()
        wrapped = '( ' + command + ' ); rc=$?; printf "\\n__C1_SETUP_EXIT_test=%s\\n" "$rc"'
        result = subprocess.run([QEMU, '-L', str(FIRMWARE), str(BB), 'sh', '-n', '-c', wrapped],
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_old_crlf_literal_reproduces_reported_do_syntax_error(self):
        broken = compiled_probe().replace('\n', '\r\n')
        result = subprocess.run([QEMU, '-L', str(FIRMWARE), str(BB), 'sh', '-n', '-c', broken],
                                capture_output=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'expecting', result.stderr)
        self.assertIn(b'do', result.stderr)

    def test_parent_and_starttime_with_spaces_and_parentheses_in_comm(self):
        rows = self.probe()
        app = next(r.split('\t') for r in rows if r.startswith('C1RUN_PROCESS\t100\t'))
        self.assertEqual(app[1:4], ['100', '90', '110'])
        self.assertEqual(app[4], hashlib.sha256((self.artifacts / 'C1ancher').read_bytes()).hexdigest())
        self.assertEqual(app[5], str(self.artifacts / 'C1ancher'))

    def test_fork_supervisor_is_preserved_with_parent_relationship(self):
        self.node(101, 100, 120)
        self.node(102, 101, 130)
        rows = self.probe()
        self.assertEqual(len([r for r in rows if r.startswith('C1RUN_PROCESS\t')]), 4)
        self.assertTrue(any(r.startswith('C1RUN_PROCESS\t101\t100\t120\t') for r in rows))
        self.assertTrue(any(r.startswith('C1RUN_PROCESS\t102\t101\t130\t') for r in rows))

    def test_hash_failure_is_explicit_not_silently_ignored(self):
        (self.bin / 'sha256sum').write_text('#!/bin/sh\nexit 71\n')
        rows = self.probe()
        self.assertIn('C1RUN_ERROR\t100\thash', rows)
        self.assertFalse(any(r.startswith('C1RUN_PROCESS\t') for r in rows))

    def test_missing_stat_is_explicit_not_silently_ignored(self):
        (self.root / 'proc/100/stat').unlink()
        self.assertIn('C1RUN_ERROR\t100\tidentity-before', self.probe())

    def test_deleted_image_is_preserved_for_rejection(self):
        deleted = self.artifacts / 'C1ancher (deleted)'
        deleted.write_bytes(b'private deleted fixture')
        self.node(101, 100, 120, deleted.name)
        self.assertTrue(any(r.endswith('/C1ancher (deleted)') for r in self.probe()))

    def test_stat_change_during_hash_is_explicit(self):
        wrapper = self.bin / 'sha256sum'
        original = wrapper.read_text()
        wrapper.write_text('#!/bin/sh\n'
            + f"printf '%s\\n' '100 (replacement) S 90 " + '0 ' * 17 + "999 0' > " + shlex.quote(str(self.root / 'proc/100/stat')) + '\n'
            + original.removeprefix('#!/bin/sh\n'))
        self.assertIn('C1RUN_ERROR\t100\tchanged', self.probe())

    def test_current_pointer_change_is_explicit(self):
        other = self.core / 'releases/8-other/artifacts'
        other.mkdir(parents=True)
        (other / 'C1ancher').write_bytes(b'other fixture')
        wrapper = self.bin / 'sha256sum'
        original = wrapper.read_text()
        wrapper.write_text('#!/bin/sh\n'
            + '/bin/ln -sfn releases/8-other ' + shlex.quote(str(self.core / 'current')) + '\n'
            + original.removeprefix('#!/bin/sh\n'))
        self.assertIn('C1RUN_ERROR\t0\tcurrent-changed', self.probe())

if __name__ == '__main__':
    unittest.main()
