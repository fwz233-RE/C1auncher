#!/usr/bin/env python3
"""Monkey test action model, weighted generator, targeted probes, and logging.

This module is *imported by Claude* when driving term-ime via the term-debug-mcp
(``mcp__tui-debug__*``) tools, and is also usable standalone to replay a recorded
sequence into a PTY. It deliberately has no dependency on the MCP tools themselves
so it can be exercised from plain Python.

Conventions followed (see tests/test_e2e.py siblings):
  - standalone module, stdlib only, no pytest
  - paths derived from __file__, not hardcoded
  - exit-code semantics handled by the driver, not here
"""

from __future__ import annotations

import json
import random
import string
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator, List, Optional, Union

# ---------------------------------------------------------------------------
# Byte constants
# ---------------------------------------------------------------------------
CTRL_A = 0x01
ESC = 0x1B
BACKSPACE = 0x08
DEL = 0x7F
TAB = 0x09
ENTER = 0x0D
SPACE = 0x20

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BIN = REPO_ROOT / "build" / "term-ime"


# ---------------------------------------------------------------------------
# Action model
# ---------------------------------------------------------------------------
@dataclass
class Action:
    """Base. Every concrete action knows its raw bytes, a human label, and a
    JSON-serializable form. ``resize`` and ``wait`` produce no PTY bytes but are
    first-class steps so the log/replay captures timing and geometry changes.

    ``kind`` is a class attribute (not an init field) so positional construction
    binds to the first real field (e.g. ``Printable("z")`` -> text="z")."""

    kind: str = field(default="action", init=False)

    def to_bytes(self) -> bytes:
        return b""

    def to_label(self) -> str:
        return self.kind

    def serialize(self) -> dict:
        b = self.to_bytes()
        return {
            "kind": self.kind,
            "label": self.to_label(),
            "bytes_hex": b.hex(" "),
            "n": len(b),
        }


@dataclass
class Printable(Action):
    text: str = ""
    kind = "printable"

    def to_bytes(self) -> bytes:
        return self.text.encode("utf-8")

    def to_label(self) -> str:
        return f"print:{self.text!r}"

    def serialize(self) -> dict:
        d = super().serialize()
        d["text"] = self.text
        return d


@dataclass
class Byte(Action):
    value: int = 0
    kind = "byte"

    def to_bytes(self) -> bytes:
        return bytes([self.value & 0xFF])

    def to_label(self) -> str:
        return f"byte:0x{self.value:02x}"

    def serialize(self) -> dict:
        d = super().serialize()
        d["value"] = self.value
        return d


@dataclass
class CtrlA(Action):
    """Ctrl+A prefix followed by a categorized next byte.

    next:
      space  -> toggle Chinese/English mode
      s      -> toggle settings panel
      ctrla  -> forward a literal 0x01
      other  -> forward Ctrl+A + arbitrary byte (shell passthrough)
    """
    next: str = "space"
    other_byte: int = ord("x")
    kind = "ctrla"

    def _next_byte(self) -> int:
        if self.next == "space":
            return SPACE
        if self.next == "s":
            return ord("s")
        if self.next == "ctrla":
            return CTRL_A
        return self.other_byte & 0xFF

    def to_bytes(self) -> bytes:
        if self.next == "ctrla":
            # forward_literal_ctrl_a emits a single 0x01 (input_processor.hpp:111)
            return bytes([CTRL_A])
        return bytes([CTRL_A, self._next_byte()])

    def to_label(self) -> str:
        return f"Ctrl+A+{self.next}"

    def serialize(self) -> dict:
        d = super().serialize()
        d["next"] = self.next
        if self.next == "other":
            d["other_byte"] = self.other_byte
        return d


@dataclass
class Arrow(Action):
    dir: str = "A"  # A=up B=down C=right D=left
    kind = "arrow"

    def to_bytes(self) -> bytes:
        return bytes([ESC, ord("["), ord(self.dir)])

    def to_label(self) -> str:
        return f"arrow:{self.dir}"


@dataclass
class Enter(Action):
    kind = "enter"

    def to_bytes(self) -> bytes:
        return bytes([ENTER])


@dataclass
class Tab(Action):
    kind = "tab"

    def to_bytes(self) -> bytes:
        return bytes([TAB])


@dataclass
class Space(Action):
    kind = "space"

    def to_bytes(self) -> bytes:
        return bytes([SPACE])


@dataclass
class Backspace(Action):
    use_del: bool = False
    kind = "backspace"

    def to_bytes(self) -> bytes:
        return bytes([DEL if self.use_del else BACKSPACE])


@dataclass
class Esc(Action):
    """A lone ESC byte. Probes the SML Escape state (swallowed, no forward)."""
    kind = "esc"

    def to_bytes(self) -> bytes:
        return bytes([ESC])


