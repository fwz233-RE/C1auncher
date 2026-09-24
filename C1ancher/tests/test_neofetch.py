#!/usr/bin/env python3
"""Real-PTY regression tests for the bundled C1 Neofetch launcher (Linux/WSL).

Run: python3 tests/test_neofetch.py
Only Python's standard library, Bash and the bundled upstream script are needed.
No device, network, third-party Python package or installed neofetch is used.

The PTY has a kernel winsize and one persistent shell with deliberately stale
exported dimensions. Output is interpreted as terminal operations, not stripped
of ANSI and sliced into lines: the screen model records autowrap, clipped writes
with wrapping disabled, scrolling, cursor state, and the final shell prompt.
Fixtures replace information getters only; launcher, formatter and upstream main
are production code. Separate tests also execute the unmodified device profile.
"""
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import shlex
import struct
import subprocess
import tempfile
import termios
import time
import unicodedata
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "third_party" / "neofetch"
FULL_LABELS = ("Device", "SoC", "CPU", "OS", "Kernel", "Uptime", "Memory",
               "Battery", "Wi-Fi", "IP", "Storage")
COMPACT_LABELS = ("Device", "OS", "Kernel", "Battery", "Wi-Fi", "Storage",
                  "Uptime", "Memory", "IP", "SoC", "CPU")


class Screen:
    """Small strict VT interpreter, including deferred autowrap and wide cells."""

    def __init__(self, columns, rows):
        self.columns, self.rows = columns, rows
        self.cells = [[" "] * columns for _ in range(rows)]
        self.x = self.y = 0
        self.pending_wrap = False
        self.autowrap = self.cursor_visible = True
        self.wraps = self.scrolls = self.clipped = 0

    @staticmethod
    def width(char):
        if unicodedata.category(char) in ("Mn", "Me", "Cf"):
            return 0
        return 2 if unicodedata.east_asian_width(char) in ("W", "F") else 1

    def newline(self):
        self.pending_wrap = False
        self.y += 1
        if self.y == self.rows:
            self.scrolls += 1
            self.cells.pop(0)
            self.cells.append([" "] * self.columns)
            self.y -= 1

    def write(self, char):
        width = self.width(char)
        if width == 0:
            return
        if self.pending_wrap or self.x + width > self.columns:
            if self.autowrap:
                self.wraps += 1
                self.x = 0
                self.newline()
            else:
                self.clipped += 1
                self.pending_wrap = False
                self.x = max(0, self.columns - width)
        if width > self.columns:
            self.clipped += 1
            return
        self.cells[self.y][self.x] = char
        if width == 2:
            self.cells[self.y][self.x + 1] = ""
        self.x += width
        if self.x == self.columns:
            self.x -= 1
            self.pending_wrap = True

    def csi(self, params, command):
        if command == "m":
            return  # SGR changes appearance, not cursor geometry.
        if params.startswith("?") and command in ("h", "l"):
            for mode in params[1:].split(";"):
                if mode == "7":
                    self.autowrap = command == "h"
                elif mode == "25":
                    self.cursor_visible = command == "h"
                else:
                    raise AssertionError(f"Unsupported terminal mode: {mode}")
            return
        values = [int(part or 0) for part in params.split(";")]
        count = values[0] or 1
        self.pending_wrap = False
        if command == "A":
            self.y = max(0, self.y - count)
        elif command == "B":
            self.y = min(self.rows - 1, self.y + count)
        elif command == "C":
            self.x = min(self.columns - 1, self.x + count)
        elif command == "D":
            self.x = max(0, self.x - count)
        elif command in ("H", "f"):
            self.y = min(self.rows - 1, count - 1)
            self.x = min(self.columns - 1, (values[1] if len(values) > 1 else 1) - 1)
        elif command == "J" and values == [2]:
            self.cells = [[" "] * self.columns for _ in range(self.rows)]
        else:
            raise AssertionError(f"Unsupported CSI: {params}{command}")

    def feed(self, data):
        text = data.decode("utf-8") if isinstance(data, bytes) else data
        index = 0
        while index < len(text):
            char = text[index]
            if char == "\x1b":
                match = re.match(r"\x1b\[([0-9;?]*)([A-Za-z])", text[index:])
                if not match:
                    raise AssertionError(f"Unknown/incomplete escape: {text[index:index + 30]!r}")
                self.csi(*match.groups())
                index += len(match.group(0))
                continue
            if char == "\r":
                self.x = 0
                self.pending_wrap = False
            elif char == "\n":
                self.newline()
            elif char == "\b":
                self.x = max(0, self.x - 1)
                self.pending_wrap = False
            elif char == "\t":
                self.x = min(self.columns - 1, (self.x // 8 + 1) * 8)
            elif ord(char) < 32 or 127 <= ord(char) <= 159:
                raise AssertionError(f"Unexpected terminal control: {ord(char):#x}")
            else:
                self.write(char)
            index += 1

    def text(self):
        return "\n".join("".join(row).rstrip() for row in self.cells)


class PtyShell:
    """Keep one real shell/PTY alive while changing its kernel window size."""

    def __init__(self, launcher, columns, rows, env, args=(), stdin_null=False):
        self.master, self.slave = pty.openpty()
        self.resize(columns, rows)
        attrs = termios.tcgetattr(self.slave)
        attrs[3] &= ~termios.ECHO
        attrs[1] |= termios.OPOST | termios.ONLCR
        termios.tcsetattr(self.slave, termios.TCSANOW, attrs)
        script = r'''
shopt -u checkwinsize
while IFS= read -r request; do
    case "$request" in
        run)
            if [[ $C1_TEST_STDIN_NULL == 1 ]]; then
                /bin/bash "$@" </dev/null
            else
                /bin/bash "$@"
            fi
            result=$?
            printf '\036C1_DONE:%d\037' "$result"
            ;;
        quit) exit 0 ;;
        *) exit 90 ;;
    esac
done
'''
        environment = dict(env, C1_TEST_STDIN_NULL="1" if stdin_null else "0")
        self.child = subprocess.Popen(
            ["/bin/bash", "--noprofile", "--norc", "-c", script,
             "c1-neofetch-test", str(launcher), *args],
            stdin=self.slave, stdout=self.slave, stderr=self.slave, env=environment,
            start_new_session=True,
            preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0),
        )

    def resize(self, columns, rows):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))

    def run(self, expected_status=0):
        os.write(self.master, b"run\n")
        data = bytearray()
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.master], [], [], 0.1)
            if not ready:
                if self.child.poll() is not None:
                    raise AssertionError(f"PTY shell exited: {self.child.returncode}, {data!r}")
                continue
            try:
                chunk = os.read(self.master, 65536)
            except OSError as exc:
                if exc.errno != errno.EIO:
                    raise
                chunk = b""
            if not chunk:
                raise AssertionError(f"PTY closed before completion: {data!r}")
            data.extend(chunk)
            match = re.search(rb"\x1eC1_DONE:(\d+)\x1f", data)
            if match:
                if int(match[1]) != expected_status:
                    raise AssertionError(f"Neofetch exit {match[1]!r}: {data!r}")
                if match.end() != len(data):
                    raise AssertionError(f"Unexpected output after completion: {data!r}")
                return bytes(data[:match.start()])
        raise AssertionError(f"Neofetch timed out: {data!r}")

    def close(self):
        try:
            os.write(self.master, b"quit\n")
            self.child.wait(timeout=3)
        except (OSError, subprocess.TimeoutExpired):
            self.child.kill()
            self.child.wait(timeout=3)
        finally:
            os.close(self.master)
            os.close(self.slave)

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()


class NeofetchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="c1-neofetch-pty-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.fixture = self.root / "package"
        self.fixture.mkdir()
        # Byte-for-byte copies avoid repeatedly parsing 10k upstream lines over
        # /mnt's Windows filesystem on WSL; no upstream source is patched.
        for name in ("neofetch", "neofetch.upstream", "c1-logo.txt"):
            (self.fixture / name).write_bytes((PACKAGE / name).read_bytes())
        config = "source " + shlex.quote(str(PACKAGE / "c1-config.conf")) + "\n"
        defaults = {
            "distro": "C1 Linux", "kernel": "4.4.94", "uptime": "1h",
            "memory": "24MiB / 64MiB (37%)", "c1_battery": "82% Discharging 3.94V",
            "c1_wifi": "C1-WiFi", "c1_ip": "192.0.2.10",
            "c1_storage": "1.0G / 8.0G (12%)",
        }
        for field, value in defaults.items():
            config += (f"get_{field}() {{ {field}=${{TEST_{field.upper()}-"
                       + shlex.quote(value) + "}; }\n")
        (self.fixture / "c1-config.conf").write_text(config)
        self.launcher = self.fixture / "neofetch"
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(("TEST_", "C1_NEOFETCH_", "BASH_FUNC_"))
                    and key not in ("BASH_ENV", "ENV", "SHELLOPTS", "BASHOPTS")}
        self.env.update(HOME=str(self.root), XDG_CONFIG_HOME=str(self.root / "config"),
                        XDG_CACHE_HOME=str(self.root / "cache"), TERM="xterm-256color",
                        LC_ALL="C", COLUMNS="80", LINES="24")

    def run_pty(self, columns=37, rows=8, args=(), env=None, launcher=None, stdin_null=False,
                expected_status=0):
        with PtyShell(launcher or self.launcher, columns, rows,
                      dict(self.env, **(env or {})), args, stdin_null) as shell:
            return shell.run(expected_status)

    def assert_fit(self, output, columns, rows, count=None):
        # Compact rendering must never rely on hiding overflow with DEC autowrap
        # off, ANSI stripping, horizontal cursor jumps or terminal erasure.
        self.assertNotIn(b"\x1b", output)
        self.assertNotIn(b"\t", output)
        self.assertNotIn(b"\b", output)
        text = output.decode("utf-8")
        lines = text.splitlines()
        if count is not None:
            self.assertEqual(len(lines), count, repr(output))
        self.assertLessEqual(len(lines), max(0, rows - 2), repr(output))
        for line in lines:
            self.assertTrue(line, "A blank row consumes the prompt budget")
            self.assertLess(sum(Screen.width(char) for char in line), columns, repr(line))
        screen = Screen(columns, rows)
        if rows > 1:
            screen.feed("$\r\n")  # Previous shell command occupies its own row.
        screen.feed(output)
        self.assertEqual(screen.x, 0, "Next shell prompt must begin in column zero")
        self.assertFalse(screen.pending_wrap)
        screen.feed("$")  # Next prompt must fit without scroll or overwriting info.
        self.assertEqual((screen.wraps, screen.scrolls, screen.clipped), (0, 0, 0), screen.text())
        self.assertTrue(screen.autowrap)
        self.assertTrue(screen.cursor_visible)
        self.assertEqual(screen.y, len(lines) + (rows > 1))
        return lines, screen

    @staticmethod
    def rendered(output):
        screen = Screen(240, 80)
        screen.feed(output)
        return screen.text()

    def test_screen_model_detects_wrap_and_disabled_wrap_clipping(self):
        screen = Screen(5, 3)
        screen.feed("12345X")
        self.assertEqual(screen.wraps, 1)
        screen = Screen(5, 3)
        screen.feed("\x1b[?7l12345X")
        self.assertEqual(screen.clipped, 1)
        screen = Screen(5, 1)
        screen.feed("x\r\n")
        self.assertEqual(screen.scrolls, 1)

    def test_normal_and_ime_screens(self):
        for rows, count in ((8, 6), (6, 4)):
            with self.subTest(rows=rows):
                output = self.run_pty(rows=rows)
                lines, _ = self.assert_fit(output, 37, rows, count)
                self.assertEqual([line.split(":")[0] for line in lines], list(COMPACT_LABELS[:count]))
                self.assertIn("Device: C1-Slim MP-D261", lines)
                self.assertIn("Battery: 82% Discharging 3.94V", lines)

    def test_real_profile_without_fixture(self):
        for rows in (8, 6):
            with self.subTest(rows=rows):
                output = self.run_pty(rows=rows, launcher=PACKAGE / "neofetch")
                self.assert_fit(output, 37, rows, rows - 2)
                self.assertIn(b"Device: C1-Slim MP-D261", output)

    def test_resize_same_pty_overrides_unchanged_environment(self):
        for stale in (("80", "24"), ("37", "8")):
            with self.subTest(stale=stale):
                env = dict(self.env, COLUMNS=stale[0], LINES=stale[1])
                with PtyShell(self.launcher, 37, 8, env) as shell:
                    for rows in (8, 6, 8, 6):
                        shell.resize(37, rows)
                        self.assert_fit(shell.run(), 37, rows, rows - 2)
                    shell.resize(49, 19)
                    text = self.rendered(shell.run())
                    self.assertIn("XBurst MIPS32r2 (1)", text)
                    self.assertIn("_____", text)
                    shell.resize(37, 6)
                    self.assert_fit(shell.run(), 37, 6, 4)

    def test_busybox_stty_does_not_override_live_size_with_environment(self):
        tools = self.root / "bin"
        tools.mkdir()
        stty = tools / "stty"
        stty.write_text("#!/bin/sh\n"
                        'if [ "$1" = size ] && [ -n "${LINES:-}" ] && [ -n "${COLUMNS:-}" ]; then\n'
                        '    printf "%s %s\\n" "$LINES" "$COLUMNS"\n'
                        'else exec /bin/stty "$@"; fi\n')
        stty.chmod(0o755)
        env = dict(self.env, PATH=str(tools) + ":" + self.env["PATH"], LINES="19", COLUMNS="49")
        for stdin_null in (False, True):
            with PtyShell(self.launcher, 37, 8, env, stdin_null=stdin_null) as shell:
                for rows in (8, 6, 8):
                    shell.resize(37, rows)
                    self.assert_fit(shell.run(), 37, rows, rows - 2)

    def test_redirected_stdin_still_reads_output_tty(self):
        self.assert_fit(self.run_pty(rows=6, stdin_null=True), 37, 6, 4)

    def test_narrow_widths_and_short_heights(self):
        for columns in (1, 2, 3, 4, 8, 12, 20, 36, 37, 48):
            with self.subTest(columns=columns):
                output = self.run_pty(columns=columns)
                self.assert_fit(output, columns, 8, 0 if columns == 1 else 6)
        for rows in (1, 2, 3, 4, 6, 8, 12, 18):
            with self.subTest(rows=rows):
                self.assert_fit(self.run_pty(rows=rows), 37, rows, min(11, max(0, rows - 2)))

    def test_long_values_are_fitted_before_rendering(self):
        fields = ("DISTRO", "KERNEL", "C1_BATTERY", "C1_WIFI", "C1_STORAGE")
        env = {"TEST_" + field: field + "-" + "long_value_" * 400 for field in fields}
        output = self.run_pty(env=env)
        lines, _ = self.assert_fit(output, 37, 8, 6)
        self.assertEqual(lines[0], "Device: C1-Slim MP-D261")
        for label, line in zip(COMPACT_LABELS[1:], lines[1:]):
            self.assertTrue(line.startswith(label + ": "), line)
            self.assertTrue(line.endswith("..."), line)
            self.assertEqual(len(line), 36)

    def test_unicode_is_valid_width_safe_and_locale_independent(self):
        values = {"TEST_DISTRO": "中文系统" * 30, "TEST_KERNEL": "e\u0301" * 40,
                  "TEST_C1_WIFI": "书房网络🙂" * 30}
        results = []
        for locale in ("C", "C.UTF-8"):
            with self.subTest(locale=locale):
                output = self.run_pty(env=dict(values, LC_ALL=locale))
                self.assert_fit(output, 37, 8, 6)
                self.assertIn("OS: 中文系统", output.decode())
                self.assertIn("Wi-Fi: 书房网络🙂", output.decode())
                self.assertNotIn("\ufffd", output.decode())
                results.append(output)
        self.assertEqual(results[0], results[1])
        for columns in (8, 9, 10, 11, 12, 13, 14):
            with self.subTest(columns=columns):
                self.assert_fit(self.run_pty(columns=columns, env=values), columns, 8, 6)

    def test_control_sequences_and_invalid_utf8_are_data_not_instructions(self):
        values = {
            "TEST_DISTRO": "safe\x1b[2J\tX\rY\nZ\b",
            "TEST_KERNEL": "v\\e[2J\\n",  # printf must not interpret backslash escapes.
            "TEST_C1_BATTERY": "82%\u009b31m",
            "TEST_C1_WIFI": b"bad\xf0(\x8c(\xed\xa0\x80\xc0\xaf".decode("utf-8", "surrogateescape"),
        }
        output = self.run_pty(env=values)
        self.assert_fit(output, 37, 8, 6)
        self.assertIn(b"OS: safe?[2J X Y Z?", output)
        self.assertIn(b"Kernel: v\\e[2J\\n", output)
        self.assertIn(b"Battery: 82%?31m", output)
        self.assertIn(b"Wi-Fi: bad?(?(?????", output)

    def test_regular_terminal_retains_full_profile_and_original_logo(self):
        for columns, rows in ((49, 19), (80, 24)):
            with self.subTest(columns=columns, rows=rows):
                output = self.run_pty(columns, rows)
                screen = Screen(columns, rows)
                screen.feed("$\r\n")
                screen.feed(output)
                screen.feed("$")
                self.assertEqual((screen.wraps, screen.scrolls, screen.clipped), (0, 0, 0), screen.text())
                self.assertTrue(screen.autowrap)
                self.assertTrue(screen.cursor_visible)
                for label in FULL_LABELS:
                    self.assertIn(label + ":", screen.text())
                for logo_line in ("_____  __", "/ ___/ / /", "C1-SLIM", "MP-D261"):
                    self.assertIn(logo_line, screen.text())

    def test_stdout_always_has_all_information_without_logo_or_truncation(self):
        value = "long-wifi-" * 30
        output = self.run_pty(rows=6, args=("--stdout",), env={"TEST_C1_WIFI": value})
        self.assertNotIn(b"\x1b", output)
        text = output.decode()
        self.assertEqual([line.split(":")[0] for line in text.splitlines() if line], list(FULL_LABELS))
        self.assertIn("Wi-Fi: " + value, text)
        self.assertNotIn("_____", text)
        self.assertNotIn("...", text)

    def test_explicit_options_keep_upstream_semantics(self):
        output = self.run_pty(rows=6, args=("--off",))
        text = self.rendered(output)
        self.assertIn("SoC: Ingenic X1600", text)
        self.assertIn("CPU: XBurst MIPS32r2 (1)", text)
        self.assertNotIn("_____", text)
        output = self.run_pty(rows=6, args=("--stdout", "--disable", "c1_wifi"))
        self.assertNotIn(b"Wi-Fi:", output)
        self.assertIn(b"IP: 192.0.2.10", output)
        output = self.run_pty(rows=6, args=("kernel",))
        self.assertEqual(output.strip(), b"kernel: 4.4.94")
        output = self.run_pty(rows=6, args=("--stdout", "--separator", "="))
        self.assertIn(b"Device= C1-Slim MP-D261", output)
        # Upstream 7.1.0 deliberately exits 1 for both --version and --help.
        output = self.run_pty(rows=6, args=("--version",), expected_status=1)
        self.assertIn(b"Neofetch 7.1.0", output)
        output = self.run_pty(rows=6, args=("--help",), expected_status=1)
        self.assertIn(b"--stdout", output)
        self.assertIn(b"--config", output)
        logo = self.root / "custom logo.txt"
        # Upstream rejects one-line artwork and falls back to a distro logo.
        logo.write_text("CUSTOM-LOGO\nSECOND-LINE\n")
        output = self.run_pty(rows=6, args=("--source", str(logo)))
        self.assertIn("CUSTOM-LOGO", self.rendered(output))
        self.assertIn("CPU: XBurst", self.rendered(output))

    def test_custom_config_and_config_none_are_not_overridden(self):
        config = self.root / "custom config.conf"
        config.write_text('print_info() { prin "Custom" "$COLUMNS x $LINES"; }\n')
        output = self.run_pty(rows=6, args=("--config", str(config), "--stdout"),
                              env={"C1_NEOFETCH_COMPACT": "1"})
        self.assertEqual(output.strip(), b"Custom: 37 x 6")
        output = self.run_pty(rows=6, args=("--config", "none", "--stdout"))
        self.assertNotIn(b"Device:", output)
        self.assertNotIn(b"C1-Slim MP-D261", output)
        self.assertIn(b"OS:", output)

    def test_invalid_and_zero_winsize_uses_validated_environment_fallback(self):
        config = self.root / "geometry.conf"
        config.write_text('print_info() { prin "Size" "$COLUMNS x $LINES"; }\n')
        args = ("--config", str(config), "--stdout")
        for columns, rows, expected in (("037", "008", "37 x 8"),
                                        ("0", "-1", "80 x 24"),
                                        ("1+2", "oops", "80 x 24"),
                                        ("999999999999999999", "65536", "80 x 24")):
            with self.subTest(columns=columns, rows=rows):
                output = self.run_pty(0, 0, args, {"COLUMNS": columns, "LINES": rows})
                self.assertEqual(output.strip(), ("Size: " + expected).encode())
        self.assert_fit(self.run_pty(env={"COLUMNS": "bad", "LINES": "bad"}), 37, 8, 6)
        self.assert_fit(self.run_pty(env={"COLUMNS": "", "LINES": ""}), 37, 8, 6)

    def test_redirected_output_retains_full_profile(self):
        env = dict(self.env, COLUMNS="37", LINES="6")
        for args in ((), ("--stdout",)):
            with self.subTest(args=args):
                result = subprocess.run(["/bin/bash", str(self.launcher), *args],
                                        env=env, stdin=subprocess.DEVNULL,
                                        capture_output=True, timeout=20, check=True)
                self.assertEqual(result.stderr, b"")
                # A pipe has no PTY OPOST/ONLCR; model those output semantics
                # explicitly before displaying the complete, untrimmed capture.
                text = self.rendered(result.stdout.replace(b"\n", b"\r\n"))
                for label in FULL_LABELS:
                    self.assertIn(label + ":", text)
                if not args:
                    self.assertIn("_____", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
