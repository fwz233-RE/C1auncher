# SPDX-License-Identifier: GPL-3.0-or-later
"""Run: py -3 -m unittest discover -s App/badapple/tools -v"""

import io
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

from PIL import Image

# Also support discovery from a working directory other than this directory.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import prepare


def png_bytes(color=0, size=(296, 152)):
    stream = io.BytesIO()
    Image.new("L", size, color).save(stream, format="PNG")
    return stream.getvalue()


class PixelTests(unittest.TestCase):
    def test_exact_vertical_page_bit_order(self):
        image = Image.new("L", (296, 152), 255)
        points = [(0, 0), (1, 1), (2, 7), (3, 8), (295, 151)]
        expected = bytearray(5624)
        for x, y in points:
            image.putpixel((x, y), 0)
            expected[(y // 8) * 296 + x] |= 0x80 >> (y % 8)
        self.assertEqual(prepare.pack_frame(image), bytes(expected))
        self.assertEqual(expected[0], 0x80)
        self.assertEqual(expected[1], 0x40)
        self.assertEqual(expected[2], 0x01)
        self.assertEqual(expected[299], 0x80)
        self.assertEqual(expected[-1], 0x01)

    def test_solid_black_and_white(self):
        for color, byte in [(0, 255), (255, 0)]:
            with self.subTest(color=color):
                self.assertEqual(prepare.pack_frame(Image.new("L", (296, 152), color)),
                                 bytes([byte]) * 5624)

    def test_threshold_boundary_without_dithering(self):
        for gray, expected in [(0, 0), (127, 0), (128, 255), (255, 255)]:
            with self.subTest(gray=gray):
                result = prepare.render_frame(Image.new("L", (296, 152), gray))
                self.assertEqual(result.getextrema(), (expected, expected))
        result = prepare.render_frame(Image.new("L", (296, 152), 0), threshold=0)
        self.assertEqual(result.getextrema(), (255, 255))

    def test_square_has_white_side_padding(self):
        result = prepare.render_frame(Image.new("RGB", (100, 100), "black"))
        self.assertEqual(result.size, (296, 152))
        self.assertEqual(Image.eval(result, lambda value: 255 - value).getbbox(),
                         (72, 0, 224, 152))

    def test_wide_image_has_white_top_bottom_padding(self):
        result = prepare.render_frame(Image.new("L", (592, 152), 0))
        self.assertEqual(Image.eval(result, lambda value: 255 - value).getbbox(),
                         (0, 38, 296, 114))

    def test_transparency_is_composited_on_white(self):
        result = prepare.render_frame(Image.new("RGBA", (296, 152), (0, 0, 0, 0)))
        self.assertEqual(prepare.pack_frame(result), bytes(5624))
        image = Image.new("P", (296, 152), 0)
        image.info["transparency"] = 0
        self.assertEqual(prepare.pack_frame(prepare.render_frame(image)), bytes(5624))

    def test_invalid_pixel_inputs(self):
        for image in [Image.new("L", (1, 1)), Image.new("RGB", (296, 152)),
                      Image.new("L", (296, 152), 100)]:
            with self.subTest(image=image):
                with self.assertRaises(prepare.ConversionError):
                    prepare.pack_frame(image)
        with self.assertRaises(prepare.ConversionError):
            prepare.render_frame(Image.new("L", (1, 1)), 256)


class SamplingTests(unittest.TestCase):
    def test_default_rate(self):
        self.assertEqual(prepare.select_indices(60, 30, 2), [0, 15, 30, 45])

    def test_fractional_rate_ratio_and_short_input(self):
        self.assertEqual(prepare.select_indices(10, 7, 3), [0, 2, 4, 7, 9])
        self.assertEqual(prepare.select_indices(31, 30, 2), [0, 15, 30])
        self.assertEqual(prepare.select_indices(1, 30, 2), [0])
        self.assertEqual(prepare.select_indices(5, 3, 3), [0, 1, 2, 3, 4])

    def test_real_time_duration_rounding(self):
        for n in (1, 7, 30, 31, 299):
            for source in (7, 24, 30, 60):
                for fps in (1, 2, 3):
                    with self.subTest(n=n, source=source, fps=fps):
                        indices = prepare.select_indices(n, source, fps)
                        self.assertTrue(all(index < n for index in indices))
                        self.assertGreaterEqual(len(indices) * source, n * fps)
                        self.assertLess(len(indices) * source - n * fps, source)
                        self.assertEqual(indices, [k * source // fps for k in range(len(indices))])

    def test_invalid_rates_and_count(self):
        for args in [(0, 30, 2), (1, 0, 2), (1, 30, 0), (1, 30, 31),
                     (1, 1, 2), (1, 2**32, 2)]:
            with self.subTest(args=args), self.assertRaises(prepare.ConversionError):
                prepare.select_indices(*args)


class FileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.frames = self.root / "frames"
        self.frames.mkdir()
        self.output = self.root / "movie.bin"

    def frame(self, name, color=0, size=(296, 152)):
        path = self.frames / name
        path.parent.mkdir(parents=True, exist_ok=True)
        Image.new("L", size, color).save(path)
        return path

    def archive(self, entries, compression=zipfile.ZIP_STORED):
        path = self.root / "frames.zip"
        with zipfile.ZipFile(path, "w", compression=compression) as archive:
            for name, contents in entries:
                archive.writestr(name, contents)
        return path

    def test_header_payload_numeric_order_and_independent_frames(self):
        self.frame("frame10.PNG", 255)
        self.frame("frame2.jpg", 0)
        count = prepare.convert(self.frames, self.output, source_fps=2, fps=2)
        data = self.output.read_bytes()
        self.assertEqual(count, 2)
        self.assertEqual(prepare.HEADER.size, 32)
        self.assertEqual(struct.unpack("<8sHHIIIII", data[:32]),
                         (b"C1BA0001", 296, 152, 2, 1, 2, 5624, 0))
        self.assertEqual(len(data), 32 + 2 * 5624)
        self.assertEqual(data[32:32 + 5624], b"\xff" * 5624)
        self.assertEqual(data[32 + 5624:], b"\0" * 5624)

    def test_zip_matches_directory_output(self):
        self.frame("folder/frame1.png", 0)
        self.frame("folder/frame2.png", 255)
        archive = self.archive([("folder/", b""), ("folder/frame2.png", png_bytes(255)),
                                ("folder/frame1.png", png_bytes(0)), ("README.txt", b"notes")])
        prepare.convert(self.frames, self.output, 2, 2)
        zip_output = self.root / "zip.bin"
        prepare.convert(archive, zip_output, 2, 2)
        self.assertEqual(self.output.read_bytes(), zip_output.read_bytes())
        self.assertFalse((self.root / "folder").exists())

    def test_conversion_actually_resamples(self):
        for index in range(7):
            self.frame(f"{index}.png", 0 if index in (0, 3, 6) else 255)
        self.assertEqual(prepare.convert(self.frames, self.output, 6, 2), 3)
        self.assertEqual(self.output.read_bytes()[32:], b"\xff" * (3 * 5624))

    def test_duplicate_frame_number_rejected(self):
        self.frame("a001.png")
        self.frame("b1.jpg")
        with self.assertRaisesRegex(prepare.ConversionError, "duplicate frame number"):
            prepare.convert(self.frames, self.output)
        archive = self.archive([("a/1.png", png_bytes()), ("b/001.png", png_bytes())])
        with self.assertRaisesRegex(prepare.ConversionError, "duplicate frame number"):
            prepare.convert(archive, self.output)

    def test_unnumbered_empty_and_nonexistent_inputs(self):
        with self.assertRaises(prepare.ConversionError):
            prepare.convert(self.frames, self.output)
        with self.assertRaises(prepare.ConversionError):
            prepare.convert(self.root / "missing", self.output)
        self.frame("unnumbered.png")
        with self.assertRaisesRegex(prepare.ConversionError, "frame number"):
            prepare.convert(self.frames, self.output)

    def test_failure_preserves_existing_output_and_cleans_temporary(self):
        self.frame("1.png")
        (self.frames / "2.png").write_bytes(b"not an image")
        self.output.write_bytes(b"old output")
        before = set(self.root.iterdir())
        with self.assertRaises(prepare.ConversionError):
            prepare.convert(self.frames, self.output, 2, 2)
        self.assertEqual(self.output.read_bytes(), b"old output")
        self.assertEqual(set(self.root.iterdir()), before)

    def test_failure_leaves_absent_output_absent(self):
        (self.frames / "1.png").write_bytes(b"bad")
        with self.assertRaises(prepare.ConversionError):
            prepare.convert(self.frames, self.output)
        self.assertFalse(self.output.exists())
        self.assertEqual(set(self.root.iterdir()), {self.frames})

    def test_replace_failure_cleans_temporary(self):
        self.frame("1.png")
        self.output.write_bytes(b"old")
        before = set(self.root.iterdir())
        with mock.patch.object(prepare.os, "replace", side_effect=OSError("locked")):
            with self.assertRaises(OSError):
                prepare.convert(self.frames, self.output)
        self.assertEqual(self.output.read_bytes(), b"old")
        self.assertEqual(set(self.root.iterdir()), before)

    def test_input_frame_and_archive_cannot_be_overwritten(self):
        frame = self.frame("1.png")
        original = frame.read_bytes()
        with self.assertRaisesRegex(prepare.ConversionError, "overwrite input"):
            prepare.convert(self.frames, frame)
        self.assertEqual(frame.read_bytes(), original)
        archive = self.archive([("1.png", png_bytes())])
        original_zip = archive.read_bytes()
        with self.assertRaisesRegex(prepare.ConversionError, "overwrite input"):
            prepare.convert(archive, archive)
        self.assertEqual(archive.read_bytes(), original_zip)

    def test_hardlink_input_alias_cannot_be_overwritten(self):
        frame = self.frame("1.png")
        try:
            os.link(frame, self.output)
        except OSError as error:
            self.skipTest(f"hardlinks unavailable: {error}")
        with self.assertRaisesRegex(prepare.ConversionError, "overwrite input"):
            prepare.convert(self.frames, self.output)

    def test_directory_symlink_rejected(self):
        frame = self.frame("1.png")
        link = self.frames / "2.png"
        try:
            link.symlink_to(frame)
        except OSError as error:
            self.skipTest(f"symlinks unavailable: {error}")
        with self.assertRaisesRegex(prepare.ConversionError, "symlink"):
            prepare.convert(self.frames, self.output)

    def test_unsafe_zip_paths_even_for_non_images(self):
        for name in ("../evil.txt", "/evil.txt", "C:/evil.txt", "a\\evil.txt",
                     "a/../evil.txt", "a//evil.txt", "./evil.txt"):
            with self.subTest(name=name):
                entry = zipfile.ZipInfo(name)
                # Windows ZipInfo construction normalizes backslashes; preserve
                # the literal hostile name in the archive fixture instead.
                entry.filename = name
                archive = self.archive([(entry, b"bad"), ("1.png", png_bytes())])
                with self.assertRaisesRegex(prepare.ConversionError, "unsafe ZIP path"):
                    prepare.convert(archive, self.output)
                self.assertFalse(self.output.exists())

    def test_zip_symlink_rejected(self):
        entry = zipfile.ZipInfo("link")
        entry.create_system = 3
        entry.external_attr = (stat.S_IFLNK | 0o777) << 16
        archive = self.archive([(entry, b"target"), ("1.png", png_bytes())])
        with self.assertRaisesRegex(prepare.ConversionError, "symlinks/special"):
            prepare.convert(archive, self.output)

    def test_zip_duplicate_member_rejected(self):
        with self.assertWarns(UserWarning):
            archive = self.archive([("1.png", png_bytes()), ("1.png", png_bytes())])
        with self.assertRaisesRegex(prepare.ConversionError, "duplicate ZIP member"):
            prepare.convert(archive, self.output)

    def test_malformed_zip_rejected(self):
        archive = self.root / "broken.zip"
        archive.write_bytes(b"not a zip")
        with self.assertRaises(zipfile.BadZipFile):
            prepare.convert(archive, self.output)

    def test_compression_bomb_rejected(self):
        archive = self.archive([("bomb.txt", b"0" * 100_000), ("1.png", png_bytes())],
                               zipfile.ZIP_DEFLATED)
        with self.assertRaisesRegex(prepare.ConversionError, "compression ratio"):
            prepare.convert(archive, self.output)

    def test_file_byte_pixel_and_entry_limits(self):
        self.frame("1.png", size=(101, 100))
        with mock.patch.object(prepare, "MAX_INPUT_BYTES", 10):
            with self.assertRaisesRegex(prepare.ConversionError, "frame size"):
                prepare.convert(self.frames, self.output)
        with mock.patch.object(prepare, "MAX_PIXELS", 10_000):
            with self.assertRaisesRegex(prepare.ConversionError, "pixel limit"):
                prepare.convert(self.frames, self.output)
        with mock.patch.object(prepare, "MAX_ENTRIES", 0):
            with self.assertRaisesRegex(prepare.ConversionError, "too many"):
                prepare.convert(self.frames, self.output)
        self.assertFalse(self.output.exists())

    def test_zip_size_limits(self):
        archive = self.archive([("1.png", png_bytes())])
        for setting in ("MAX_INPUT_BYTES", "MAX_TOTAL_BYTES", "MAX_ZIP_BYTES"):
            with self.subTest(setting=setting), mock.patch.object(prepare, setting, 10):
                with self.assertRaises(prepare.ConversionError):
                    prepare.convert(archive, self.output)

    def test_wrong_image_format_and_truncated_png_rejected(self):
        path = self.frames / "1.png"
        Image.new("L", (20, 20)).save(path, format="GIF")
        with self.assertRaisesRegex(prepare.ConversionError, "not PNG/JPEG"):
            prepare.convert(self.frames, self.output)
        path.write_bytes(png_bytes()[:50])
        with self.assertRaises(prepare.ConversionError):
            prepare.convert(self.frames, self.output)

    def test_demo_is_deterministic_and_moves(self):
        self.assertEqual(prepare.write_demo(self.output, fps=2, seconds=2), 4)
        first = self.output.read_bytes()
        prepare.write_demo(self.output, fps=2, seconds=2)
        self.assertEqual(first, self.output.read_bytes())
        self.assertEqual(len(first), 32 + 4 * 5624)
        self.assertNotEqual(first[32:32 + 5624], first[32 + 5624:32 + 2 * 5624])
        with self.assertRaises(prepare.ConversionError):
            prepare.write_demo(self.output, seconds=0)

    def cli(self, *args):
        return subprocess.run([sys.executable, str(Path(prepare.__file__).resolve()), *map(str, args)],
                              capture_output=True, text=True, timeout=20)

    def test_short_cli_smoke(self):
        result = self.cli("--demo", "--seconds", "1", "--fps", "2", "--output", self.output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Wrote 2 frames at 2 fps", result.stdout)
        self.assertEqual(self.output.stat().st_size, 32 + 2 * 5624)
        self.frame("0001.png")
        result = self.cli("--input", self.frames, "--output", self.output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.stat().st_size, 32 + 5624)

    def test_cli_rejects_invalid_options_and_reports_bad_input(self):
        for args in [[], ["--demo", "--fps", "31"], ["--demo", "--fps", "0"],
                     ["--demo", "--source-fps", "1", "--fps", "2"],
                     ["--demo", "--threshold", "256"], ["--demo", "--seconds", "0"],
                     ["--demo", "--input", self.frames], ["--input", "https://example.com/frames.zip"]]:
            with self.subTest(args=args):
                result = self.cli(*args, "--output", self.output)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertNotIn("Traceback", result.stderr)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