@dataclass
class CsiMalformed(Action):
    """ESC [ followed by `digit_run` (digits/semicolons). If `terminate` is
    False, no final byte is sent — leaves the SML stuck in EscapeCSI with an
    ever-growing buffer_ (bug #1, input_processor.hpp:107)."""
    digit_run: str = "1;2"
    terminate: bool = False
    kind = "csi_malformed"

    def to_bytes(self) -> bytes:
        b = bytes([ESC, ord("[")]) + self.digit_run.encode("ascii")
        if self.terminate:
            b += bytes([ord("A")])  # arbitrary terminator
        return b

    def serialize(self) -> dict:
        d = super().serialize()
        d["digit_run"] = self.digit_run
        d["terminate"] = self.terminate
        return d


@dataclass
class Resize(Action):
    rows: int = 24
    cols: int = 80
    kind = "resize"

    def to_label(self) -> str:
        return f"resize:{self.rows}x{self.cols}"

    def serialize(self) -> dict:
        d = super().serialize()
        d["rows"] = self.rows
        d["cols"] = self.cols
        return d


@dataclass
class Wait(Action):
    ms: int = 100
    kind = "wait"

    def to_label(self) -> str:
        return f"wait:{self.ms}ms"

    def serialize(self) -> dict:
        d = super().serialize()
        d["ms"] = self.ms
        return d


# ---------------------------------------------------------------------------
# Weighted random generator
# ---------------------------------------------------------------------------
# (weight, factory). Weights sum to 100. Tuned to stress the confirmed bugs:
# Ctrl+A combos, malformed CSI, and (mode-biased) letters are over-weighted.
_WEIGHTS: List[tuple] = [
    ("letters", 22, lambda rng: Printable(rng.choice(string.ascii_lowercase))),
    ("digits", 10, lambda rng: Printable(rng.choice("123456789"))),
    ("space", 8, lambda rng: Space()),
    ("backspace", 8, lambda rng: Backspace(use_del=rng.random() < 0.5)),
    ("enter", 6, lambda rng: Enter()),
    ("tab", 4, lambda rng: Tab()),
    ("esc", 6, lambda rng: Esc()),
    ("arrow", 8, lambda rng: Arrow(rng.choice("ABCD"))),
    ("ctrla", 14, lambda rng: CtrlA(next=rng.choices(
        ["space", "a", "s", "ctrla", "other"],
        weights=[6, 3, 3, 1, 1])[0],
        other_byte=rng.randrange(32, 127))),
    ("csi_malformed", 6, lambda rng: CsiMalformed(
        digit_run="".join(rng.choice("0123456789;") for _ in range(rng.randint(1, 8))),
        terminate=rng.random() < 0.5)),
    ("symbols", 6, lambda rng: Printable(rng.choice(
        string.ascii_uppercase + "`-=[]\\;',./"))),
    ("resize", 2, lambda rng: Resize(rows=rng.randint(12, 60), cols=rng.randint(40, 200))),
    ("wait", 4, lambda rng: Wait(ms=rng.choice([50, 100, 150, 200, 300]))),
]

_TOTAL_W = sum(w for _, w, _ in _WEIGHTS)


