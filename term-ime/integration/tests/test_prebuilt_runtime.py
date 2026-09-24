#!/usr/bin/env python3
"""Retest the exact prepared MIPS payload with fresh users, without rebuilding."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile

INTEGRATION = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEGRATION))
from runtime_support import (BUILD_FILES, OPENCC_FILES, check_elf, exercise,
                             missing_resource_check, require, sha256, snapshot)
spec = importlib.util.spec_from_file_location('prepare_runtime', INTEGRATION / 'prepare-runtime.py')
prepare = importlib.util.module_from_spec(spec); spec.loader.exec_module(prepare)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload', type=Path, required=True)
    parser.add_argument('--report-dir', type=Path, required=True)
    parser.add_argument('--qemu', default='qemu-mipsel')
    args = parser.parse_args()
    payload = args.payload.resolve()
    reports = args.report_dir.resolve(); reports.mkdir(parents=True, exist_ok=True)
    require(reports != payload and payload not in reports.parents, 'reports must be outside payload')
    prepare.validate_payload(payload)
    before = snapshot(payload)
    check_elf(payload / 'bin/c1-ime-service')
    report = {'success': False, 'device_tested': False,
              'service_sha256': sha256(payload / 'bin/c1-ime-service')}
    try:
        with tempfile.TemporaryDirectory(prefix='c1-prebuilt-', dir='/tmp') as tmp:
            root = Path(tmp)
            local = root / 'payload'; shutil.copytree(payload, local)
            binary = local / 'bin/c1-ime-service'
            data = local / 'share/rime-data'
            runner = [args.qemu, '-cpu', '24Kf']
            report['fresh_user'] = exercise(binary, data, root / 'first', reports / 'first', runner, True, 60)
            report['second_fresh_user'] = exercise(binary, data, root / 'second', reports / 'second', runner, True, 60)
            required = ['build/' + name for name in BUILD_FILES]
            required += ['opencc/' + name for name in OPENCC_FILES]
            required += ['default.yaml']
            report['missing_resources'] = [missing_resource_check(binary, data, root / f'missing-{i}', runner, name)
                                           for i, name in enumerate(required)]
            require(snapshot(local) == before, 'tests mutated payload contents')
        require(snapshot(payload) == before, 'tests mutated original payload')
        report['success'] = True
        print('PASS: two fresh users, nihao/zhongguo, paging, all required-file negatives, no dictionary rebuild')
    except Exception as error:
        report['error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        (reports / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    main()
