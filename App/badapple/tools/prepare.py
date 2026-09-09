#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert local numbered PNG/JPEG frames to C1BA0001, without video decoding.

Names must end in a decimal frame number (e.g. 0001.png or frame_0001.jpg).
Numbers determine order, not timestamps; gaps are allowed, duplicates are not.
All files in a directory tree, or ZIP members, are inspected without extraction.
Only frames selected for output are decoded. No network access is performed.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import io
import math
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import tempfile
import warnings
import zipfile

from PIL import Image, ImageDraw, ImageOps, UnidentifiedImageError

WIDTH, HEIGHT = 296, 152
FRAME_BYTES = WIDTH * HEIGHT // 8
MAGIC = b"C1BA0001"
HEADER = struct.Struct("<8sHHIIIII")
MAX_PIXELS = 16_000_000
MAX_INPUT_BYTES = 32 * 1024 * 1024
MAX_TOTAL_BYTES = 2 * 1024 * 1024 * 1024
MAX_ENTRIES = 100_000
MAX_ZIP_BYTES = 2 * 1024 * 1024 * 1024
MAX_ZIP_RATIO = 200
EXTENSIONS = {".png", ".jpg", ".jpeg"}
NUMBER = re.compile(r"([0-9]+)$")


class ConversionError(ValueError):
    """An input or option cannot safely be converted."""


def validate_rates(source_fps: int, fps: int) -> None:
    if not 1 <= source_fps <= 0xFFFFFFFF:
        raise ConversionError("source fps must be an integer from 1 to 4294967295")
    if not 1 <= fps <= 30:
        raise ConversionError("output fps must be from 1 to 30")
    if fps > source_fps:
        raise ConversionError("output fps must not exceed source fps")


