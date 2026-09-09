#!/usr/bin/env python3
"""Convert test_terrain's real 296x152 canvas PBMs; never reimplement terrain.

Run build/test_terrain first, then python3 tools/terrain_preview.py.
The native PNGs remain mode 1; inspection enlargements use nearest neighbor.
"""
from pathlib import Path
import argparse
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path, default=Path("build/previews"))
    args = parser.parse_args()
    for name in ("terrain-map", "terrain-atlas"):
        source = args.directory / (name + ".pbm")
        with Image.open(source) as image:
            assert image.size == (296, 152) and image.mode == "1", source
            image.save(source.with_suffix(".png"))
            image.resize((1184, 608), getattr(Image, "Resampling", Image).NEAREST).save(
                args.directory / (name + "-4x.png")
            )
        print(f"{source}: native 296x152 mode-1 PNG + nearest-neighbor 4x PNG")


if __name__ == "__main__":
    main()
