"""Local-only target runtime checks shared by preparation and regression tests."""
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent / 'tests'))
from test_service import (CHINESE, READY, OP_KEY, OP_MODE, OP_PAGE, OP_STATUS, request)

BUILD_FILES = ('default.yaml', 'luna_pinyin_simp.schema.yaml',
               'luna_pinyin_simp.prism.bin', 'luna_pinyin.table.bin', 'luna_pinyin.reverse.bin')
OPENCC_FILES = ('t2s_full.json', 'TSCharacters.ocd2', 'TSPhrases.ocd2',
                'variants.txt', 'variants_ext.txt', 'variants_jp.txt')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for data in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(data)
    return digest.hexdigest()


def snapshot(root):
    return {p.relative_to(root).as_posix(): sha256(p)
            for p in sorted(Path(root).rglob('*')) if p.is_file()}


def check_elf(binary, readelf='mipsel-linux-gnu-readelf'):
    env = dict(os.environ, LC_ALL='C')
    elf = subprocess.check_output([readelf, '-h', '-A', '-W', '-l', '-d', str(binary)],
                                  text=True, env=env)
    for marker in ('ELF32', 'little endian', 'MIPS', 'o32', 'mips32r2', 'Hard float', 'CPR1 size: 32'):
        require(marker in elf, 'wrong target ELF ABI: missing ' + marker)
    require('INTERP' not in elf and '(NEEDED)' not in elf, 'service must be fully static')
    stacks = [line.split() for line in elf.splitlines() if line.strip().startswith('GNU_STACK ')]
    require(len(stacks) == 1 and ''.join(stacks[0][6:-1]) == 'RW', 'service stack must be RW')
    require('GNU_RELRO' in elf, 'service must contain GNU_RELRO')
    return elf.split('Static GOT:', 1)[0]


def proc_memory(pid):
    # This is the Linux QEMU host process, including emulator/JIT allocations;
    # neither a guest-only measurement nor the real device memory footprint.
    try:
        lines = Path('/proc', str(pid), 'status').read_text().splitlines()
        return {line.split(':')[0] + '_kib': int(line.split()[1]) for line in lines
                if line.startswith(('VmRSS:', 'VmHWM:'))}
    except (OSError, ValueError, IndexError):
        return {}


def exercise(binary, shared, work, logs, runner, prebuilt_only, timeout=900):
    """Use a new private user directory; return report after a graceful exit."""
    require(not sys.flags.optimize, 'run without python -O: wire verifier uses assertions')
    work.mkdir(mode=0o700)
    runtime = work / 'runtime'; runtime.mkdir(mode=0o700)
    user = work / 'user'; user.mkdir(mode=0o700)
    path = runtime / 'socket'
    logs.mkdir(parents=True, exist_ok=True)
    before = snapshot(shared)
    command = [*runner, str(binary), '--socket', str(path), '--shared-data', str(shared), '--user-data', str(user)]
    if prebuilt_only:
        command.append('--prebuilt-only')
    result = {'command': command, 'service_sha256': sha256(binary), 'fresh_user': True,
              'prebuilt_only': prebuilt_only, 'device_tested': False,
              'memory_scope': 'Linux /proc QEMU process including emulator/JIT, not guest-only or device RSS'}
    peak = [0]
    done = threading.Event()
    with (logs / 'stdout.log').open('w') as stdout, (logs / 'stderr.log').open('w') as stderr:
        started = time.monotonic()
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        def sample():
            while not done.wait(.02):
                peak[0] = max(peak[0], proc_memory(process.pid).get('VmRSS_kib', 0))
        sampler = threading.Thread(target=sample, daemon=True); sampler.start()
        try:
            deadline = started + timeout
            while not path.exists():
                require(process.poll() is None, f'service exited {process.returncode}; see {logs}')
                require(time.monotonic() < deadline, 'service socket startup timed out')
                time.sleep(.02)
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(max(.1, deadline - time.monotonic()))
                client.connect(str(path))
                seq = 0
                def call(op, keysym=0, value=0):
                    nonlocal seq
                    seq += 1
                    return request(client, op, seq, keysym=keysym, value=value)
                flags, status, preedit, commit, candidates = call(OP_STATUS)
                require(flags & READY and not flags & CHINESE and status == 0,
                        'expected READY in English mode')
                require(not preedit and not commit and not candidates, 'fresh session has stale composition')
                result['ready_seconds'] = round(time.monotonic() - started, 3)
                result['ready_memory'] = proc_memory(process.pid)
                client.settimeout(30)
                call(OP_MODE, value=1)
                result['commits'] = {}
                for pinyin, expected in (('nihao', '你好'), ('zhongguo', '中国')):
                    for char in pinyin:
                        response = call(OP_KEY, keysym=ord(char))
                    require(response[4] and response[4][0] == expected, f'{pinyin}: {response[4]}')
                    committed = call(OP_KEY, keysym=32)[3]
                    require(committed == expected, f'wrong committed text: {committed}')
                    result['commits'][pinyin] = committed
                first = call(OP_KEY, keysym=ord('n'))[4]
                second = call(OP_PAGE, value=1)[4]
                require(len(first) == len(second) == 5 and first != second, 'five-candidate paging failed')
                result['five_candidate_paging'] = True
                result['after_input_memory'] = proc_memory(process.pid)
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
            done.set(); sampler.join()
            result['exit_code'] = process.returncode
            result['sampled_peak_VmRSS_kib'] = peak[0] or None
    require(process.returncode == 0, f'service exited {process.returncode}')
    require(not path.exists(), 'service left its socket behind')
    result['user_files'] = sorted(snapshot(user))
    result['user_dictionary_bins'] = [name for name in result['user_files'] if name.endswith('.bin')]
    require(snapshot(shared) == before, 'service mutated shared resources')
    if prebuilt_only:
        require(not result['user_dictionary_bins'], 'prebuilt-only created a dictionary binary')
        output = (logs / 'stdout.log').read_text() + (logs / 'stderr.log').read_text()
        require('Deploying schema:' not in output, 'prebuilt-only attempted schema deployment')
    return result


def missing_resource_check(binary, shared, work, runner, missing):
    """Caller supplies an isolated shared copy; restore even when test fails."""
    work.mkdir(mode=0o700)
    runtime = work / 'runtime'; runtime.mkdir(mode=0o700)
    user = work / 'user'; user.mkdir(mode=0o700)
    path = runtime / 'socket'
    resource = shared / missing
    hidden = resource.with_name(resource.name + '.test-hidden')
    resource.rename(hidden)
    try:
        started = time.monotonic()
        process = subprocess.run([*runner, str(binary), '--prebuilt-only', '--socket', str(path),
                                  '--shared-data', str(shared), '--user-data', str(user)],
                                 capture_output=True, text=True, timeout=15)
        require(process.returncode == 1 and 'Prebuilt-only resource missing/empty:' in process.stderr,
                f'missing {missing} did not fail closed: {process.stdout} {process.stderr}')
        require(not list(user.rglob('*.bin')), 'failed launch compiled dictionary data')
        require(not path.exists(), 'failed launch left socket behind')
        return {'missing': missing, 'exit_code': process.returncode,
                'seconds': round(time.monotonic() - started, 3), 'dictionary_bins_created': False}
    finally:
        hidden.rename(resource)