def select_indices(frame_count: int, source_fps: int, fps: int) -> list[int]:
    """Sample at k/fps while that time precedes frame_count/source_fps.

    The last output interval can extend beyond the source by less than 1/fps.
    Integer arithmetic prevents drift for non-divisible frame rates.
    """
    validate_rates(source_fps, fps)
    if not 1 <= frame_count <= MAX_ENTRIES:
        raise ConversionError(f"frame count must be from 1 to {MAX_ENTRIES}")
    count = (frame_count * fps + source_fps - 1) // source_fps
    return [k * source_fps // fps for k in range(count)]


def render_frame(image: Image.Image, threshold: int = 128) -> Image.Image:
    """Fit without cropping, composite transparency on white, then threshold."""
    if not 0 <= threshold <= 255:
        raise ConversionError("threshold must be from 0 to 255")
    if image.width < 1 or image.height < 1 or image.width * image.height > MAX_PIXELS:
        raise ConversionError(f"source images must contain at most {MAX_PIXELS} pixels")
    image = ImageOps.exif_transpose(image)
    if "A" in image.getbands() or "transparency" in image.info:
        rgba = image.convert("RGBA")
        white = Image.new("RGBA", image.size, "white")
        image = Image.alpha_composite(white, rgba)
    image = image.convert("L")
    if image.width * HEIGHT >= image.height * WIDTH:
        size = (WIDTH, max(1, image.height * WIDTH // image.width))
    else:
        size = (max(1, image.width * HEIGHT // image.height), HEIGHT)
    image = image.resize(size, Image.Resampling.LANCZOS)
    canvas = Image.new("L", (WIDTH, HEIGHT), 255)
    canvas.paste(image, ((WIDTH - size[0]) // 2, (HEIGHT - size[1]) // 2))
    # Values strictly below the threshold are black. There is no dithering.
    return canvas.point([0 if value < threshold else 255 for value in range(256)])


def pack_frame(image: Image.Image) -> bytes:
    """Pack black=1 at (y//8)*296+x, bit 0x80>>(y%8)."""
    if image.size != (WIDTH, HEIGHT) or image.mode != "L":
        raise ConversionError("packing requires a 296x152 grayscale thresholded image")
    pixels = image.tobytes()
    result = bytearray(FRAME_BYTES)
    for y in range(HEIGHT):
        row = (y // 8) * WIDTH
        mask = 0x80 >> (y % 8)
        for x in range(WIDTH):
            value = pixels[y * WIDTH + x]
            if value == 0:
                result[row + x] |= mask
            elif value != 255:
                raise ConversionError("packing requires pixels equal to 0 or 255")
    return bytes(result)


@dataclass(frozen=True)
class Frame:
    name: str
    member: Path | zipfile.ZipInfo
    number: int


def numbered_frame(name: str, member: Path | zipfile.ZipInfo) -> Frame | None:
    path = PurePosixPath(name)
    if path.suffix.lower() not in EXTENSIONS:
        return None
    match = NUMBER.search(path.stem)
    if match is None or len(match.group(1)) > 20:
        raise ConversionError(f"image name must end in a frame number (up to 20 digits): {name}")
    return Frame(name, member, int(match.group(1)))


def ordered_frames(frames: list[Frame]) -> list[Frame]:
    if not frames:
        raise ConversionError("input contains no numbered PNG/JPEG frames")
    frames.sort(key=lambda frame: (frame.number, frame.name))
    for previous, current in zip(frames, frames[1:]):
        if previous.number == current.number:
            raise ConversionError(f"duplicate frame number: {previous.name} and {current.name}")
    return frames


def check_size(size: int, name: str) -> None:
    if size < 1 or size > MAX_INPUT_BYTES:
        raise ConversionError(f"input frame size must be 1..{MAX_INPUT_BYTES} bytes: {name}")


def scan_directory(root: Path) -> list[Frame]:
    frames: list[Frame] = []
    count = total = 0

    def walk_error(error: OSError) -> None:
        raise error

    for directory, dirs, files in os.walk(root, followlinks=False, onerror=walk_error):
        for name in sorted(dirs + files):
            count += 1
            if count > MAX_ENTRIES:
                raise ConversionError("too many directory entries")
            path = Path(directory) / name
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(path, "is_junction", lambda: False)():
                raise ConversionError(f"symlinks/junctions are not allowed: {path}")
            if stat.S_ISDIR(info.st_mode):
                continue
            if not stat.S_ISREG(info.st_mode):
                raise ConversionError(f"input must contain only regular files: {path}")
            frame = numbered_frame(path.relative_to(root).as_posix(), path)
            if frame is not None:
                check_size(info.st_size, frame.name)
                total += info.st_size
                if total > MAX_TOTAL_BYTES:
                    raise ConversionError("total source frame size exceeds limit")
                frames.append(frame)
    return ordered_frames(frames)


def scan_zip(archive: zipfile.ZipFile) -> list[Frame]:
    entries = archive.infolist()
    if len(entries) > MAX_ENTRIES:
        raise ConversionError("too many ZIP entries")
    frames: list[Frame] = []
    names: set[str] = set()
    total = 0
    for entry in entries:
        name = entry.filename
        # Check the original name too: ZipInfo truncates names at embedded NULs.
        parts = name.rstrip("/").split("/")
        if (not name or entry.orig_filename != name or "\\" in name or ":" in name
                or any(part in ("", ".", "..") for part in parts)):
            raise ConversionError(f"unsafe ZIP path: {name!r}")
        if name in names:
            raise ConversionError(f"duplicate ZIP member: {name}")
        names.add(name)
        mode = entry.external_attr >> 16
        kind = stat.S_IFMT(mode)
        if kind not in (0, stat.S_IFREG, stat.S_IFDIR):
            raise ConversionError(f"ZIP symlinks/special files are not allowed: {name}")
        if entry.flag_bits & 1:
            raise ConversionError(f"encrypted ZIP entries are not supported: {name}")
        if entry.file_size > MAX_INPUT_BYTES:
            raise ConversionError(f"ZIP member exceeds size limit: {name}")
        total += entry.file_size
        if total > MAX_TOTAL_BYTES:
            raise ConversionError("uncompressed ZIP size exceeds limit")
        if entry.file_size > MAX_ZIP_RATIO * max(1, entry.compress_size):
            raise ConversionError(f"suspicious ZIP compression ratio: {name}")
        if entry.is_dir():
            continue
        frame = numbered_frame(name, entry)
        if frame is not None:
            check_size(entry.file_size, name)
            frames.append(frame)
    return ordered_frames(frames)


@contextmanager
def open_source(path: Path):
    if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
        raise ConversionError("input must not be a symlink or junction")
    if path.is_dir():
        yield scan_directory(path), None
    elif path.is_file() and path.suffix.lower() == ".zip":
        if path.stat().st_size > MAX_ZIP_BYTES:
            raise ConversionError("ZIP archive exceeds size limit")
        with zipfile.ZipFile(path) as archive:
            yield scan_zip(archive), archive
    else:
        raise ConversionError("input must be a local directory or ZIP archive")


def decode_frame(frame: Frame, archive: zipfile.ZipFile | None, threshold: int) -> bytes:
    if archive is None:
        path = Path(frame.member)
        if path.is_symlink() or not path.is_file():
            raise ConversionError(f"input frame is no longer a regular file: {frame.name}")
        stream = path.open("rb")
    else:
        stream = archive.open(frame.member)
    with stream:
        data = stream.read(MAX_INPUT_BYTES + 1)
    check_size(len(data), frame.name)
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            with Image.open(io.BytesIO(data)) as image:
                if image.format not in {"PNG", "JPEG"}:
                    raise ConversionError(f"frame is not PNG/JPEG: {frame.name}")
                if image.width * image.height > MAX_PIXELS:
                    raise ConversionError(f"source image exceeds pixel limit: {frame.name}")
                if getattr(image, "n_frames", 1) != 1:
                    raise ConversionError(f"animated images are not supported: {frame.name}")
                image.load()
                return pack_frame(render_frame(image, threshold))
    except (UnidentifiedImageError, Image.DecompressionBombError,
            Image.DecompressionBombWarning, OSError, SyntaxError) as error:
        raise ConversionError(f"cannot decode {frame.name}: {error}") from error


def same_file(left: Path, right: Path) -> bool:
    if left.resolve() == right.resolve():
        return True
    return left.exists() and right.exists() and os.path.samefile(left, right)


def protect_input(output: Path, source: Path, frames: list[Frame]) -> None:
    if same_file(output, source):
        raise ConversionError("output must not overwrite input")
    for frame in frames:
        if isinstance(frame.member, Path) and same_file(output, frame.member):
            raise ConversionError(f"output must not overwrite input frame: {frame.name}")


def atomic_write(output: Path, fps: int, count: int, payloads) -> None:
    """Replace output only after every complete frame has been written successfully."""
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="wb", prefix=f".{output.name}.",
                                         suffix=".tmp", dir=output.parent,
                                         delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(HEADER.pack(MAGIC, WIDTH, HEIGHT, fps, 1, count, FRAME_BYTES, 0))
            written = 0
            for payload in payloads:
                if len(payload) != FRAME_BYTES:
                    raise ConversionError("incorrect encoded frame size")
                stream.write(payload)
                written += 1
            if written != count:
                raise ConversionError("incorrect encoded frame count")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def convert(input_path: Path, output_path: Path, source_fps: int = 30,
            fps: int = 2, threshold: int = 128) -> int:
    validate_rates(source_fps, fps)
    if not 0 <= threshold <= 255:
        raise ConversionError("threshold must be from 0 to 255")
    input_path, output_path = Path(input_path), Path(output_path)
    with open_source(input_path) as (frames, archive):
        indices = select_indices(len(frames), source_fps, fps)
        protect_input(output_path, input_path, frames)
        atomic_write(output_path, fps, len(indices),
                     (decode_frame(frames[index], archive, threshold) for index in indices))
    return len(indices)


def demo_frame(index: int, fps: int, threshold: int) -> bytes:
    image = Image.new("L", (WIDTH, HEIGHT), 255)
    draw = ImageDraw.Draw(image)
    x = (index * 60 // fps) % (WIDTH + 40) - 40
    y = int(HEIGHT / 2 + 35 * math.sin(index / fps * 2))
    draw.ellipse((x, y - 20, x + 40, y + 20), fill=0)
    square_x = WIDTH - 40 - (index * 35 // fps) % (WIDTH - 40)
    draw.rectangle((square_x, 15, square_x + 25, 40), fill=0)
    return pack_frame(render_frame(image, threshold))


def write_demo(output_path: Path, fps: int = 2, seconds: int = 10,
               threshold: int = 128) -> int:
    validate_rates(fps, fps)
    if seconds < 1 or seconds * fps > MAX_ENTRIES:
        raise ConversionError(f"demo duration must be positive and at most {MAX_ENTRIES} frames")
    if not 0 <= threshold <= 255:
        raise ConversionError("threshold must be from 0 to 255")
    count = seconds * fps
    atomic_write(Path(output_path), fps, count,
                 (demo_frame(index, fps, threshold) for index in range(count)))
    return count


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--input", type=Path, help="local numbered PNG/JPEG directory or ZIP")
    source.add_argument("--demo", action="store_true", help="generate procedural smoke-test frames")
    parser.add_argument("--output", required=True, type=Path, help="output C1BA0001 file")
    parser.add_argument("--source-fps", type=int, default=30)
    parser.add_argument("--fps", type=int, default=2, help="output integer frame rate, 1..30")
    parser.add_argument("--threshold", type=int, default=128, help="black below this value, 0..255")
    parser.add_argument("--seconds", type=int, default=10, help="positive integer demo duration")
    args = parser.parse_args(argv)
    try:
        validate_rates(args.source_fps, args.fps)
        if args.demo:
            count = write_demo(args.output, args.fps, args.seconds, args.threshold)
        else:
            count = convert(args.input, args.output, args.source_fps, args.fps, args.threshold)
    except (ConversionError, OSError, zipfile.BadZipFile, zipfile.LargeZipFile,
            RuntimeError, NotImplementedError) as error:
        parser.exit(2, f"error: {error}\n")
    print(f"Wrote {count} frames at {args.fps} fps to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
