#!/usr/bin/env python3
"""Prepare an unsigned runtime payload using only a real MIPS service in QEMU.

Linux/WSL only. Incrementally builds the target, generates dictionaries in an
empty /tmp directory with qemu-mipsel -cpu 24Kf, allowlists runtime resources,
then validates a NEW user directory with --prebuilt-only. No host dictionary
compiler, devices, ADB, signing, downloads, publishing, or git operations.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from runtime_support import (BUILD_FILES, OPENCC_FILES, check_elf, exercise,
                             missing_resource_check, require, sha256, snapshot)

INTEGRATION = Path(__file__).resolve().parent
TERM = INTEGRATION.parent
LICENSE_FILES = {
    'term-ime-THIRD_PARTY_NOTICES.md': 'THIRD_PARTY_NOTICES.md',
    'librime-BSD.txt': 'deps/librime/LICENSE',
    'darts-clone-BSD.txt': 'deps/librime/include/COPYING.darts-clone',
    'spdlog-MIT.txt': 'deps/spdlog/LICENSE',
    'fmt-MIT.rst': 'deps/spdlog/include/spdlog/fmt/bundled/fmt.license.rst',
    'utf8proc-Unicode-MIT.txt': 'deps/utf8proc/LICENSE.md',
    'yaml-cpp-MIT.txt': 'deps/librime/deps/yaml-cpp/LICENSE',
    'leveldb-BSD.txt': 'deps/librime/deps/leveldb/LICENSE',
    'marisa-dual-license.txt': 'deps/librime/deps/marisa-trie/COPYING.md',
    'OpenCC-Apache-2.0.txt': 'deps/librime/deps/opencc/LICENSE',
    'OpenCC-AUTHORS.txt': 'deps/librime/deps/opencc/AUTHORS',
    'RapidJSON-license.txt': 'deps/librime/deps/opencc/deps/rapidjson-1.1.0/license.txt',
    'LGPL-3.0.txt': 'licenses/LGPL-3.0.txt',
    'rime-luna-pinyin-AUTHORS.txt': 'licenses/rime-luna-pinyin-AUTHORS.txt',
    'rime-essay-AUTHORS.txt': 'licenses/rime-essay-AUTHORS.txt',
    'rime-prelude-AUTHORS.txt': 'licenses/rime-prelude-AUTHORS.txt',
}
SYSTEM_LICENSES = {
    'glibc-cross-copyright.txt': '/usr/share/doc/libc6-mipsel-cross/copyright',
    'GCC-runtime-copyright.txt': '/usr/share/doc/gcc-10-mipsel-linux-gnu-base/copyright',
    'LGPL-2.1.txt': '/usr/share/common-licenses/LGPL-2.1',
    'GPL-2.txt': '/usr/share/common-licenses/GPL-2',
    'GPL-3.txt': '/usr/share/common-licenses/GPL-3',
}


def copy_file(source, target):
    require(source.is_file() and not source.is_symlink(), f'not a regular source file: {source}')
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    target.chmod(0o644)


def licenses(payload, shared):
    out = payload / 'licenses'
    for name, source in LICENSE_FILES.items():
        copy_file(TERM / source, out / name)
    for name, source in SYSTEM_LICENSES.items():
        # Distribution-managed documentation is commonly a symlink.
        copy_file(Path(source).resolve(), out / name)
    for name in ('keysym.h', 'keysymdef.h'):
        text = Path('/usr/include/X11', name).read_text()
        # X11 places full permission/copyright notices before its first define.
        marker = text.find('#')
        require(marker > 0 and 'Copyright' in text[:marker], 'missing X11 license header')
        (out / (name + '-NOTICE.txt')).write_text(text[:marker], encoding='utf-8')
    header = (shared / 'luna_pinyin.dict.yaml').read_text(encoding='utf-8').split('\n---', 1)[0]
    (out / 'luna-pinyin-dictionary-NOTICE.txt').write_text(header + '\n', encoding='utf-8')
    copy_file(INTEGRATION / 'runtime-NOTICE.txt', payload / 'NOTICE.txt')


def validate_payload(payload):
    files = set(snapshot(payload))
    runtime = {'bin/c1-ime-service', 'NOTICE.txt', 'share/rime-data/default.yaml',
               'share/rime-data/luna_pinyin_simp.schema.yaml'}
    runtime.update('share/rime-data/build/' + name for name in BUILD_FILES)
    runtime.update('share/rime-data/opencc/' + name for name in OPENCC_FILES)
    notices = set('licenses/' + name for name in (*LICENSE_FILES, *SYSTEM_LICENSES,
                 'keysym.h-NOTICE.txt', 'keysymdef.h-NOTICE.txt', 'luna-pinyin-dictionary-NOTICE.txt'))
    require(files == runtime | notices, f'payload allowlist mismatch: {files ^ (runtime | notices)}')
    require(not any(p.is_symlink() for p in payload.rglob('*')), 'payload contains a symlink')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=TERM.parent / '.c1-ime-mips-build')
    parser.add_argument('--output', type=Path, required=True, help='new output directory (never overwritten)')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--qemu', default='qemu-mipsel')
    parser.add_argument('--startup-timeout', type=float, default=900)
    args = parser.parse_args()
    require(sys.platform == 'linux', 'run on Linux/WSL')
    require(not sys.flags.optimize, 'run without python -O')
    require(args.jobs >= 1 and args.startup_timeout > 0, 'invalid jobs/timeout')
    require(shutil.which(args.qemu), 'qemu-mipsel is required')
    output = args.output.resolve()
    require(not output.exists(), 'output already exists; choose a NEW --output directory')
    build = args.build_dir.resolve()
    require(output != build and output not in build.parents and build not in output.parents,
            'keep output and build directories separate')
    # Rebuild rather than accepting an untraceable/stale package binary.
    subprocess.run([sys.executable, str(INTEGRATION / 'build-mips.py'), '--build-dir', str(build),
                    '--jobs', str(args.jobs)], check=True)
    package = build / 'package'
    binary = package / 'bin/c1-ime-service'
    require(sha256(binary) == sha256(build / 'service/integration/c1-ime-service'), 'stale package service')
    elf = check_elf(binary)
    output.mkdir(parents=True)
    reports = output / 'reports'; reports.mkdir()
    payload = output / 'payload'
    report = {'success': False, 'device_tested': False, 'signed': False, 'published': False,
              'cpu': '24Kf', 'entry': 'bin/c1-ime-service', 'service_sha256': sha256(binary),
              'distribution_review_complete': False, 'source_archive_created': False,
              'argv_contract': ['--prebuilt-only', '--socket', '<absolute-private-runtime>/socket',
                                '--shared-data', '<absolute-install-root>/share/rime-data',
                                '--user-data', '<absolute-dedicated-private-user>']}
    try:
        (reports / 'elf-report.txt').write_text(elf)
        report['toolchain_packages'] = subprocess.check_output(
            ['dpkg-query', '-W', 'libc6-mipsel-cross', 'libstdc++-10-dev-mipsel-cross',
             'libgcc-10-dev-mipsel-cross'], text=True).splitlines()
        report['qemu_version'] = subprocess.check_output([args.qemu, '--version'], text=True).splitlines()[0]
        runner = [args.qemu, '-cpu', '24Kf']
        with tempfile.TemporaryDirectory(prefix='c1-runtime-', dir='/tmp') as temporary:
            root = Path(temporary)
            shared = root / 'source-data'; shared.mkdir()
            source = package / 'share/rime-data'
            require(not (source / 'build').exists() and not list(source.rglob('*.bin')),
                    'package source dictionaries must not contain precompiled Rime bins')
            for path in source.glob('*.yaml'):
                copy_file(path, shared / path.name)
            copy_file(source / 'essay.txt', shared / 'essay.txt')
            for name in OPENCC_FILES:
                copy_file(source / 'opencc' / name, shared / 'opencc' / name)
            report['source_data_sha256'] = snapshot(shared)
            print('Deploying dictionaries with the freshly built MIPS service under QEMU 24Kf...', flush=True)
            report['target_deployment'] = exercise(binary, shared, root / 'deploy', reports / 'deploy',
                                                    runner, False, args.startup_timeout)
            data = payload / 'share/rime-data'
            copy_file(binary, payload / 'bin/c1-ime-service')
            (payload / 'bin/c1-ime-service').chmod(0o755)
            for name in BUILD_FILES:
                copy_file(root / 'deploy/user/build' / name, data / 'build' / name)
            # ConfigBuilder needs small root YAML files. Compiled default also
            # carries the five-candidate/no-switcher-hotkey service settings.
            copy_file(data / 'build/default.yaml', data / 'default.yaml')
            copy_file(data / 'build/luna_pinyin_simp.schema.yaml', data / 'luna_pinyin_simp.schema.yaml')
            for name in OPENCC_FILES:
                copy_file(shared / 'opencc' / name, data / 'opencc' / name)
            licenses(payload, shared)
            validate_payload(payload)
            # Verify the eventual payload binary/data copied to a Linux filesystem;
            # /mnt/d does not reliably implement Unix directory mode semantics.
            runtime_copy = root / 'runtime-payload'
            shutil.copytree(payload, runtime_copy)
            target_binary = runtime_copy / 'bin/c1-ime-service'
            target_data = runtime_copy / 'share/rime-data'
            report['prebuilt_runtime'] = exercise(target_binary, target_data, root / 'fresh',
                                                  reports / 'prebuilt', runner, True, 60)
            report['missing_resource_checks'] = []
            for i, name in enumerate(BUILD_FILES):
                report['missing_resource_checks'].append(missing_resource_check(
                    target_binary, target_data, root / f'missing-{i}', runner, 'build/' + name))
            require(snapshot(runtime_copy) == snapshot(payload), 'tested payload differs from output')
        report['payload_sha256'] = snapshot(payload)
        report['payload_bytes'] = sum(path.stat().st_size for path in payload.rglob('*') if path.is_file())
        report['source_control_files_sha256'] = {
            str(p.relative_to(TERM)): sha256(p) for p in
            [INTEGRATION / 'service.cpp', TERM / 'src/ime/rime_engine.cpp', TERM / 'src/ime/rime_engine.hpp',
             INTEGRATION / 'prepare-runtime.py', INTEGRATION / 'runtime_support.py']}
        report['success'] = True
        (output / 'SHA256SUMS').write_text(''.join(f'{value}  payload/{name}\n'
                                                   for name, value in report['payload_sha256'].items()))
        print(f"PASS: unsigned runtime {payload}; ready={report['prebuilt_runtime']['ready_seconds']}s; "
              f"sha256={report['service_sha256']}", flush=True)
    except Exception as error:
        report['error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        (reports / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print('BLOCKED:', error, file=sys.stderr)
        sys.exit(1)
