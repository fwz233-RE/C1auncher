#!/usr/bin/env python3
"""Linux/WSL release gate: validate packaged shell bytes and parse, never install.
Only the USB helper's allowlisted shebang/set prelude is executed to reproduce
CRLF option failures; all complete production scripts are parsed with -n only.
"""
import argparse
import os
from pathlib import Path
import runpy
import subprocess

PROJECT = Path(__file__).resolve().parents[1]
packer = runpy.run_path(str(PROJECT / 'installer/build-bundle.py'))


def check(payload):
    failures = []
    env = {key: value for key, value in os.environ.items() if key not in ('ENV', 'BASH_ENV')}
    for name in packer['DEVICE_SHELL']:
        data = packer['read_file'](payload / name)
        print(f'{name}: bytes={len(data)} CRLF={data.count(bytes([13, 10]))} CR={data.count(bytes([13]))}', flush=True)
        try:
            packer['validate_shell_text'](data, name)
        except ValueError as error:
            failures.append(str(error))
        shell = '/bin/bash' if name.startswith('accessories/') else '/bin/sh'
        result = subprocess.run([shell, '-n', str(payload / name)], capture_output=True, timeout=15, env=env)
        if result.returncode:
            failures.append(f'{name}: syntax failed: {result.stderr.decode("utf-8", errors="replace")}')
        if name == 'usb/device-open-adb.sh':
            # Strict whitelist: execute only a shebang and set, never the helper.
            assert data.splitlines()[:2] == [b'#!/bin/sh', b'set -eu'], 'Unexpected USB prelude; review required'
            prelude = b''.join(data.splitlines(keepends=True)[:2]) + b'\nexit 0\n'
            result = subprocess.run(['/bin/sh', '-s'], input=prelude, capture_output=True, timeout=5, env=env)
            print(f'USB prelude only: exit={result.returncode}, stderr={result.stderr!r}', flush=True)
            if result.returncode:
                failures.append('USB set -eu prelude failed')
    if failures:
        raise ValueError('\n'.join(failures))
    print(f'PASS: {len(packer["DEVICE_SHELL"])} packaged device shell files validated and syntax-checked; USB set -eu executed in isolation. No ADB/device operations.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload', type=Path, required=True)
    check(parser.parse_args().payload.resolve())
