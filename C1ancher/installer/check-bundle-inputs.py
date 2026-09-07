#!/usr/bin/env python3
"""Compare a complete bundle to explicitly selected current public build inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy

PROJECT = Path(__file__).resolve().parents[1]
packer = runpy.run_path(str(PROJECT / 'installer/build-bundle.py'))


def check(bundle, enrollment, publisher, executable, adb, output):
    payload = bundle / 'payload'
    state = json.loads(packer['read_file'](bundle / 'BUNDLE-STATUS.json'))
    if state.get('status') != 'READY':
        raise ValueError('Expected complete READY bundle')
    info = packer['validate_enrollment'](payload / 'enrollment', state['enrollment']['core_key_sha256'])
    if info != state['enrollment']:
        raise ValueError('Bundle state does not describe signed enrollment')
    entries = []
    def compare(relative, source, lf=False):
        expected = packer['read_file'](source)
        if lf:
            expected = expected.replace(b'\r\n', b'\n')
        actual = packer['read_file'](bundle / relative)
        if actual != expected:
            raise ValueError('Current input mismatch: ' + relative)
        entries.append({'file': relative, 'size': len(actual), 'sha256': hashlib.sha256(actual).hexdigest(), 'matches_selected_source': True})
    compare('C1SlimInstaller.exe', executable)
    for name in packer['ENROLLMENT']:
        compare('payload/enrollment/' + name, enrollment / name)
    # Confirm signed boot helpers also match the current project, not just the old bundle.
    for name in ('app-daemon-bootstrap.sh', 'device-core-enroll.sh'):
        compare('payload/enrollment/' + name, PROJECT / 'scripts' / name, lf=True)
    for name in packer['LAYERS']['developer']:
        compare('payload/developer/' + name, publisher / name)
    for name in packer['LAYERS']['tools']:
        compare('payload/tools/' + name, adb / name)
    compare('payload/device-setup.sh', PROJECT / 'installer/device-setup.sh', lf=True)
    compare('payload/usb/device-open-adb.sh', PROJECT / 'scripts/device-open-adb.sh', lf=True)
    for name in ('device-repository-config.sh', 'c1-update-check.sh'):
        compare('payload/profile/' + name, PROJECT / 'scripts' / name, lf=True)
    for name in packer['LAYERS']['accessories']:
        source = PROJECT.parent / 'Pic/wallpaper.raw' if name == 'wallpaper.raw' else PROJECT / 'third_party/neofetch' / name
        compare('payload/accessories/' + name, source, lf=('accessories/' + name in packer['SHELL_SOURCES']))
    compare('payload/profile/repository.ed25519.pub', PROJECT / 'config/app-repo/repository.ed25519.pub')
    compare('USER-GUIDE.md', PROJECT / 'installer/USER-GUIDE.md')
    compare('THIRD-PARTY-NOTICES.txt', PROJECT / 'installer/THIRD-PARTY-NOTICES.txt')
    compare('PLATFORM-TOOLS-NOTICE.txt', adb / 'NOTICE.txt')
    packer['validate_public_payload'](payload, PROJECT)
    for root in (bundle, payload):
        manifest = packer['read_file'](root / 'SHA256SUMS').decode('ascii')
        for line in manifest.splitlines():
            digest, name = line.split('  ', 1)
            if hashlib.sha256(packer['read_file'](root / name)).hexdigest() != digest:
                raise ValueError('Checksum mismatch: ' + name)
    result = {'status': 'PASS', 'version': info['version'], 'sequence': info['sequence'],
              'release_sha256': info['release_sha256'], 'checks': entries,
              'scope': 'Offline verification against selected current project, signed enrollment, rebuilt EXE and public publisher tools. No device write or server deployment.'}
    with output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    print(f"PASS: version={info['version']} sequence={info['sequence']}; {len(entries)} source comparisons, signed manifests and public configuration verified.")


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'enrollment', 'publisher', 'executable', 'adb', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    args = p.parse_args()
    check(args.bundle, args.enrollment, args.publisher, args.executable, args.adb, args.output)
