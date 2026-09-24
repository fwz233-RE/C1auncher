#!/usr/bin/env python3
"""Exercise real static MIPS binaries under qemu-user; never connects a device.

Temporary runtime/user data live on the Linux filesystem (not a Windows mount).
A fresh user directory forces target-side dictionary deployment. Only this
script's own foreground child is terminated during cleanup.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time

from test_service import (CHINESE, COMPOSING, CONSUMED, PASSWORD, PASSWORD_FLAG, READY,
                          OP_CANCEL, OP_KEY, OP_MODE, OP_PAGE, OP_PURPOSE, OP_SELECT,
                          OP_STATUS, request)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', required=True, type=Path)
    parser.add_argument('--report-dir', required=True, type=Path)
    parser.add_argument('--qemu', default='qemu-mipsel')
    parser.add_argument('--cpu', default='24Kf')
    parser.add_argument('--startup-timeout', type=float, default=900)
    args = parser.parse_args()
    assert shutil.which(args.qemu), 'qemu-mipsel is required'
    package = args.package.resolve()
    report_dir = args.report_dir.resolve()
    report_dir.mkdir(parents=True, exist_ok=True)
    runner = [args.qemu, '-cpu', args.cpu]
    report = {'runner': runner, 'device_tested': False, 'fresh_target_deployment': True,
              'checks': [], 'success': False}
    try:
        report['binary_sha256'] = {}
        for name in ('c1-ime-service', 'c1-ime-sdk-test', 'c1-ime-client-demo'):
            binary = package / 'bin' / name
            assert binary.is_file(), name
            report['binary_sha256'][name] = hashlib.sha256(binary.read_bytes()).hexdigest()
        unit = subprocess.run([*runner, str(package / 'bin/c1-ime-sdk-test')],
                              capture_output=True, text=True, timeout=30)
        assert unit.returncode == 0, (unit.stdout, unit.stderr)
        report['checks'].append('MIPS C17 SDK synthetic-peer regression')
        with tempfile.TemporaryDirectory(prefix='c1-ime-mips-', dir='/tmp') as root:
            root = Path(root)
            runtime = root / 'runtime'; runtime.mkdir(mode=0o700)
            user = root / 'user'; user.mkdir(mode=0o700)
            path = runtime / 'socket'
            shared = root / 'shared'
            shutil.copytree(package / 'share/rime-data', shared)
            # Never reuse precompiled host Rime tables (native mapped structs).
            assert not (shared / 'build').exists(), 'package must contain source Rime dictionaries only'
            command = [*runner, str(package / 'bin/c1-ime-service'), '--socket', str(path),
                       '--shared-data', str(shared), '--user-data', str(user)]
            with (report_dir / 'service.stdout.log').open('w') as stdout, \
                 (report_dir / 'service.stderr.log').open('w') as stderr:
                started = time.monotonic()
                service = subprocess.Popen(command, stdout=stdout, stderr=stderr)
                try:
                    deadline = started + args.startup_timeout
                    while not path.exists():
                        if service.poll() is not None:
                            raise RuntimeError(f'MIPS service exited: {service.returncode}; see service.stderr.log')
                        if time.monotonic() >= deadline:
                            raise TimeoutError('MIPS service did not bind its socket')
                        time.sleep(.05)
                    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                        client.settimeout(max(.1, deadline - time.monotonic()))
                        client.connect(str(path))
                        flags, status, preedit, commit, candidates = request(client, OP_STATUS, 1)
                        assert flags & READY and not flags & CHINESE and status == 0
                        assert not preedit and not commit and not candidates
                    report['cold_ready_seconds'] = round(time.monotonic() - started, 3)
                    report['checks'].append('MIPS service cold deploy and READY')
                    print(f"MIPS cold deploy READY after {report['cold_ready_seconds']}s", flush=True)
                    time.sleep(.1)  # Let the service process the focus-disconnect.
                    sdk = subprocess.run([*runner, str(package / 'bin/c1-ime-sdk-test')],
                                         env=dict(os.environ, C1_IME_TEST_SOCKET=str(path)),
                                         capture_output=True, text=True, timeout=120)
                    assert sdk.returncode == 0, (sdk.stdout, sdk.stderr)
                    report['checks'].append('MIPS SDK to MIPS service: nihao, commit, mode, password')
                    time.sleep(.1)
                    demo = subprocess.run([*runner, str(package / 'bin/c1-ime-client-demo'), str(path)],
                                          capture_output=True, text=True, timeout=60)
                    assert demo.returncode == 0 and 'commit: 你好' in demo.stdout, (demo.stdout, demo.stderr)
                    report['demo_stdout'] = demo.stdout
                    print(demo.stdout, end='', flush=True)
                    report['checks'].append('MIPS C demo committed 你好')
                    time.sleep(.1)
                    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                        client.settimeout(30); client.connect(str(path))
                        seq = 0
                        def call(op, keysym=0, value=0):
                            nonlocal seq
                            seq += 1
                            return request(client, op, seq, keysym=keysym, value=value)
                        flags, _, _, _, _ = call(OP_STATUS)
                        assert not flags & CHINESE
                        call(OP_MODE, value=1)
                        for char in 'nihao': response = call(OP_KEY, keysym=ord(char))
                        assert response[4][0] == '你好' and response[0] & CONSUMED
                        report['nihao_candidates'] = response[4]
                        assert call(OP_KEY, keysym=0xff0d)[3] == '你好'
                        for char in 'zhongguo': response = call(OP_KEY, keysym=ord(char))
                        assert response[4][0] == '中国', response[4]
                        report['simplified_candidates'] = response[4]
                        assert call(OP_KEY, keysym=0x20)[3] == '中国'
                        report['checks'].append('Reused OpenCC ocd2 data converts 中國 to 中国 on MIPS')
                        first = call(OP_KEY, keysym=ord('n'))[4]
                        second = call(OP_PAGE, value=1)[4]
                        assert len(first) == len(second) == 5 and first != second
                        assert call(OP_PAGE, value=0)[4] == first
                        assert call(OP_SELECT, value=4)[3] == first[4]
                        call(OP_KEY, keysym=ord('n'))
                        assert not call(OP_KEY, keysym=0xff08)[2]
                        call(OP_KEY, keysym=ord('n'))
                        assert not call(OP_KEY, keysym=0xff1b)[2]
                        call(OP_KEY, keysym=ord('n'))
                        assert not call(OP_CANCEL)[2]
                        call(OP_PURPOSE, value=PASSWORD)
                        for char in 'password nihao 123':
                            flags, _, preedit, commit, candidates = call(OP_KEY, keysym=ord(char))
                            assert flags & PASSWORD_FLAG and flags & CHINESE
                            assert not flags & (CONSUMED | COMPOSING)
                            assert not preedit and not commit and not candidates
                    report['checks'].append('Five candidates, page, select, enter, backspace, Escape, cancel, password bypass')
                    report['deployed_files'] = sorted(str(p.relative_to(user)) for p in user.rglob('*') if p.is_file())
                finally:
                    if service.poll() is None:
                        service.send_signal(signal.SIGTERM)
                        try:
                            service.wait(timeout=30)
                        except subprocess.TimeoutExpired:
                            service.kill(); service.wait()
                    report['service_exit_code'] = service.returncode
                assert service.returncode == 0, f'MIPS service exit code: {service.returncode}'
                assert not path.exists(), 'MIPS service did not remove its own socket'
                report['checks'].append('Graceful SIGTERM and own-socket cleanup')
        report['success'] = True
        print('PASS: real MIPS service and MIPS C SDK under qemu-mipsel; device acceptance remains pending.', flush=True)
    except Exception as error:
        report['error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        (report_dir / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    main()
