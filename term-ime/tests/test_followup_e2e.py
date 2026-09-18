#!/usr/bin/env python3
"""Real App/Rime/PTY regression checks; all fixtures live in a temporary HOME."""
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import tempfile
import termios
import time
import unittest

BINARY = str(Path(os.environ.get("TERM_IME_BINARY", "./build/term-ime")).resolve())
ANSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")
RECEIVER = """#!/usr/bin/python3
import os, tty
tty.setraw(0)
os.write(1, b'\\x1b[?2004hRECEIVER_READY\\r\\n')
received = bytearray()
while True:
    data = os.read(0, 4096)
    if not data:
        break
    if b'\\x04' in data:
        received.extend(data.split(b'\\x04', 1)[0])
        break
    received.extend(data)
with open(os.environ['TEST_CAPTURE'], 'wb') as f:
    f.write(received)
os.write(1, b'\\x1b[?2004lRECEIVER_DONE\\r\\n')
"""


class FollowupE2E(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="term-ime-followup-e2e-")
        cls.root = Path(cls.temp.name)
        cls.receiver = cls.root / "receiver"
        cls.receiver.write_text(RECEIVER, encoding="utf-8")
        cls.receiver.chmod(0o700)
        cls.rime = cls.root / "rime"
        cls.rime.mkdir()
        (cls.rime / "luna_pinyin_simp.custom.yaml").write_text(
            "patch:\n  menu/page_size: 12\n  translator/enable_user_dict: false\n")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def session(self, payload=None, chinese=False, shell=None, shell_env=None, bash=False):
        case = self.root / self._testMethodName
        case.mkdir(exist_ok=True)
        capture = case / "capture"
        config = {"shell": str(self.receiver) if shell is None else shell,
                  "rime_user_data_dir": str(self.rime), "fuzzy_pinyin": False,
                  "max_candidates": 1, "log_file": str(case / "app.log")}
        path = case / "config.json"
        path.write_text(json.dumps(config))
        env = dict(os.environ, HOME=str(case), XDG_CONFIG_HOME=str(case / "config"),
                   XDG_DATA_HOME=str(case / "data"), TERM="xterm-256color",
                   SHELL=str(self.receiver) if shell_env is None else shell_env,
                   TEST_CAPTURE=str(capture))
        pid, master = pty.fork()
        if pid == 0:
            os.execve(BINARY, [BINARY, str(path)], env)
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        output = bytearray()
        status = None

        def receive(seconds, needle=None):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if needle is not None and needle in ANSI.sub(b"", output):
                    return True
                ready, _, _ = select.select([master], [], [], 0.05)
                if ready:
                    try:
                        data = os.read(master, 65536)
                    except OSError:
                        break
                    if not data:
                        break
                    output.extend(data)
            return needle is None or needle in ANSI.sub(b"", output)

        try:
            if payload is not None:
                self.assertTrue(receive(40, b"[EN]"), output[-1000:])
                if not bash:
                    self.assertTrue(receive(5, b"RECEIVER_READY"), output[-1000:])
                if chinese:
                    os.write(master, b"\x01 ")
                    self.assertTrue(receive(3, "[拼]".encode()), output[-1000:])
                for chunk in payload:
                    os.write(master, chunk)
                    receive(0.1)
                if not bash:
                    os.write(master, b"\x04")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                receive(0.1)
                ended, code = os.waitpid(pid, os.WNOHANG)
                if ended:
                    status = os.waitstatus_to_exitcode(code)
                    break
            self.assertIsNotNone(status, "application did not exit")
            return status, bytes(output), capture.read_bytes() if capture.exists() else None
        finally:
            if status is None:
                try:
                    os.kill(pid, signal.SIGTERM)
                    os.waitpid(pid, 0)
                except ProcessLookupError:
                    pass
            os.close(master)

    def test_chinese_paste_preserves_mixed_text_and_shortcuts(self):
        body = "hello 123 中文".encode() + b"\x01s\x01 \x01\x03\x1b[20x"
        paste = b"\x1b[200~" + body + b"\x1b[201~"
        # Split inside the closing marker; waiting here exceeds the normal
        # Escape timeout but must not terminate or reinterpret paste content.
        code, _, received = self.session([paste[:-3], paste[-3:]], chinese=True)
        self.assertEqual(code, 0)
        self.assertEqual(received, paste)

    def test_english_paste_remains_literal(self):
        paste = b"\x1b[200~hello 123\x01s\x1b[201~"
        code, _, received = self.session([paste])
        self.assertEqual(code, 0)
        self.assertEqual(received, paste)

    def test_tenth_candidate_is_committed(self):
        code, _, received = self.session([b"ni.........", b"1"], chinese=True)
        self.assertEqual(code, 0)
        self.assertIsNotNone(received)
        self.assertTrue(received, "visible tenth candidate produced no commit")
        self.assertTrue(any(ord(ch) > 127 for ch in received.decode("utf-8")))

    def test_explicit_bash_overrides_environment(self):
        code, output, received = self.session(
            [b"printf '\\102\\101\\123\\110\\137\\117\\113\\n'; exit\r"],
            shell="/bin/bash", bash=True)
        self.assertEqual(code, 0)
        self.assertIsNone(received, "SHELL environment incorrectly replaced explicit bash")
        self.assertIn(b"BASH_OK", output)

    def test_empty_shell_uses_environment(self):
        code, _, received = self.session([b"environment-shell"], shell="")
        self.assertEqual(code, 0)
        self.assertEqual(received, b"environment-shell")

    def test_invalid_shell_reports_nonzero_startup_failure(self):
        invalid = "/nonexistent/term-ime-followup-shell"
        code, output, received = self.session(shell=invalid)
        self.assertNotEqual(code, 0)
        self.assertIsNone(received)
        self.assertIn(invalid.encode(), output)
        self.assertIn("启动失败".encode(), output)


if __name__ == "__main__":
    unittest.main()
