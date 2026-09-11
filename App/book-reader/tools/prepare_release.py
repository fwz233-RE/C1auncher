"""Freeze an allowlisted reader payload and corresponding source; never include books."""
import argparse, gzip, hashlib, io, json, tarfile, time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
REPO=ROOT.parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--version', required=True)
a=p.parse_args()
import re
if not re.fullmatch(r'\d+\.\d+\.\d+',a.version):p.error('numeric semantic version required')
build=REPO/'build'/('book-reader-'+a.version)
base=build/'payload'
allowed={'book-reader','README.md','CHANGELOG.md','THIRD_PARTY_NOTICES.md','VALIDATION.md','LICENSE','font-LICENSE.txt'}
actual={x.relative_to(base).as_posix() for x in base.rglob('*') if x.is_file()}
if actual!=allowed:raise ValueError(f'Unexpected/incomplete build payload: {actual ^ allowed}')
stage=build/('publish-'+time.strftime('%Y%m%d-%H%M%S'))
payload=stage/'payload'
payload.mkdir(parents=True,exist_ok=False)
for name in sorted(allowed):
    dest=payload/name
    dest.write_bytes((base/name).read_bytes())
    dest.chmod(0o755 if name=='book-reader' else 0o644)
# Include only source and licensed font inputs. No test output, private books,
# runtime caches, signing material or device evidence can enter the archive.
files=[]
for folder in [ROOT,REPO/'App/c1device']:
    for f in folder.rglob('*'):
        rel=f.relative_to(folder)
        if not f.is_file() or any(x in {'build','assets','__pycache__','.git'} or x.startswith('build-') for x in rel.parts):continue
        if f.suffix in {'.go','.md','.ps1','.py'} or f.name in {'go.mod','go.sum'}:files.append(f)
files += [ROOT/'assets/pkg-font.bin',ROOT/'assets/font-LICENSE.txt',REPO/'LICENSE',REPO/'C1ancher/LICENSE',
          REPO/'C1ancher/src/pkg/font_generated.h',REPO/'C1ancher/third_party/pkg_font/LICENSE.txt']
raw=io.BytesIO()
with tarfile.open(fileobj=raw,mode='w',format=tarfile.GNU_FORMAT) as tar:
    for f in sorted(set(files)):
        data=f.read_bytes()
        entry=tarfile.TarInfo('source/'+f.relative_to(REPO).as_posix())
        entry.size=len(data);entry.mode=0o644;entry.mtime=0
        tar.addfile(entry,io.BytesIO(data))
archive=gzip.compress(raw.getvalue(),mtime=0)
(payload/f'book-reader-source-{a.version}.tar.gz').write_bytes(archive)
report=[]
for f in sorted(payload.rglob('*')):
    if f.is_file():
        data=f.read_bytes();report.append({'path':f.relative_to(payload).as_posix(),'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
(stage/'payload-hashes.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(payload)
print(json.dumps(report,indent=2))
