#!/usr/bin/env python3
"""Create a new ZIP from an assembled bundle after checking every listed file."""
from pathlib import Path
import argparse
import hashlib
import re
import runpy
import zipfile

helpers = runpy.run_path(str(Path(__file__).with_name('build-bundle.py')))


def archive(bundle: Path, output: Path) -> None:
    bundle = helpers['safe_path'](bundle)
    output = helpers['safe_path'](output, missing=True)
    if output.exists() or output.is_relative_to(bundle):
        raise ValueError('Choose a new archive path outside the bundle directory')
    manifest = helpers['read_file'](bundle / 'SHA256SUMS', 65536)
    records = {}
    for line in manifest.decode('ascii').splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9_./-]+)', line)
        if not match:
            raise ValueError('Invalid bundle checksum record')
        digest, name = match.groups()
        if name.startswith('/') or any(p in ('', '.', '..') for p in name.split('/')) or name.lower() in records:
            raise ValueError('Invalid or duplicate bundle path')
        records[name.lower()] = (name, digest)
    helpers['exact_tree'](bundle, {v[0] for v in records.values()} | {'SHA256SUMS'})
    # Validate before creating an archive; recheck the exact bytes being archived.
    for name, digest in records.values():
        if hashlib.sha256(helpers['read_file'](bundle / name)).hexdigest() != digest:
            raise ValueError('Bundle checksum mismatch: ' + name)
    output.parent.mkdir(parents=True, exist_ok=True)
    created = False
    try:
        with zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as result:
            created = True
            result.writestr(bundle.name + '/SHA256SUMS', manifest)
            for name, digest in records.values():
                data = helpers['read_file'](bundle / name)
                if hashlib.sha256(data).hexdigest() != digest:
                    raise ValueError('Bundle changed while archiving: ' + name)
                result.writestr(bundle.name + '/' + name, data)
        with zipfile.ZipFile(output) as result:
            if result.testzip() is not None:
                raise ValueError('ZIP integrity check failed')
    except BaseException:
        if created:
            output.unlink()
        raise
    with output.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    print(f'Archive: {output}')
    print(f'Size: {output.stat().st_size} bytes')
    print(f'SHA256: {digest}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle-dir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    archive(args.bundle_dir, args.output)
