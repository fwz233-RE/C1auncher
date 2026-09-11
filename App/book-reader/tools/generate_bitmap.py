"""Copy the package manager's exact OFL Unifont pixels into an embedded font.
No book contents or machine-local fonts are used. Run from any directory.
"""
from pathlib import Path
import re
import struct
import hashlib

root = Path(__file__).resolve().parents[1]
repo = root.parents[1]
source = repo / 'C1ancher/src/pkg/font_generated.h'
text = source.read_text(encoding='utf-8')
ranges = [(int(a,16),int(b,16),int(c)) for a,b,c in re.findall(r'\{0x([0-9a-f]+)U, 0x([0-9a-f]+)U, ([0-9]+)U\}',text)]
width_text = text.split('c1pkg_font_widths[] = {')[1].split('};')[0]
widths = [int(x) for x in re.findall(r'\d+',width_text)]
row_text = text.split('c1pkg_font_rows[][16] = {')[1].split('};')[0]
rows = [int(x,16) for x in re.findall(r'0x([0-9a-fA-F]+)',row_text)]
assert len(rows)==16*len(widths)
data=bytearray(b'C1BF')
for lo,hi,start in ranges:
    for cp in range(lo,hi+1):
        index=start+cp-lo
        data += struct.pack('<IB',cp,widths[index])
        data += struct.pack('>16H',*rows[index*16:index*16+16])
out=root/'assets/pkg-font.bin'
out.parent.mkdir(parents=True,exist_ok=True)
out.write_bytes(data)
(root/'assets/font-LICENSE.txt').write_bytes((repo/'C1ancher/third_party/pkg_font/LICENSE.txt').read_bytes())
print(f'{len(widths)} glyphs, {len(data)} bytes, SHA256 {hashlib.sha256(data).hexdigest()}')
