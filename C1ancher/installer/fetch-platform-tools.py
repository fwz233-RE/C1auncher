#!/usr/bin/env python3
"""Fetch Google's stable Windows ADB distribution into a NEW isolated directory.
Never changes the installed SDK, starts ADB, or extracts unlisted ZIP members.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

BASE = 'https://dl.google.com/android/repository/'
MEMBERS = ('adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'NOTICE.txt', 'source.properties')


def download(url, limit):
    with urllib.request.urlopen(url, timeout=90) as response:
        if not response.url.startswith(BASE):
            raise ValueError('Unexpected download redirect')
        data = response.read(limit + 1)
    if not data or len(data) > limit:
        raise ValueError('Empty or oversized download')
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    if args.output_dir.exists():
        raise ValueError('Output already exists; choose a new directory')
    metadata = download(BASE + 'repository2-1.xml', 8 * 1024 * 1024)
    root = ET.fromstring(metadata)
    packages = [p for p in root if p.tag.endswith('remotePackage') and p.get('path') == 'platform-tools' and p.find('channelRef').get('ref') == 'channel-0']
    if len(packages) != 1:
        raise ValueError('Expected exactly one stable platform-tools package')
    package = packages[0]
    version = '.'.join(package.findtext('revision/' + p, '0') for p in ('major', 'minor', 'micro'))
    archive = next(a for a in package.findall('archives/archive') if a.findtext('host-os') == 'windows')
    filename = archive.findtext('complete/url')
    if not re.fullmatch(r'platform-tools_r[0-9.]+-win\.zip', filename):
        raise ValueError('Unexpected package filename')
    data = download(BASE + filename, 64 * 1024 * 1024)
    if len(data) != int(archive.findtext('complete/size')) or hashlib.sha1(data).hexdigest() != archive.findtext('complete/checksum'):
        raise ValueError('Package differs from Google HTTPS metadata')
    files = {}
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for name in MEMBERS:
            matches = [i for i in z.infolist() if i.filename == 'platform-tools/' + name]
            if len(matches) != 1 or not 0 < matches[0].file_size <= 32 * 1024 * 1024:
                raise ValueError('Missing, duplicate or oversized member: ' + name)
            files[name] = z.read(matches[0])
    properties = files['source.properties'].decode('utf-8')
    if not re.search(r'^Pkg\.Revision\s*=\s*' + re.escape(version) + r'\s*$', properties, re.M):
        raise ValueError('Package version differs from Google metadata')
    report = {'version': version, 'url': BASE + filename, 'zip_sha256': hashlib.sha256(data).hexdigest(),
              'metadata_sha256': hashlib.sha256(metadata).hexdigest(),
              'files': {name: hashlib.sha256(content).hexdigest() for name, content in files.items()}}
    args.output_dir.mkdir(parents=True, exist_ok=False)
    for name, content in files.items():
        with (args.output_dir / name).open('xb') as stream:
            stream.write(content)
    with (args.output_dir / 'DOWNLOAD-RESULT.json').open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
