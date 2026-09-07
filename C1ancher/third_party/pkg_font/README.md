# C1 Package Bitmap font

This bundled font is a 16-pixel-high bitmap subset of GNU Unifont 16.0.04,
renamed **C1 Package Bitmap**. Glyph outlines are unchanged. The subset contains
30,064 glyphs, including all 20,992 CJK Unified Ideographs (U+4E00–U+9FFF), all
6,592 Extension A ideographs (U+3400–U+4DBF), compatibility ideographs, Chinese
punctuation/radicals, kana, bopomofo, ASCII, Latin, arrows and box symbols.
Simplified and traditional Chinese names are supported without maintaining a
fixed list of GUI characters. Supplementary-plane characters, including rare
Chinese Extension B names, are valid metadata but render as a replacement glyph.
This is a bitmap renderer, not a complex-script shaping engine.

## Source and license

Upstream: https://unifoundry.com/unifont/

Pinned source archive:
https://unifoundry.com/pub/unifont/unifont-16.0.04/unifont-16.0.04.tar.gz

SHA-256: `2bd4e4679757126f48e1bf2c1be40b09aa92162bfedda4683ce5fbc70a2a5972`

The font data is distributed under the **SIL Open Font License 1.1** option of
Unifont's dual license. `LICENSE.txt` preserves upstream license/author notices.
The generated font has a different name, and no proprietary/system font is used.
Keep this notice and `LICENSE.txt` with redistributions, including binary release
packages containing the font. The complete notices are also embedded in `c1pkg`
and available with `c1pkg font-license`, including four-component core updates.
The font license does not relicense independent
application or renderer code. The original Unifont utilities are not compiled or
included as code dependencies.

## Reproduction

Run `python3 tools/generate_pkg_font.py` from any directory. Python's standard
library is sufficient. The generator downloads a checksummed release archive,
caches it under `~/.cache/c1pkg`, and emits `src/pkg/font_generated.h`
and `third_party/pkg_font/LICENSE.txt`. For offline generation, pass
`--source /path/to/unifont-16.0.04.tar.gz`. Add `--check` to verify generated outputs
byte-for-byte without modifying the checked-in files. Device and host builds use
the checked-in header directly and never download fonts.

The generated arrays contain 992,112 bytes of glyph rows and widths plus a small
range map. Runtime uses no allocation, font filesystem access, decompression,
locale, FreeType, iconv or other external library. Compile `src/pkg/text.c` once;
the generated header is private to that translation unit.

## Metadata contract

Name and author fields retain the existing **40 UTF-8 byte** limit (plus NUL in
the C buffers), not 40 Unicode characters. C `c1pkg_valid_label()` and server
`label()` share an explicit version-stable rejection policy for malformed UTF-8,
control/bidirectional/format/invisible modifiers, line separators, noncharacters
and the replacement character. Leading/trailing Unicode whitespace is rejected.
Internal ordinary Unicode whitespace is allowed. Metadata is never truncated or
normalized, so signed UTF-8 bytes remain unchanged.

## Standalone host tests

From the project root in Linux/WSL:

- Compile `tests/test_pkg_text.c src/pkg/text.c` with `-Isrc -D_DEFAULT_SOURCE`.
  AddressSanitizer and UndefinedBehaviorSanitizer are supported. The test checks
  malformed/truncated UTF-8, guard pages, pixel layout, clipping, transparent ink,
  Chinese glyph coverage and the exact 40-byte boundary.
- Compile `tests/test_pkg_metadata_utf8.c src/pkg/repo.c src/pkg/util.c
  src/pkg/text.c` with `-D_POSIX_C_SOURCE=200809L -Isrc -Isrc/pkg
  -Ithird_party/ed25519 -ffunction-sections -fdata-sections -Wl,--gc-sections`.
  Linker section removal isolates parser tests from unused transport/crypto I/O.
