#!/usr/bin/env python3
"""Prepare a fresh allowlisted payload for the Windows publisher EXE.
Includes matching corresponding source for offline GPL compliance.
"""
from pathlib import Path
import hashlib
import json
import tarfile
import time
import re

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.search(r'#define IW_VERSION "([0-9.]+)"', (ROOT/'src/version.h').read_text()).group(1)
archive = ROOT / f'build/package/packages/inkwars/{VERSION}.tar.gz'
source = ROOT / f'build/inkwars-source-{VERSION}.tar.gz'
output = ROOT / 'build' / ('publish-' + VERSION + '-' + time.strftime('%Y%m%d-%H%M%S'))
payload = output / 'payload'
payload.mkdir(parents=True, exist_ok=False)
allowed = {'launch.sh', 'inkwars', 'README.md', 'TESTING.md', 'KNOWN_ISSUES.md', 'CHANGELOG.md',
           'LICENSE', 'licenses/Adafruit-GFX-BSD.txt', 'licenses/bitmap-font-OFL.txt',
           'licenses/GPL-3.0.txt'}
seen = set()
with tarfile.open(archive, 'r:gz') as tar:
    expected_manifest = f'C1PKG-PACKAGE 2\nid\tinkwars\nversion\t{VERSION}\nentry\tinkwars\nmode\tdirect\n'
    if tar.extractfile('manifest.v1').read() != expected_manifest.encode('ascii'):
        raise ValueError('Publication requires an authenticated direct-mode manifest')
    for member in tar.getmembers():
        if member.isdir() or member.name == 'manifest.v1':
            continue
        if not member.isfile() or not member.name.startswith('payload/'):
            raise ValueError('Unexpected package member')
        name = member.name[len('payload/'):]
        if name not in allowed or name in seen:
            raise ValueError('Unexpected or duplicate payload file: ' + name)
        seen.add(name)
        path = payload / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(tar.extractfile(member).read())
        path.chmod(0o755 if name in ('launch.sh', 'inkwars') else 0o644)
if seen != allowed:
    raise ValueError('Incomplete release payload')
if (payload / 'inkwars').read_bytes() != (ROOT / 'build/inkwars').read_bytes():
    raise ValueError('Packaged executable differs from tested executable')
(payload / source.name).write_bytes(source.read_bytes())
manifest = []
for path in sorted(payload.rglob('*')):
    if path.is_file():
        data = path.read_bytes()
        manifest.append({'path': path.relative_to(payload).as_posix(), 'bytes': len(data),
                         'sha256': hashlib.sha256(data).hexdigest()})
(output / 'payload-hashes.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
print(str(payload))
print(json.dumps(manifest, indent=2))
