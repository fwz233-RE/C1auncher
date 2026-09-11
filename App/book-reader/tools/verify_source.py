"""Rebuild the exact staged source in an isolated directory and compare MIPS ELF."""
import argparse, hashlib, os, subprocess, tarfile, tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--stage',required=True,type=Path)
p.add_argument('--version',required=True)
a=p.parse_args()
go=Path(r'C:\Program Files\Go\bin\go.exe')
with tempfile.TemporaryDirectory(prefix='reader-source-') as folder:
    root=Path(folder)
    with tarfile.open(a.stage/'payload'/f'book-reader-source-{a.version}.tar.gz') as tar:
        for m in tar:
            assert m.isfile() and m.name.startswith('source/') and '..' not in Path(m.name).parts
            dest=root/m.name;dest.parent.mkdir(parents=True,exist_ok=True)
            dest.write_bytes(tar.extractfile(m).read())
    app=root/'source/App/book-reader'
    env=dict(os.environ)
    for key in list(env):
        if key.startswith('C1_') or key.startswith('BOOK_READER_TEST'):env.pop(key)
    env.update(GOOS='windows',GOARCH='amd64',CGO_ENABLED='0')
    subprocess.run([str(go),'test','./...'],cwd=app,env=env,check=True)
    subprocess.run([str(go),'vet','./...'],cwd=app,env=env,check=True)
    env.update(GOOS='linux',GOARCH='mipsle',GOMIPS='hardfloat')
    binary=root/'book-reader'
    subprocess.run([str(go),'build','-trimpath','-ldflags',f'-s -w -X main.version={a.version}','-o',str(binary),'.'],cwd=app,env=env,check=True)
    data=binary.read_bytes()
    assert data==(a.stage/'payload/book-reader').read_bytes(),'corresponding-source ELF mismatch'
    print('Isolated source tests, vet and byte-identical MIPS rebuild PASS: '+hashlib.sha256(data).hexdigest())
