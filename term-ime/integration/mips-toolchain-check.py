#!/usr/bin/env python3
"""Read-only toolchain preflight except temporary compilation outputs.
No downloads, installation, device execution, or deployment.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


def run(args):
    os.environ['LC_ALL'] = 'C'
    prefix = os.environ.get('MIPS_CXX_PREFIX', 'mipsel-linux-gnu-')
    cc = os.environ.get('MIPS_CC', prefix + 'gcc')
    cxx = os.environ.get('MIPS_CXX', prefix + 'g++')
    readelf = os.environ.get('MIPS_READELF', prefix + 'readelf')
    for tool in (cc, readelf):
        if not shutil.which(tool):
            raise RuntimeError('missing tool: ' + tool)
    flags = shlex.split(os.environ.get('MIPS_CFLAGS', ''))
    flags += ['-EL', '-march=mips32r2', '-mabi=32', '-mhard-float', '-mfp32']
    if os.environ.get('MIPS_SYSROOT'):
        flags += ['--sysroot=' + os.environ['MIPS_SYSROOT']]
    strict = ['-Wall', '-Wextra', '-Werror', '-pedantic']
    source = Path(__file__).resolve().parent
    print('C compiler:', cc, flush=True)
    subprocess.run([cc, '-dumpmachine'], check=True)

    def verify_elf(path):
        header = subprocess.check_output([readelf, '-h', '-A', str(path)], text=True)
        print(header.split('Static GOT:', 1)[0], flush=True)
        for required in ('ELF32', 'little endian', 'o32', 'mips32r2', 'Hard float', 'CPR1 size: 32'):
            if required not in header:
                raise RuntimeError('target ELF ABI mismatch: missing ' + required)
        programs = subprocess.check_output([readelf, '-l', str(path)], text=True)
        if 'INTERP' in programs:
            raise RuntimeError('binary requires a dynamic loader')

    with tempfile.TemporaryDirectory(prefix='c1-ime-mips-check-') as temp:
        temp = Path(temp)
        demo = temp / 'sdk-demo'
        subprocess.run([cc, *flags, '-std=c17', *strict, '-static', '-I' + str(source),
                        str(source / 'c1_ime_client.c'), str(source / 'examples/client_demo.c'),
                        '-o', str(demo)], check=True)
        # Also compile the real C regression program with the target compiler.
        subprocess.run([cc, *flags, '-std=c17', *strict, '-static', '-I' + str(source),
                        str(source / 'c1_ime_client.c'), str(source / 'tests/test_sdk.c'),
                        '-o', str(temp / 'sdk-test')], check=True)
        verify_elf(demo)
        print('PASS: C17 SDK/demo and tests statically linked for MIPS32r2 LE o32 hard-float FP32.', flush=True)
        if args.qemu:
            subprocess.run([args.qemu, '-cpu', '24Kf', str(temp / 'sdk-test')], check=True, timeout=30)
            print('PASS: MIPS C SDK synthetic-peer tests under QEMU.', flush=True)
        if args.sdk_only:
            return
        if not shutil.which(cxx):
            for query in ('-print-file-name=libstdc++.a', '-print-prog-name=cc1plus'):
                print(query + ': ' + subprocess.check_output([cc, *flags, query], text=True).strip(), flush=True)
            raise RuntimeError('missing MIPS C++ driver: ' + cxx)
        library = subprocess.check_output([cxx, *flags, '-print-file-name=libstdc++.a'], text=True).strip()
        if not Path(library).is_file():
            raise RuntimeError('missing target static libstdc++: ' + library)
        probe = temp / 'probe.cpp'
        probe.write_text('#include <filesystem>\n#include <string>\n#include <thread>\n'
                         '#include <condition_variable>\n'
                         'int main() { std::string s = std::filesystem::path("/tmp").string(); '
                         'std::condition_variable cv; cv.notify_all(); '
                         'std::thread t([] {}); t.join(); return s.empty(); }\n')
        # Match the service's static-link workaround, including condition-variable
        # destruction: a successful thread join alone misses another weak call.
        symbols = (source / 'mips/pthread-symbols.txt').read_text().splitlines()
        thread_link = ['-Wl,-u,' + symbol for symbol in symbols if symbol and not symbol.startswith('#')]
        subprocess.run([cxx, *flags, '-std=c++17', *strict, '-static', '-pthread',
                        *thread_link, str(probe),
                        '-o', str(temp / 'cxx-probe')], check=True)
        verify_elf(temp / 'cxx-probe')
        if args.qemu:
            subprocess.run([args.qemu, '-cpu', '24Kf', str(temp / 'cxx-probe')], check=True, timeout=30)
            print('PASS: MIPS C++17 filesystem/thread execution under QEMU.', flush=True)
        print('PASS: C++17 filesystem/thread static link. Rime/OpenCC target deps and host tools still require a cross-build.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-only', action='store_true', help='validate C SDK without requiring MIPS C++')
    parser.add_argument('--qemu', help='also execute target probes with this qemu-mipsel (CPU 24Kf)')
    try:
        run(parser.parse_args())
    except (RuntimeError, subprocess.SubprocessError, OSError) as error:
        print('BLOCKED:', error, file=sys.stderr)
        sys.exit(1)
