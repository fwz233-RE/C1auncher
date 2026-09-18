#!/usr/bin/env python3
"""Candidate bar must offer simplified Chinese forms only.

Regression for the leak where the simplifier (OpenCC t2s) did not cover Japanese
shinjitai (楽/薬/気…) nor variants such as 妳/麹/麺/麽, so raw dictionary entries
surfaced as candidates. The fix chains conversion data into zh_hans
(data/rime-data/opencc/t2s_full.json: JP shinjitai → TS → explicit variants).

Two things are checked, because the naive "no traditional char" check has both
false positives and false negatives:

* leak set  — forms that must never appear (日文新字体 / 异体字 / 妳).
* keep set  — simplified characters that look like the above but are correct
  Chinese (芸 yún, 沪 hù, 薹 tái, 乾 qián in 乾隆, 曲, 面, 你, 于) and must stay,
  plus guards against over-conversion (芸 must not become 艺, 沪 not 滤).

Runs the real binary in a PTY with a throwaway HOME, so it exercises the deployed
schema + compiled prism exactly as a user would.
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

# Variant / traditional forms that must never reach the candidate bar.
# Populated two ways below: a hand-curated block (kokuji + zhuyin + a few common
# traditional forms as a guard — their presence means the opencc chain failed to
# load), then the union of every opencc variant-table key (data-driven).
NON_SIMPLIFIED = set(
    # Common traditional forms: present only if the opencc chain failed to load.
    "過 國 長 學 關 門 時 為 對 開 關 業 話 說 見 來"
    "楽薬気実沢圧辺団従収効営豊発関対観転変続経顔頭帰広県領両勧権検桜済斎斉"
    "読認費単図雑仮伝仏応択線録錯帰撃撃蔵麹麺麽苧剋夥妳"
    # Japanese kokuji pruned from luna_pinyin.dict.yaml (2026-09-15): coined in
    # Japan, absent from 通用规范汉字表. 腺 is the one exception — it is a
    # Japanese-coined char but fully naturalized in Chinese (甲状腺), so it is
    # kept and asserted in KEEP below.
    "俤俧働凧凩凪匁匂哘噺囎圦圷圸垰垳塀墹嬶屶岾峅峠怺杢杤枠枡栃桝梺椙椛椣椥榊樫"
    "燵狆瓩畑畠硲硴笹簓籾粁粂粍糀糎纃聢膤膵艝萙蘒蟎裃躮躾軈轌辷辻込迚遖釺銯錺錻"
    "鎹雫鞆颪鮗鯥鯰鯱鯳鰯鱈鳰鴫鶎鶫麿"
    # Bopomofo / zhuyin symbols (dead entries: the schemas' alphabet is pinyin).
    "ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦㄧㄨㄩ"
)


def _table_keys(fname: str) -> set:
    """Every variant key shipped in an opencc table must never survive in a
    candidate: the chain converts them to their standard form. Data-driven so a
    re-generated table (e.g. after re-vendoring the dictionary) is covered
    automatically."""
    path = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "data", "rime-data", "opencc", fname,
    )
    keys = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            if "\t" in line:
                keys.add(line.split("\t")[0])
    return keys


NON_SIMPLIFIED |= (
    _table_keys("variants.txt")
    | _table_keys("variants_jp.txt")
    | _table_keys("variants_ext.txt")
)

# Every syllable that previously exposed one of the above.
SYLLABLES = [
    "ni", "nihao", "yue", "le", "yao", "qi", "dui", "guan", "bian", "tu",
    "qu", "mian", "me", "mo", "neng", "nen", "ke", "huo", "yun", "hu", "fu",
    "chang", "zhang", "xue", "xian", "guo", "ren",
]

# Simplified characters that must remain selectable (and must not be converted
# into an unrelated char by the added mappings).
KEEP = {
    "ni": "你",
    "nihao": "你好",
    "yue": "月",
    "le": "乐",
    "yao": "药",
    "yun": "芸",   # yún, not 艺 (JP 芸 == 藝)
    "hu": "沪",    # hù, not 滤 (JP 沪 == 濾 in some tables)
    "tai": "薹",   # tái, correct in 蒜薹
    "qian": "乾",  # qián, correct in 乾隆 (must NOT become 干)
    "qu": "曲",
    "mian": "面",
    "yu": "于",
    "xian": "腺",  # Japanese-coined but standard Chinese (甲状腺) — must survive the kokuji prune
}

ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
OSC = re.compile(r"\x1b\][^\x07]*\x07")


def strip_ansi(data: bytes) -> str:
    return OSC.sub("", ANSI.sub("", data.decode("utf-8", "replace")))


class Session:
    def __init__(self) -> None:
        self.home = tempfile.mkdtemp(prefix="simp-")
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
                continue  # non-blocking fd reported readable but had nothing yet
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
        frame = strip_ansi(self.read(0.8))
        lines = [l for l in frame.splitlines() if re.search(r"\[拼\]", l)]
        if not lines:
            return []
        return [m[1] for m in re.findall(r"(\d)\.(\S+)", lines[-1])]

    def candidates(self, pinyin):
        """Type `pinyin` and page through every page, returning all candidates."""
        self.read(0.3)
        self.send(pinyin.encode())
        time.sleep(0.7)
        collected, page = [], 0
        while page < 6:
            now = self.bar()
            fresh = [c for c in now if c not in collected]
            if not now or (page > 0 and not fresh):
                break
            collected += fresh
            self.send(b".")
            time.sleep(0.45)
            page += 1
        self.send(b"\x1b")
        time.sleep(0.4)
        return collected

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

        cache = {}
        for syllable in SYLLABLES:
            candidates = cache[syllable] = session.candidates(syllable)
            leaked = sorted({ch for c in candidates for ch in c if ch in NON_SIMPLIFIED})
            ok = bool(candidates) and not leaked
            results.append(("no leak: " + syllable, ok))
            print(
                "  [%s] %-7s n=%-3d %s%s"
                % (
                    "PASS" if ok else "FAIL",
                    syllable,
                    len(candidates),
                    "".join(candidates)[:42],
                    ("  <-- leaked: " + " ".join(leaked)) if leaked else "",
                )
            )

        for syllable, expected in KEEP.items():
            candidates = cache.get(syllable) or session.candidates(syllable)
            ok = any(expected in c for c in candidates)
            results.append(("keep: " + expected, ok))
            print("  [%s] %-7s offers %s" % ("PASS" if ok else "FAIL", syllable, expected))
    finally:
        session.close()

    passed = sum(1 for _, ok in results if ok)
    print("\n%d/%d checks passed" % (passed, len(results)))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
