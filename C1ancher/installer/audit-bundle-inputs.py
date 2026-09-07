#!/usr/bin/env python3
"""Compare an installer with explicit public release inputs; never execute payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import zipfile

PROJECT = Path(__file__).resolve().parents[1]
BUNDLE = runpy.run_path(str(PROJECT / 'installer/build-bundle.py'))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def audit(args):
    payload = args.bundle / 'payload'
    report = {'bundle': str(args.bundle), 'source_version': (PROJECT / 'VERSION').read_text().strip(), 'files': []}
    source_map = {'device-setup.sh': PROJECT / 'installer/device-setup.sh',
                  'usb/device-open-adb.sh': PROJECT / 'scripts/device-open-adb.sh',
                  'usb/S90usb.original': PROJECT.parent / 'firmware-analysis/system-rootfs/etc/init.d/S90usb',
                  'profile/repository.ed25519.pub': PROJECT / 'config/app-repo/repository.ed25519.pub'}
    for name in BUNDLE['LAYERS']['developer']:
        source_map['developer/' + name] = args.publisher / name
    for name in BUNDLE['LAYERS']['tools']:
        source_map['tools/' + name] = args.adb / name
    for name in BUNDLE['LAYERS']['accessories']:
        source_map['accessories/' + name] = PROJECT.parent / 'Pic/wallpaper.raw' if name == 'wallpaper.raw' else PROJECT / 'third_party/neofetch' / name
    for name in ('device-repository-config.sh', 'c1-update-check.sh'):
        source_map['profile/' + name] = PROJECT / 'scripts' / name
    for name in BUNDLE['ENROLLMENT']:
        source_map['enrollment/' + name] = args.enrollment / name
    if args.installer_exe:
        source_map['../C1SlimInstaller.exe'] = args.installer_exe
    for name in ('USER-GUIDE.md', 'THIRD-PARTY-NOTICES.txt'):
        source_map['../' + name] = PROJECT / 'installer' / name
    for name, source in source_map.items():
        content = BUNDLE['read_file'](source)
        if name.startswith('profile/') and name.endswith('.sh'):
            content = content.replace(b'\r\n', b'\n')
        installed = BUNDLE['read_file'](payload / name)
        report['files'].append({'file': name, 'source_sha256': sha(content), 'bundle_sha256': sha(installed), 'matches': content == installed})
    report['enrollment'] = BUNDLE['validate_enrollment'](payload / 'enrollment', args.core_key_sha256)
    BUNDLE['validate_public_payload'](payload, PROJECT)
    # A publisher ZIP is an input for comparison only, never recursively extracted.
    if args.publisher_zip:
        with zipfile.ZipFile(args.publisher_zip) as archive:
            for name in BUNDLE['LAYERS']['developer']:
                matches = [item for item in archive.infolist() if not item.is_dir() and item.filename.rsplit('/', 1)[-1] == name]
                if len(matches) != 1 or matches[0].file_size > BUNDLE['MAX_FILE']:
                    raise ValueError('Publisher ZIP missing/duplicate/oversized member: ' + name)
                if archive.read(matches[0]) != BUNDLE['read_file'](args.publisher / name):
                    raise ValueError('Publisher ZIP does not match selected directory: ' + name)
        report['publisher_zip_matches'] = True
    if args.frozen_source:
        names = {'VERSION', 'Makefile'}
        for root in (args.frozen_source, PROJECT):
            names.update(str(p.relative_to(root)).replace('\\', '/') for p in (root / 'src').rglob('*') if p.is_file() and p.suffix in ('.c', '.h'))
        changed = []
        for name in sorted(names):
            current, frozen = PROJECT / name, args.frozen_source / name
            if not current.exists() or not frozen.exists() or current.read_bytes().replace(b'\r\n', b'\n') != frozen.read_bytes().replace(b'\r\n', b'\n'):
                changed.append(name)
        report['unreleased_source_differences'] = changed
        report['bootstrap_source_matches'] = {}
        for name in ('app-daemon-bootstrap.sh', 'device-core-enroll.sh'):
            report['bootstrap_source_matches'][name] = (PROJECT / 'scripts' / name).read_bytes().replace(b'\r\n', b'\n') == (payload / 'enrollment' / name).read_bytes().replace(b'\r\n', b'\n')
    report['mismatches'] = [r['file'] for r in report['files'] if not r['matches']]
    report['checked_files'] = len(report['files'])
    with args.output.open('x', encoding='utf-8', newline='\n') as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
        output.write('\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'files'}, ensure_ascii=False, indent=2))
    return 1 if args.require_match and report['mismatches'] else 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'publisher', 'adb', 'enrollment', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--publisher-zip', type=Path)
    parser.add_argument('--frozen-source', type=Path)
    parser.add_argument('--installer-exe', type=Path)
    parser.add_argument('--core-key-sha256', required=True)
    parser.add_argument('--require-match', action='store_true')
    raise SystemExit(audit(parser.parse_args()))
