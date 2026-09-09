#!/usr/bin/env python3
"""Ship corresponding source, including only the reused repository dependencies."""
from pathlib import Path
import gzip, tarfile, io, hashlib, re
ROOT=Path(__file__).resolve().parents[1]
VERSION=re.search(r'#define IW_VERSION "([0-9.]+)"', (ROOT/'src/version.h').read_text()).group(1)
files=[]
for folder in ('src','tests','tools','licenses'):
 files.extend(p for p in (ROOT/folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts and p.suffix!='.pyc')
files.extend(ROOT/n for n in ('README.md','TESTING.md','KNOWN_ISSUES.md','CHANGELOG.md','LICENSE','Makefile','launch.sh','build.ps1','publish.ps1','.gitignore','.gitattributes'))
for name in ('canvas.c','canvas.h','font.c','font.h','font_data.c','font_gen.h'):
 files.append(ROOT.parent/'ChiChuGames/src/gfx'/name)
files.extend(ROOT.parent/n for n in ('C1ancher/src/platform/app_lease.c','C1ancher/config/app-repo/repository.ed25519.pub','C1ancher/src/platform/app_lease.h','ChiChuGames/src/config.h','ChiChuGames/licenses/Adafruit-GFX-BSD.txt','C1ancher/LICENSE','C1ancher/src/pkg/font_generated.h','C1ancher/third_party/pkg_font/LICENSE.txt'))
output=ROOT/f'build/inkwars-source-{VERSION}.tar.gz';output.parent.mkdir(exist_ok=True)
with output.open('wb') as raw:
 with gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0) as zipped:
  with tarfile.open(fileobj=zipped,mode='w',format=tarfile.GNU_FORMAT) as tar:
   for path in sorted(set(files)):
    data=path.read_bytes()
    if path.suffix in ('.sh','.py','.c','.h','.md','.txt') or path.name in ('Makefile','LICENSE'):data=data.replace(b'\r\n',b'\n')
    info=tarfile.TarInfo(path.relative_to(ROOT.parent).as_posix());info.size=len(data);info.mode=0o755 if path.suffix=='.sh' else 0o644;info.mtime=0
    tar.addfile(info,io.BytesIO(data))
(output.parent/(output.name+'.sha256')).write_text(hashlib.sha256(output.read_bytes()).hexdigest()+'  '+output.name+'\n',encoding='ascii')
print(f'Corresponding source: {output} ({len(set(files))} files, {output.stat().st_size} bytes)')
