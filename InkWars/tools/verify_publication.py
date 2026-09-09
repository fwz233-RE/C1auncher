#!/usr/bin/env python3
"""Verify the signed public index, mode-bearing archive and staged file bytes."""
import argparse, hashlib, io, json, tarfile, urllib.request
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stage', required=True, type=Path)
parser.add_argument('--version', required=True)
parser.add_argument('--sha256', required=True)
args = parser.parse_args()
stage = args.stage.resolve()
errors = []
for origin in ('http://www.fwz233.com', 'http://123.56.214.77'):
    try:
        def fetch(path, limit):
            with urllib.request.urlopen(origin + path, timeout=60) as response:
                data = response.read(limit + 1)
            if len(data) > limit: raise ValueError('Response too large')
            return data
        index = fetch('/c1/v2/index.v1', 256 * 1024)
        signature = fetch('/c1/v2/index.v1.sig', 64)
        public = (ROOT.parent/'C1ancher/config/app-repo/repository.ed25519.pub').read_bytes()
        Ed25519PublicKey.from_public_bytes(public).verify(signature, index)
        break
    except Exception as exc:
        errors.append(str(exc))
else:
    raise SystemExit('Verified repository unavailable: ' + '; '.join(errors))
lines = index.decode('utf-8').splitlines()
assert lines[0] == 'C1PKG-INDEX 2'
entry = next(line.split('\t') for line in lines[2:] if line.split('\t')[1] == 'inkwars')
assert len(entry) == 9 and entry[2] == args.version and entry[7] == 'inkwars' and entry[8] == 'fwz233'
assert entry[5] == args.sha256
assert entry[4] == 'objects/' + args.sha256 + '.tar.gz'
data = fetch('/c1/v2/' + entry[4], 32 * 1024 * 1024)
assert len(data) == int(entry[6]) and hashlib.sha256(data).hexdigest() == args.sha256
expected_files = {p.relative_to(stage/'payload').as_posix(): p.read_bytes()
                  for p in (stage/'payload').rglob('*') if p.is_file()}
manifest = f'C1PKG-PACKAGE 2\nid\tinkwars\nversion\t{args.version}\nentry\tinkwars\nmode\tdirect\n'
with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tar:
    assert tar.extractfile('manifest.v1').read() == manifest.encode('ascii')
    matched = set()
    for member in tar:
        assert member.isdir() or member.isfile()
        if member.isfile() and member.name.startswith('payload/'):
            name = member.name[len('payload/'):]
            assert name in expected_files and name not in matched
            assert tar.extractfile(member).read() == expected_files[name]
            matched.add(name)
    assert matched == set(expected_files)
report = {'verified': True, 'signature_verified': True, 'origin': origin,
          'sequence': int(lines[1].split('\t')[1]), 'version': args.version,
          'mode': 'direct', 'entry': 'inkwars', 'author': entry[8],
          'bytes': len(data), 'matched_payload_files': len(matched), 'sha256': args.sha256}
(stage/'server-verification.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
(stage/'published.tar.gz').write_bytes(data)
print(json.dumps(report, indent=2))
