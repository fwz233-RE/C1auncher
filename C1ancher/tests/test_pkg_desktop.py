#!/usr/bin/env python3
"""Build production GUI/IME regression harnesses and export actual rendered pixels.

Run on Linux/WSL: python3 tests/test_pkg_desktop.py [--sanitize]
Storage/repo/exec are mocked; asynchronous refresh uses real fork/pipes and
navigation consumes real input bytes. No external network or device access.
"""
import argparse
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
SETTINGS_HARNESS = r'''
#define main pkg_gui_workflow_main
#include "tests/test_pkg_gui.c"
#undef main
static void write_setting(const char *path, const char *value)
{
    FILE *file = fopen(path, "w"); expect(file != NULL, "settings fixture opens");
    if (file) { fputs(value, file); fclose(file); }
}
int main(int argc, char **argv)
{
    char path[] = "/tmp/c1pkg-settings-XXXXXX", clock[6], battery[8], fitted[256], huge[600];
    enum gui_language language;
    int offset, fd = mkstemp(path);
    if (fd < 0) return 2;
    close(fd); unsetenv("C1_UI_LANGUAGE");
    gui_load_settings("/missing-c1pkg-settings", &language, &offset);
    expect(language == GUI_ZH && offset == 480, "missing settings default Chinese and UTC+8");
    write_setting(path, "# comment\r\n language = en # locale\r\n utc_offset_minutes=-345 ; zone\r\n");
    gui_load_settings(path, &language, &offset);
    expect(language == GUI_EN && offset == -345, "desktop locale/timezone honor whitespace, CRLF and comments");
    setenv("C1_UI_LANGUAGE", "zh", 1); gui_load_settings(path, &language, &offset);
    expect(language == GUI_ZH && offset == -345, "language environment overrides only language, not timezone");
    setenv("C1_UI_LANGUAGE", "invalid", 1); gui_load_settings(path, &language, &offset);
    expect(language == GUI_EN, "invalid environment falls through to shared configuration");
    unsetenv("C1_UI_LANGUAGE");
    write_setting(path, "language=enough\nlanguage=de\nutc_offset_minutes=841\nutc_offset_minutes=17\n");
    gui_load_settings(path, &language, &offset);
    expect(language == GUI_ZH && offset == 480, "unsupported language and out-of-range/non-quarter-hour timezone ignored");
    memset(huge, 'x', 255U); strcpy(huge + 255U, "language=en\n"); write_setting(path, huge);
    gui_load_settings(path, &language, &offset);
    expect(language == GUI_ZH, "oversized line suffix cannot inject a settings key");
    write_setting(path, "language=en\nutc_offset_minutes=840"); gui_load_settings(path, &language, &offset);
    expect(language == GUI_EN && offset == 840, "last settings line needs no newline");
    setenv("TZ", "America/New_York", 1); tzset();
    gui_clock_text(clock, (time_t)0, 480); expect(!strcmp(clock, "08:00"), "UTC+8 ignores process TZ");
    gui_clock_text(clock, (time_t)0, -345); expect(!strcmp(clock, "18:15"), "negative quarter-hour timezone wraps previous day");
    gui_clock_text(clock, (time_t)(23 * 3600), 840); expect(!strcmp(clock, "13:00"), "positive timezone wraps next day");
    write_setting(path, "87\n"); gui_battery_text(battery, path); expect(!strcmp(battery, "87%"), "actual battery percent");
    write_setting(path, "101\n"); gui_battery_text(battery, path); expect(!strcmp(battery, "--%"), "invalid battery is unknown, never fabricated");
    gui_battery_text(battery, "/missing-c1pkg-battery"); expect(!strcmp(battery, "--%"), "missing battery is unknown");
    gui_fit_text(fitted, sizeof(fitted), "中文音乐播放器电子阅读", 80U);
    expect(!strcmp(fitted, "中文音...") && c1pkg_valid_label(fitted), "ellipsis preserves Chinese codepoints");
    gui_fit_text(fitted, sizeof(fitted), "中文ABCD", 48U); expect(!strcmp(fitted, "中..."), "mixed glyph widths are measured");
    gui_fit_text(fitted, sizeof(fitted), "A\xe4\xb8", 80U); expect(!strcmp(fitted, "A"), "malformed tail never splits a glyph");
    gui_fit_text(fitted, sizeof(fitted), "A\x01Z", 80U); expect(!strcmp(fitted, "A"), "control bytes never paint commands");
    unlink(path);
    if (failures) return 1;
    puts("PASS: shared locale/timezone, actual battery, UTF-8 layout");
    return pkg_gui_workflow_main(argc, argv);
}
'''


