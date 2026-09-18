#!/usr/bin/env python3
"""模糊音开关的契约测试。

设置面板的「模糊音」开关在 luna_pinyin_simp（精确）与 luna_pinyin_simp_fuzzy
（模糊）两份 schema 之间切换——不是改配置、不是重部署，所以切换是即时的。

模糊音各组必须真的跨组出候选（南方口音的常见混淆）：n/l、zh/z、en/eng、an/ang、
ian/iang、uan/uang。关掉后必须回到精确拼音。

跑真实二进制（PTY + 一次性 HOME），检验部署后的 schema 与编译好的 prism。
"""
import fcntl
import os
import pty
import re
import select
import shutil
import struct
import sys
import tempfile
import termios
import time

BIN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "term-ime")

# (音节, 该音节只在*模糊音*下才会出现的字, 说明)
FUZZY_ONLY = [
    ("la", "那", "n/l"),
    ("fen", "风", "en/eng"),
    ("fan", "方", "an/ang"),
    ("lan", "狼", "an/ang"),
    ("lang", "蓝", "ang/an"),
    ("qian", "枪", "ian/iang"),
    ("wan", "网", "uan/uang"),
]

# (音节, 精确拼音下必须有的字) —— 确保关掉模糊音后回到正常
PRECISE = [
    ("la", "啦"),
    ("fan", "饭"),
    ("lan", "蓝"),
]

ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
OSC = re.compile(r"\x1b\][^\x07]*\x07")


def strip_ansi(data: bytes) -> str:
    return OSC.sub("", ANSI.sub("", data.decode("utf-8", "replace")))


class Session:
    def __init__(self) -> None:
        self.home = tempfile.mkdtemp(prefix="fuzzy-")
        env = dict(os.environ)
        env.update(
            HOME=self.home,
            XDG_CONFIG_HOME=os.path.join(self.home, ".config"),
            XDG_DATA_HOME=os.path.join(self.home, ".local", "share"),
            SHELL="/bin/sh",
            TERM="xterm-256color",
        )
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            try:
                os.execvpe(BIN, [BIN], env)
            except Exception:
                os._exit(127)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 140, 0, 0))
        flags = fcntl.fcntl(self.fd, fcntl.F_GETFL)
        fcntl.fcntl(self.fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    def read(self, seconds=1.0, quiet=0.10) -> bytes:
        out = b""
        end = time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], quiet)
            if not ready:
                break
            try:
                chunk = os.read(self.fd, 65536)
            except BlockingIOError:
                continue  # non-blocking fd reported readable but had nothing yet
            except OSError:
                break
            if not chunk:
                break
            out += chunk
        return out

    def wait_for(self, pattern: str, seconds=60.0) -> bool:
        rx = re.compile(pattern)
        buf = b""
        end = time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except BlockingIOError:
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            if rx.search(strip_ansi(buf)):
                return True
        return False

    def send(self, data: bytes) -> None:
        try:
            os.write(self.fd, data)
        except OSError:
            pass

    def bar(self):
        frame = strip_ansi(self.read(0.9))
        lines = [l for l in frame.splitlines() if re.search(r"\[拼\]", l)]
        if not lines:
            return []
        return [m[1] for m in re.findall(r"(\d)\.(\S+)", lines[-1])]

    def candidates(self, pinyin, pages=3) -> str:
        """Type `pinyin`, page a few times, return all candidates as one string."""
        self.read(0.3)
        self.send(pinyin.encode())
        time.sleep(0.7)
        collected = []
        for _ in range(pages):
            collected += self.bar()
            self.send(b".")
            time.sleep(0.4)
        self.send(b"\x1b")
        time.sleep(0.35)
        return "".join(collected)

    def toggle_fuzzy(self, key: bytes, want_label: str) -> bool:
        """Open settings, focus the 模糊音 row, press `key` (h=关, l=开), close."""
        self.read(0.4)
        self.send(b"\x01s")
        if not self.wait_for(r"模糊音", seconds=10.0):
            return False
        self.read(0.3)
        self.send(b"jj")  # ui_language -> candidates -> fuzzy
        self.wait_for(r"模糊音: \[[关开]\]", seconds=6.0)
        self.read(0.3)
        self.send(key)
        ok = self.wait_for(r"模糊音: \[" + want_label + r"\]", seconds=20.0)
        self.read(0.4)
        self.send(b"\x1b")
        time.sleep(0.9)
        self.read(0.5)
        return ok

    def close(self) -> None:
        try:
            os.kill(self.pid, 9)
            os.waitpid(self.pid, 0)
        except Exception:
            pass
        shutil.rmtree(self.home, ignore_errors=True)


def main() -> int:
    session = Session()
    results = []
    try:
        if not session.wait_for(r"\[EN\]|\[拼\]"):
            print("  [FAIL] app did not become ready")
            return 1
        session.send(b"\x01 ")  # Chinese mode
        session.wait_for(r"\[拼\]", seconds=8.0)

        print("模糊音默认开（config fuzzy_pinyin 默认 true）:")
        for pinyin, char, group in FUZZY_ONLY:
            got = session.candidates(pinyin)
            ok = char in got
            results.append((f"on: {pinyin}->{char} ({group})", ok))
            print("  [%s] %-6s offers %s  (%s)" % ("PASS" if ok else "FAIL", pinyin, char, group))

        print("关掉模糊音:")
        toggled = session.toggle_fuzzy(b"h", "关")
        results.append(("toggle off works", toggled))
        print("  [%s] 面板开关切到 关" % ("PASS" if toggled else "FAIL"))
        for pinyin, char, group in FUZZY_ONLY:
            got = session.candidates(pinyin)
            ok = char not in got
            results.append((f"off: {pinyin} no {char}", ok))
            print("  [%s] %-6s no %s  (%s)" % ("PASS" if ok else "FAIL", pinyin, char, group))
        for pinyin, char in PRECISE:
            got = session.candidates(pinyin)
            ok = char in got
            results.append((f"off: {pinyin} keeps {char}", ok))
            print("  [%s] %-6s keeps %s" % ("PASS" if ok else "FAIL", pinyin, char))

        print("再打开:")
        toggled = session.toggle_fuzzy(b"l", "开")
        results.append(("toggle on works", toggled))
        got = session.candidates("fan")
        ok = "方" in got
        results.append(("on again: fan->方", ok))
        print("  [%s] toggle back + fan offers 方" % ("PASS" if toggled and ok else "FAIL"))
    finally:
        session.close()

    passed = sum(1 for _, ok in results if ok)
    print("\n%d/%d checks passed" % (passed, len(results)))
    for name, ok in results:
        if not ok:
            print("FAILED:", name)
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
