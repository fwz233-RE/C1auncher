"""Convert deterministic firmware-rendered PBM previews; never accesses a device."""
import argparse
from pathlib import Path
from PIL import Image, ImageDraw

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path)
args = parser.parse_args()
names = ['wifi-list', 'wifi-more-networks', 'wifi-connecting', 'wifi-stopping',
         'wifi-off', 'wifi-password', 'wifi-symbols', 'wifi-validation']
sheet = Image.new('RGB', (1240, ((len(names) + 1) // 2) * 350 + 20), 'white')
draw = ImageDraw.Draw(sheet)
for index, name in enumerate(names):
    with Image.open(args.directory / (name + '.pbm')) as image:
        assert image.size == (296, 152)
        image.save(args.directory / (name + '.png'))
        large = image.resize((592, 304), Image.Resampling.NEAREST)
        large.save(args.directory / (name + '-2x.png'))
        x, y = 20 + (index % 2) * 620, 35 + (index // 2) * 350
        draw.text((x, y - 20), name, fill='black')
        sheet.paste(large, (x, y))
        draw.rectangle((x-1, y-1, x+592, y+304), outline='black')
sheet.save(args.directory / 'wifi-preview-sheet.png')
print(args.directory / 'wifi-preview-sheet.png')
