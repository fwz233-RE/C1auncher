#!/usr/bin/env python3
"""Extract exact 16x16 bitmap glyphs from the repository's OFL font; no rasterizer."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
source = root.parent / 'C1ancher/src/pkg/font_generated.h'
chars = sorted({ord(c) for p in (root/'src').glob('*.c') for c in p.read_text(encoding='utf-8') if '\u3000' <= c <= '\uffef'})
rows = {int(cp,16):row for row,cp in re.findall(r'\{([^{}]+)\}, /\* U\+([0-9A-F]+) \*/',source.read_text(encoding='utf-8'))}
missing = [chr(c) for c in chars if c not in rows]
if missing: raise SystemExit(f'Missing bitmap glyphs: {missing}')
text = '/* Ink Wars Bitmap, subset of C1 Package Bitmap / GNU Unifont 16.0.04. SIL OFL 1.1. */\n#include <stdint.h>\n'
text += f'#define IW_GLYPHS {len(chars)}u\nstatic const uint16_t iw_codes[] = {{'+','.join(hex(c) for c in chars)+'};\n'
text += 'static const uint16_t iw_rows[][16] = {\n'+'\n'.join('{'+rows[c]+'},' for c in chars)+'\n};\n'
(root/'src/cjk.h').write_text(text,encoding='utf-8')
license_dir=root/'licenses';license_dir.mkdir(exist_ok=True)
for src,name in [(root.parent/'C1ancher/LICENSE','GPL-3.0.txt'),(root.parent/'C1ancher/third_party/pkg_font/LICENSE.txt','bitmap-font-OFL.txt'),(root.parent/'ChiChuGames/licenses/Adafruit-GFX-BSD.txt','Adafruit-GFX-BSD.txt')]:
 (license_dir/name).write_bytes(src.read_bytes())
print(f'{len(chars)} exact 16x16 bitmap glyphs; all source characters covered')
