#!/usr/bin/env python3
"""Build only c1-ime-service and its C SDK for MIPS; run on Linux/WSL.

All compiled dependencies are rebuilt with one target toolchain. OpenCC's
library-only target and src-only install deliberately skip target data tools.
Only trusted source dictionaries/OpenCC data are copied; never host .a or
precompiled Rime build/*.bin. No device access, installation, or downloads.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    integration = Path(__file__).resolve().parent
    term = integration.parent
    parser.add_argument('--build-dir', type=Path, default=term.parent / '.c1-ime-mips-build')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--prefix', default='mipsel-linux-gnu-')
    parser.add_argument('--sysroot', type=Path)
    parser.add_argument('--x11-headers', type=Path, default=Path('/usr/include/X11'))
    args = parser.parse_args()
    if sys.platform != 'linux' or args.jobs < 1:
        parser.error('run on Linux/WSL with --jobs >= 1')
    for tool in ('cmake', args.prefix + 'gcc', args.prefix + 'g++', args.prefix + 'readelf'):
        if not shutil.which(tool):
            parser.error('missing tool: ' + tool)
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    stage = build / 'target-stage'
    toolchain = integration / 'mips/toolchain.cmake'
    environment = dict(os.environ, LC_ALL='C', CMAKE_BUILD_PARALLEL_LEVEL=str(args.jobs))
    # Avoid externally injected host include/library paths.
    for key in ('CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS'):
        environment.pop(key, None)
    with (build / 'build.log').open('w') as log:
        def run(command):
            command = [str(item) for item in command]
            line = '+ ' + shlex.join(command)
            print(line, flush=True)
            log.write(line + '\n'); log.flush()
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, env=environment)
            for line in process.stdout:
                print(line, end='', flush=True)
                log.write(line); log.flush()
            if process.wait():
                raise subprocess.CalledProcessError(process.returncode, command)

        common = [f'-DCMAKE_TOOLCHAIN_FILE={toolchain}', f'-DC1_MIPS_PREFIX={args.prefix}',
                  f'-DC1_MIPS_STAGE={stage}', f'-DCMAKE_INSTALL_PREFIX={stage}',
                  '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_BUILD_TYPE=Release',
                  '-DCMAKE_C_FLAGS_RELEASE=-Os -DNDEBUG -ffunction-sections -fdata-sections',
                  '-DCMAKE_CXX_FLAGS_RELEASE=-Os -DNDEBUG -ffunction-sections -fdata-sections',
                  '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DBUILD_SHARED_LIBS=OFF',
                  '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', '-DBUILD_TESTING=OFF']
        if args.sysroot:
            common.append(f'-DCMAKE_SYSROOT={args.sysroot.resolve()}')
        deps = term / 'deps/librime/deps'
        for name, source, options in (
            ('yaml-cpp', 'yaml-cpp', ['-DYAML_CPP_BUILD_TESTS=OFF', '-DYAML_CPP_BUILD_TOOLS=OFF',
                                     '-DYAML_CPP_BUILD_CONTRIB=OFF', '-DYAML_CPP_INSTALL=ON']),
            ('leveldb', 'leveldb', ['-DLEVELDB_BUILD_TESTS=OFF', '-DLEVELDB_BUILD_BENCHMARKS=OFF',
                                   '-DHAVE_SNAPPY=OFF', '-DHAVE_CRC32C=OFF', '-DHAVE_TCMALLOC=OFF']),
            ('marisa', 'marisa-trie', ['-DENABLE_TOOLS=OFF', '-DENABLE_NATIVE_CODE=OFF']),
        ):
            output = build / 'deps' / name
            run(['cmake', '-S', deps / source, '-B', output, *common, *options])
            run(['cmake', '--build', output, '--target', 'install', '-j', args.jobs])

        # Upstream OpenCC has unconditional data targets. Build only libopencc,
        # then install only src/; never run its MIPS opencc_dict/phrase_extract.
        # USE_SYSTEM_MARISA selects our staged target marisa, not the host one.
        opencc = build / 'deps/opencc'
        run(['cmake', '-S', deps / 'opencc', '-B', opencc, *common,
             '-DUSE_SYSTEM_MARISA=ON', f'-DLIBMARISA={stage}/lib/libmarisa.a',
             f'-DCMAKE_CXX_FLAGS=-EL -march=mips32r2 -mabi=32 -mhard-float -mfp32 -I{stage}/include',
             '-DCMAKE_CXX_STANDARD=17', '-DCMAKE_CXX_STANDARD_REQUIRED=ON', '-DCMAKE_CXX_EXTENSIONS=OFF',
             '-DENABLE_GTEST=OFF', '-DENABLE_BENCHMARK=OFF', '-DBUILD_PYTHON=OFF'])
        run(['cmake', '--build', opencc, '--target', 'libopencc', '-j', args.jobs])
        run(['cmake', '-DCMAKE_INSTALL_CONFIG_NAME=Release', '-DCMAKE_INSTALL_LOCAL_ONLY=ON',
             '-P', opencc / 'src/cmake_install.cmake'])
        # These two X11 protocol headers contain integer constants only. Copying
        # them avoids exposing the host /usr/include (or linking host Xlib).
        x11 = stage / 'include/X11'
        x11.mkdir(parents=True, exist_ok=True)
        for name in ('keysym.h', 'keysymdef.h'):
            shutil.copy2(args.x11_headers / name, x11 / name)

        service_build = build / 'service'
        run(['cmake', '-S', integration / 'mips', '-B', service_build, *common])
        run(['cmake', '--build', service_build, '--target', 'c1-ime-service', 'c1-ime-sdk-test',
             'c1-ime-client-demo', '-j', args.jobs])
        package = build / 'package'
        binaries = package / 'bin'
        binaries.mkdir(parents=True, exist_ok=True)
        reports = []
        hardening_report = {}
        for name in ('c1-ime-service', 'c1-ime-sdk-test', 'c1-ime-client-demo'):
            output = service_build / 'integration' / name
            elf = subprocess.check_output([args.prefix + 'readelf', '-h', '-A', str(output)], text=True)
            headers = subprocess.check_output([args.prefix + 'readelf', '-W', '-l', '-d', str(output)], text=True)
            for value in ('ELF32', 'little endian', 'o32', 'mips32r2', 'Hard float', 'CPR1 size: 32'):
                if value not in elf:
                    raise RuntimeError(f'{name}: wrong target ABI, missing {value}')
            if 'INTERP' in headers or '(NEEDED)' in headers:
                raise RuntimeError(f'{name}: runtime dynamic dependency')
            stack_lines = [line.split() for line in headers.splitlines()
                           if line.strip().startswith('GNU_STACK ')]
            if len(stack_lines) != 1:
                raise RuntimeError(f'{name}: missing or duplicate GNU_STACK program header')
            stack_flags = ''.join(stack_lines[0][6:-1])
            has_relro = any(line.strip().startswith('GNU_RELRO ') for line in headers.splitlines())
            hardening_report[name] = {'gnu_stack': stack_flags, 'gnu_relro': has_relro,
                                     'sha256': hashlib.sha256(output.read_bytes()).hexdigest()}
            if name == 'c1-ime-service':
                if stack_flags != 'RW' or not has_relro:
                    raise RuntimeError(f'{name}: expected GNU_STACK RW and GNU_RELRO')
                hardening_report[name]['link_options'] = '-Wl,-z,noexecstack,-z,relro,-z,now'
                hardening_report[name]['bind_now'] = 'not applicable: static ELF has no dynamic section'
            reports.append(f'=== {name} ===\n' + elf.split('Static GOT:', 1)[0] + headers)
            shutil.copy2(output, binaries / name)
        (build / 'elf-report.txt').write_text('\n'.join(reports))
        (build / 'hardening-audit.json').write_text(json.dumps(hardening_report, indent=2) + '\n')
        required_threads = {line for line in (integration / 'mips/pthread-symbols.txt').read_text().splitlines()
                            if line and not line.startswith('#')}
        symbol_output = subprocess.check_output([args.prefix + 'nm', '--defined-only',
                                                 str(binaries / 'c1-ime-service')], text=True)
        definitions = {parts[2]: parts[0] for line in symbol_output.splitlines()
                       if len(parts := line.split()) == 3}
        if any(not int(definitions.get(name, '0'), 16) for name in required_threads):
            raise RuntimeError('static service has unresolved/zero weak pthread entry points')
        (build / 'pthread-link-audit.json').write_text(json.dumps(
            {name: definitions[name] for name in sorted(required_threads)}, indent=2) + '\n')

        # Audit every compiler-produced archive member, including librime/spdlog,
        # rather than claiming target provenance from the final ELF alone.
        archive_report = {}
        for archive in sorted(build.rglob('*.a')):
            details = subprocess.check_output([args.prefix + 'readelf', '-h', str(archive)], text=True)
            machines = [line.strip() for line in details.splitlines() if 'Machine:' in line]
            flags = [line.strip() for line in details.splitlines() if 'Flags:' in line]
            byte_orders = [line.strip() for line in details.splitlines() if 'Data:' in line]
            if (not machines or any('MIPS' not in line for line in machines)
                    or len(flags) != len(machines) or len(byte_orders) != len(machines)
                    or any('mips32r2' not in line or 'o32' not in line for line in flags)
                    or any('little endian' not in line for line in byte_orders)):
                raise RuntimeError(f'non-target archive detected: {archive}')
            archive_report[str(archive.relative_to(build))] = len(machines)
        (build / 'archive-audit.json').write_text(json.dumps(archive_report, indent=2) + '\n')

        data = package / 'share/rime-data'
        data.mkdir(parents=True, exist_ok=True)
        source_data = term / 'data/rime-data'
        for source in sorted(source_data.glob('*.yaml')):
            shutil.copy2(source, data / source.name)
        shutil.copy2(source_data / 'essay.txt', data / 'essay.txt')
        shutil.copytree(source_data / 'opencc', data / 'opencc', dirs_exist_ok=True)
        hashes = {str(path.relative_to(package)): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in sorted(package.rglob('*')) if path.is_file()}
        (build / 'package-sha256.json').write_text(json.dumps(hashes, indent=2) + '\n')
        run([args.prefix + 'g++', '--version'])
        print(f'PASS: fully static MIPS service/SDK, all archives target-checked: {package}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print('BLOCKED:', error, file=sys.stderr)
        sys.exit(1)
