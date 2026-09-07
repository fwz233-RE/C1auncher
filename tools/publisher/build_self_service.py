"""Build and package self-service publisher 1.1.0 from public source.

python tools/publisher/build_self_service.py --public-key PATH --output-dir PATH
Requires Go 1.24+. Runs tests/vet, cross-compiles Windows/Linux clients and
Linux server, and packages an explicit client-only file list. No network
publication or production credentials are needed. Output must be new.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import stat
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'C1ancher-server'
VERSION = '1.1.0'
ASSET = f'C1Slim-Publisher-{VERSION}.zip'
PUBLIC_HASH = 'f18a060a6125bd96e877f5518e9f44c249e17cda12f87202eb0008056ab3cd27'
VALIDATE = runpy.run_path(str(Path(__file__).with_name('package_release.py')))['validate_binary']


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--public-key', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--go', default='go')
    args = p.parse_args()
    public = args.public_key.read_bytes()
    if len(public) != 32 or sha(public) != PUBLIC_HASH:
        p.error('Expected the existing official raw public key; no new trust root is generated')
    out = args.output_dir.resolve()
    if out.exists():
        p.error('Output exists; use a new directory')
    env = os.environ.copy()
    for key in ('GOOS', 'GOARCH', 'GOFLAGS', 'GOAMD64', 'GOARM64'):
        env.pop(key, None)
    env['CGO_ENABLED'] = '0'
    for command in ([args.go, 'test', './...'], [args.go, 'vet', './...']):
        subprocess.run(command, cwd=SOURCE, env=env, check=True)
    out.mkdir(parents=True)
    client = out / 'C1-Publisher'
    client.mkdir()
    targets = [('windows', 'amd64', 'c1publish.exe'), ('linux', 'amd64', 'c1publish-linux-amd64'), ('linux', 'arm64', 'c1publish-linux-arm64')]
    files = {}
    for system, arch, name in targets:
        build_env = dict(env, GOOS=system, GOARCH=arch)
        if arch == 'amd64':
            build_env['GOAMD64'] = 'v1'
        path = client / name
        subprocess.run([args.go, 'build', '-buildvcs=false', '-trimpath', '-ldflags=-s -w', '-o', str(path), './cmd/c1publish'], cwd=SOURCE, env=build_env, check=True)
        files[name] = path.read_bytes()
        VALIDATE(name, files[name])
        if system == 'linux':
            path.chmod(0o755)
    server = out / 'c1repo-linux-amd64'
    subprocess.run([args.go, 'build', '-buildvcs=false', '-trimpath', '-ldflags=-s -w', '-o', str(server), './cmd/c1repo'], cwd=SOURCE, env=dict(env, GOOS='linux', GOARCH='amd64', GOAMD64='v1'), check=True)
    VALIDATE(server.name, server.read_bytes())
    if os.name == 'nt':
        reported = subprocess.check_output([str(client / 'c1publish.exe'), '-tool-version'], text=True).strip()
        if reported != 'C1-Slim Publisher ' + VERSION:
            raise ValueError('Unexpected client version: ' + reported)
    for name in ('README.md', 'DISTRIBUTION-NOTE.txt', 'THIRD_PARTY_NOTICES.txt'):
        files[name] = (ROOT / 'tools/publisher' / name).read_text(encoding='utf-8').encode('utf-8')
    files['GUIDE.zh-CN.md'] = (ROOT / 'docs/publishing.md').read_text(encoding='utf-8').encode('utf-8')
    files['LICENSE'] = (ROOT / 'C1ancher/LICENSE').read_bytes()
    files['server.url'] = b'http://www.fwz233.com\n'
    files['repository.ed25519.pub'] = public
    source_sums = {}
    for base in ('cmd', 'internal'):
        for path in sorted((SOURCE / base).rglob('*.go')):
            source_sums[path.relative_to(SOURCE).as_posix()] = sha(path.read_bytes().replace(b'\r\n', b'\n'))
    source_sums['go.mod'] = sha((SOURCE / 'go.mod').read_bytes().replace(b'\r\n', b'\n'))
    files['BUILD-INFO.json'] = (json.dumps({'version': VERSION, 'go': subprocess.check_output([args.go, 'version'], text=True).strip(), 'sourceSHA256': source_sums}, indent=2) + '\n').encode()
    files['SHA256SUMS'] = ''.join(f'{sha(data)}  {name}\n' for name, data in sorted(files.items())).encode()
    for name, data in files.items():
        if not (client / name).exists():
            (client / name).write_bytes(data)
    archive = out / ASSET
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo('C1-Publisher/' + name, (2026, 9, 7, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | (0o755 if name.startswith('c1publish') else 0o644)) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        assert set(z.namelist()) == {'C1-Publisher/' + n for n in files}
        for name, data in files.items():
            assert z.read('C1-Publisher/' + name) == data
    (out / (ASSET + '.sha256')).write_text(f'{sha(archive.read_bytes())}  {ASSET}\n', encoding='ascii')
    (out / 'build-result.json').write_text(json.dumps({'archive': str(archive), 'archiveSHA256': sha(archive.read_bytes()), 'serverSHA256': sha(server.read_bytes()), 'clientMembers': sorted(files)}, indent=2), encoding='utf-8')
    print('Verified self-service publisher package:', archive)
    print('Server SHA256:', sha(server.read_bytes()))


if __name__ == '__main__':
    main()