def weighted_action(rng: random.Random, mode_bias: str = "en") -> Action:
    """Sample one action. ``mode_bias`` in {"en","zh"}: in zh mode, letter weight
    is boosted (drives IME composition) at the expense of symbols/enter."""
    names = [n for n, _, _ in _WEIGHTS]
    weights = [w for _, w, _ in _WEIGHTS]
    if mode_bias == "zh":
        # double letters, halve symbols & enter to spend more time composing
        for i, n in enumerate(names):
            if n == "letters":
                weights[i] *= 2
            elif n in ("symbols", "enter"):
                weights[i] = max(1, weights[i] // 2)
    pick = rng.choices(names, weights=weights)[0]
    for n, _, factory in _WEIGHTS:
        if n == pick:
            return factory(rng)
    raise RuntimeError("unreachable")


# ---------------------------------------------------------------------------
# Fixed probe sequences (P1..P6) — see docs/MONKEY_TESTING.md
# ---------------------------------------------------------------------------
def _p1_escapecsi() -> List[Action]:
    rng = random.Random(0xC51)  # deterministic within the probe
    flood = "".join(rng.choice("0123456789;") for _ in range(5000))
    return [
        Esc(),
        Byte(ord("[")),
        Printable(flood),  # 5000 non-terminator bytes -> EscapeCSI buffer_ grows
        Printable("x"),     # a normal byte after the flood
    ]


def _p2_toggle_mid_composition() -> List[Action]:
    return [
        CtrlA(next="space"),          # -> Chinese
        Printable("n"),               # start composing
        Printable("i"),               # composition grows; candidate bar expected
        CtrlA(next="space"),          # toggle to EN *without* cancel (bug #2)
        Printable("hello"),           # should be silently swallowed
    ]


def _p3_settings_esc() -> List[Action]:
    return [
        CtrlA(next="s"),              # open settings
        Esc(),                        # lone ESC — buggy: does NOT close
        Tab(),                        # reliable close
    ]


def _p6_exit_hang() -> List[Action]:
    """Just drives some input; the actual hang assertion is on tui_stop timing
    with a SIGTERM-trapping shell (bug #6)."""
    return [
        Printable("ls"),
        Enter(),
        Wait(ms=200),
    ]


PROBES = {
    "P1": _p1_escapecsi(),
    "P2": _p2_toggle_mid_composition(),
    "P3": _p3_settings_esc(),
    "P6": _p6_exit_hang(),
}


def reset_sequence() -> List[Action]:
    """INV-3: drive the app back to a clean [EN] state with no candidate bar.

    - lone Esc to nudge out of any Escape/EscapeCSI state (harmless if not in one)
    - Ctrl+A+Space up to twice to reach EN (idempotent: toggles CN<->EN)
    - Backspace x5 to clear shell line noise
    - final lone Esc
    """
    return [
        Esc(),
        CtrlA(next="space"),
        CtrlA(next="space"),   # two toggles return to original mode (EN by default)
        Backspace(), Backspace(), Backspace(), Backspace(), Backspace(),
        Esc(),
    ]


# ---------------------------------------------------------------------------
# JSONL logging / replay
# ---------------------------------------------------------------------------
@dataclass
class LogRecord:
    t: str  # "step" | "inv" | "finding"
    i: int
    seed: int
    data: dict = field(default_factory=dict)

    def to_json(self) -> str:
        return json.dumps({"t": self.t, "i": self.i, "seed": self.seed, **self.data},
                          ensure_ascii=False)


def write_log(path: Union[str, Path], records: List[LogRecord]) -> None:
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("w", encoding="utf-8") as f:
        for r in records:
            f.write(r.to_json() + "\n")


def parse_log(path: Union[str, Path]) -> Iterator[dict]:
    """Yield each JSONL record as a dict. Step records carry ``bytes_hex`` so a
    replayer can feed raw bytes without reconstructing Action objects."""
    with Path(path).open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                yield json.loads(line)


def replay_bytes_from_log(path: Union[str, Path]) -> Iterator[tuple]:
    """Yield (i, bytes) for each step record, in order — for raw PTY replay."""
    for rec in parse_log(path):
        if rec.get("t") == "step" and rec.get("bytes_hex"):
            yield rec["i"], bytes.fromhex(rec["bytes_hex"].replace(" ", ""))


# ---------------------------------------------------------------------------
# Standalone replay helper (exercises the model without the MCP tools)
# ---------------------------------------------------------------------------
def run_replay_to_pty(bin_path: Optional[str] = None, log_path: Optional[str] = None,
                      step_delay: float = 0.02) -> int:
    """Replay a recorded JSONL log into a real term-ime PTY. Used to confirm a
    finding reproduces without Claude/MCP. Returns 0 on clean replay, 1 if the
    process dies mid-replay."""
    import os
    import pty
    import select
    import time

    binary = Path(bin_path) if bin_path else DEFAULT_BIN
    log = Path(log_path) if log_path else None
    if not binary.exists():
        print(f"binary not found: {binary}", flush=True)
        return 1
    if not log or not log.exists():
        print(f"log not found: {log}", flush=True)
        return 1

    pid, master = pty.fork()
    if pid == 0:
        os.environ.setdefault("TERM", "xterm-256color")
        os.execvp(str(binary), [str(binary)])
        os._exit(1)

    import fcntl, struct, termios
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    flags = fcntl.fcntl(master, fcntl.F_GETFL)
    fcntl.fcntl(master, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    time.sleep(1.0)
    try:
        select.select([master], [], [], 0.1)
        os.read(master, 65536)
    except OSError:
        pass

    died = False
    try:
        for i, b in replay_bytes_from_log(log):
            try:
                _, alive = os.waitpid(pid, os.WNOHANG)
                if alive != 0:
                    print(f"[step {i}] process exited mid-replay", flush=True)
                    died = True
                    break
            except ChildProcessError:
                died = True
                break
            os.write(master, b)
            time.sleep(step_delay)
            try:
                while select.select([master], [], [], 0.0)[0]:
                    os.read(master, 65536)
            except OSError:
                pass
    finally:
        try:
            os.kill(pid, 15)
            os.waitpid(pid, 0)
        except Exception:
            pass
        try:
            os.close(master)
        except Exception:
            pass

    return 1 if died else 0


if __name__ == "__main__":
    import sys
    # `python3 tests/monkey_sequences.py <log>` replays a recorded log into a PTY.
    if len(sys.argv) >= 2:
        sys.exit(run_replay_to_pty(log_path=sys.argv[1]))
    # Otherwise print the probe catalog as a sanity check.
    print("term-ime monkey sequences. Probes:")
    for name, seq in PROBES.items():
        print(f"  {name}: {len(seq)} actions")
    print(f"reset_sequence: {len(reset_sequence())} actions")
    rng = random.Random(1)
    print("sample weighted actions (zh bias):")
    for _ in range(8):
        a = weighted_action(rng, "zh")
        print(f"  {a.to_label():24s} -> {a.to_bytes().hex(' ')}")