def png_from_pbm(path, scale):
    words = path.read_text(encoding="ascii").split()
    assert words[:3] == ["P1", "296", "152"]
    assert len(words[3:]) == 296 * 152
    assert set(words[3:]) <= {"0", "1"}
    pixels = [0 if word == "1" else 255 for word in words[3:]]
    raw = bytearray()
    for y in range(152):
        row = b"\0" + b"".join(bytes([pixels[y * 296 + x]]) * scale for x in range(296))
        raw.extend(row * scale)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 296 * scale, 152 * scale, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/pkg-desktop")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="c1pkg-desktop-") as tmp:
        source = Path(tmp) / "test.c"
        source.write_text(SETTINGS_HARNESS, encoding="utf-8")
        command = shlex.split(args.cc) + ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                                        "-O1", "-g", "-I", str(ROOT), "-I", str(ROOT / "src"),
                                        "-I", str(ROOT.parent / "term-ime/integration")]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
        deps = [str(ROOT / f"src/{name}") for name in
                ("pkg/text.c", "pkg/metrics.c", "security/sha256.c", "security/secure_file.c", "pkg/desktop.c", "services/desktop_data.c", "ui/preferences.c", "pkg/tui_model.c", "pkg/util.c", "platform/app_lease.c", "ui/input_method.c")]
        deps += [str(ROOT.parent / "term-ime/integration/c1_ime_client.c")]
        for name, test_source in (("gui", source), ("ime", ROOT / "tests/test_pkg_ime.c")):
            binary = Path(tmp) / name
            subprocess.run(command + [str(test_source)] + deps + ["-o", str(binary)], cwd=ROOT, check=True, timeout=120)
            arguments = [str(output / "pkg-gui-preview.pbm")] if name == "gui" else []
            subprocess.run([str(binary)] + arguments, cwd=ROOT, check=True, timeout=45)
        refresh_binary = Path(tmp) / "refresh"
        refresh_state = Path(tmp) / "repo-state"
        signatures = [str(ROOT / f"third_party/ed25519/{name}.c") for name in
                      ("fe", "ge", "sc", "sha512", "verify", "keypair", "sign")]
        if args.sanitize:
            # The bundled Ed25519 C arithmetic has pre-existing signed-left-shift
            # UBSan diagnostics. Keep ASan on those objects, and full ASan+UBSan
            # on the new worker, repository glue and GUI/IME. Never mute globally.
            print("Note: bundled Ed25519 objects use ASan only (existing signed-shift UBSan findings).", flush=True)
            objects = []
            for signature in signatures:
                obj = Path(tmp) / (Path(signature).stem + ".o")
                subprocess.run(command + ["-fno-sanitize=undefined", "-c", signature, "-o", str(obj)],
                               cwd=ROOT, check=True, timeout=120)
                objects.append(str(obj))
            signatures = objects
        subprocess.run(command + ["-Os", "-I", str(ROOT / "third_party/ed25519"),
                                 f'-DC1PKG_STATE_ROOT="{refresh_state}"', str(ROOT / "tests/test_pkg_refresh.c"),
                                 str(ROOT / "src/pkg/text.c"), str(ROOT / "src/pkg/util.c"),
                                 str(ROOT / "src/pkg/tui_model.c")] + signatures +
                       ["-o", str(refresh_binary)], cwd=ROOT, check=True, timeout=120)
        subprocess.run([str(refresh_binary)], cwd=ROOT, check=True, timeout=45)
    for path in sorted(output.glob("*.pbm")):
        path.with_suffix(".png").write_bytes(png_from_pbm(path, 1))
        path.with_name(path.stem + "-3x.png").write_bytes(png_from_pbm(path, 3))
    print(f"Native and 3x previews: {output}")


if __name__ == "__main__":
    main()
