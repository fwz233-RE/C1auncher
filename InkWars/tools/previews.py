#!/usr/bin/env python3
"""PBM to exact 1-bit PNG and nearest-neighbor contact sheet (host tooling only)."""
from pathlib import Path
from PIL import Image, ImageDraw
import sys
root=Path(sys.argv[1] if len(sys.argv)>1 else 'build/previews')
files=sorted(root.glob('*.pbm'))
if not files: raise SystemExit('No PBM frames found')
images=[]
for path in files:
 im=Image.open(path)
 assert im.size==(296,152) and im.mode=='1', (path,im.size,im.mode)
 assert set(im.getdata()) <= {0,255}
 im.save(path.with_suffix('.png'))
 images.append((path.stem,im))
scale=2;cellw=608;cellh=332
sheet=Image.new('1',(cellw*3,cellh*((len(images)+2)//3)),1);draw=ImageDraw.Draw(sheet)
for i,(name,im) in enumerate(images):
 x=(i%3)*cellw+8;y=(i//3)*cellh
 draw.text((x,y+2),name,fill=0)
 sheet.paste(im.resize((592,304),getattr(Image, 'Resampling', Image).NEAREST),(x,y+20))
sheet.save(root/'contact.png')
print(f'{len(images)} verified 296x152 1-bit previews and contact.png')
