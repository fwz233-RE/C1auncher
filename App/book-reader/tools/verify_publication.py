"""Verify Ed25519 repository signature and every byte of a published reader payload."""
import argparse, hashlib, io, json, tarfile, urllib.request
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

ROOT=Path(__file__).resolve().parents[1]
REPO=ROOT.parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--version',required=True)
p.add_argument('--stage',required=True,type=Path)
a=p.parse_args()
public=(REPO/'C1ancher/config/app-repo/repository.ed25519.pub').read_bytes()
errors=[]
for origin in ('http://www.fwz233.com','http://123.56.214.77'):
    try:
        def fetch(path,limit):
            with urllib.request.urlopen(origin+path,timeout=60) as response:data=response.read(limit+1)
            if len(data)>limit:raise ValueError('response exceeds size limit')
            return data
        index=fetch('/c1/v2/index.v1',256*1024)
        sig=fetch('/c1/v2/index.v1.sig',64)
        Ed25519PublicKey.from_public_bytes(public).verify(sig,index)
        lines=index.decode().splitlines()
        assert lines[0]=='C1PKG-INDEX 2'
        entries=[x.split('\t') for x in lines[2:]]
        row=next(x for x in entries if len(x)==9 and x[1]=='book-reader')
        assert row[2]==a.version and row[7]=='book-reader' and row[8]=='fwz233'
        assert row[4]=='objects/'+row[5]+'.tar.gz'
        data=fetch('/c1/v2/'+row[4],32*1024*1024)
        assert len(data)==int(row[6]) and hashlib.sha256(data).hexdigest()==row[5]
        break
    except Exception as exc:errors.append(str(exc))
else:raise SystemExit('Public verification failed: '+'; '.join(errors))
expected={f.relative_to(a.stage/'payload').as_posix():f.read_bytes() for f in (a.stage/'payload').rglob('*') if f.is_file()}
manifest=f'C1PKG-PACKAGE 2\nid\tbook-reader\nversion\t{a.version}\nentry\tbook-reader\nmode\tdirect\n'.encode()
with tarfile.open(fileobj=io.BytesIO(data),mode='r:gz') as tar:
    matched=set();manifest_seen=False
    for m in tar:
        if m.isdir():continue
        assert m.isfile()
        value=tar.extractfile(m).read()
        if m.name=='manifest.v1':
            assert not manifest_seen and value==manifest;manifest_seen=True
        else:
            assert m.name.startswith('payload/')
            name=m.name[8:]
            assert name not in matched and name in expected and value==expected[name]
            matched.add(name)
    assert manifest_seen and matched==set(expected)
report={'verified':True,'signature_verified':True,'origin':origin,'sequence':int(lines[1].split('\t')[1]),
        'version':a.version,'mode':'direct','bytes':len(data),'sha256':row[5],'matched_payload_files':len(expected)}
(a.stage/'published.tar.gz').write_bytes(data)
(a.stage/'server-verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report,indent=2))
