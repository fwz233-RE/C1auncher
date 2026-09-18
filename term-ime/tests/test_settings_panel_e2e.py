#!/usr/bin/env python3
"""
End-to-end test for term-ime settings panel.
Simulates real user operations via PTY.
"""

import os
import sys
import pty
import select
import time
import struct
import fcntl
import termios
import signal
import re
import shutil
import tempfile

def clean_ansi(text):
    """Remove ANSI escape sequences."""
    text = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', text)
    text = re.sub(r'\x1b\].*?\x07', '', text)
    text = re.sub(r'\x1b\[\?[0-9;]*[a-zA-Z]', '', text)
    return text

def read_all(fd, timeout=0.5):
    """Read all available data with timeout."""
    output = b""
    end_time = time.time() + timeout
    while time.time() < end_time:
        try:
            ready, _, _ = select.select([fd], [], [], 0.1)
            if not ready:
                break
            data = os.read(fd, 4096)
            if data:
                output += data
        except OSError:
            break
    return output

def poll_until(fd, buf, pattern, timeout=10.0):
    """Read from fd into buf until the cleaned text matches pattern, or timeout.

    Returns (matched, buf).
    """
    rx = re.compile(pattern) if isinstance(pattern, str) else pattern
    end_time = time.time() + timeout
    while time.time() < end_time:
        try:
            ready, _, _ = select.select([fd], [], [], 0.1)
            if ready:
                data = os.read(fd, 4096)
                if not data:
                    break
                buf += data
        except OSError:
            break
        if rx.search(clean_ansi(buf.decode('utf-8', errors='replace'))):
            return True, buf
    return False, buf

def send(fd, data):
    """Write to the pty, ignoring errors once the session is dead."""
    try:
        os.write(fd, data)
    except OSError:
        pass

def test_settings_panel():
    """Test settings panel operations."""
    print("=" * 60)
    print("Settings Panel End-to-End Test")
    print("=" * 60)

    term_ime_path = "/home/gem/project/term-ime/build/term-ime"
    if not os.path.exists(term_ime_path):
        print(f"ERROR: {term_ime_path} not found")
        return False

    # Hermetic environment: keep config/logs out of the real HOME.
    tmp_home = tempfile.mkdtemp(prefix="term-ime-panel-e2e-")
    home = os.path.join(tmp_home, "home")
    config_home = os.path.join(tmp_home, "config")
    os.makedirs(home, exist_ok=True)
    os.makedirs(config_home, exist_ok=True)
    os.environ["HOME"] = home
    os.environ["XDG_CONFIG_HOME"] = config_home
    os.environ["TERM"] = "xterm-256color"
    # Pin the shell the app spawns: otherwise it inherits the caller's $SHELL,
    # and a fresh HOME makes zsh run zsh-newuser-install (a wizard banner, never
    # a prompt), which makes every prompt-based assertion depend on the caller.
    os.environ["SHELL"] = "/bin/sh"

    # Create a new session with proper TTY
    pid, master_fd = pty.fork()
    if pid == 0:
        # Child process - pty.fork() already made us a session leader,
        # so no os.setsid() here (it would fail with EPERM).
        os.chdir("/home/gem/project/term-ime")
        os.execvp(term_ime_path, [term_ime_path])
        # If exec fails
        print(f"Failed to exec {term_ime_path}", file=sys.stderr)
        os._exit(1)

    # Set terminal size
    winsize = struct.pack('HHHH', 24, 80, 0, 0)
    fcntl.ioctl(master_fd, termios.TIOCSWINSZ, winsize)

    # Non-blocking
    flags = fcntl.fcntl(master_fd, fcntl.F_GETFL)
    fcntl.fcntl(master_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    results = []

    def drain(fd):
        """Discard unread output left over from a previous frame."""
        read_all(fd, 0.2)

    def row_focus_re(label):
        """Regex matching a focused value row: 'label: [value] < n/m'."""
        return re.compile(re.escape(label) + r':\s*\[([^\]]*)\]\s*<\s*(\d+)/(\d+)')

    try:
        # Test 1: Startup. First frame is painted only after librime deploys
        # into the fresh HOME, so poll for the status bar.
        print("\n[Test 1] Startup")
        ok, buf = poll_until(master_fd, b"", r'\[EN\]', timeout=30.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        print(f"  Output length: {len(buf)} bytes")
        print(f"  Screen preview: {repr(screen[:100])}")
        has_status = ok and '[EN]' in screen
        print(f"  Status bar [EN] visible: {has_status}")
        results.append(("Startup", has_status))

        # Test 2: Open settings panel. '界面语言' only exists inside the panel;
        # the status bar hint '^A S 设置' makes a bare '设置' ambiguous.
        # Retried a few times: a keystroke that lands while the app is still
        # starting up can be swallowed, and a retry pair (close + open) always
        # converges back to the open state.
        print("\n[Test 2] Open settings panel (Ctrl+A, S)")
        has_panel = False
        for attempt in range(3):
            drain(master_fd)
            send(master_fd, b"\x01s")  # Ctrl+A + S
            ok, buf = poll_until(master_fd, b"", r'界面语言', timeout=10.0)
            screen = clean_ansi(buf.decode('utf-8', errors='replace'))
            has_panel = ok and '界面语言' in screen and '关闭' in screen
            print(f"  Attempt {attempt + 1}: panel detected: {has_panel}")
            if has_panel:
                break
        print(f"  Output length: {len(buf)} bytes")
        print(f"  Screen preview: {repr(screen[:150])}")
        results.append(("Open settings", has_panel))

        if not has_panel:
            print("  ERROR: Settings panel not opened!")
            print(f"  Raw output: {repr(buf[:300])}")
            print("\n" + "=" * 60)
            print("RESULTS SUMMARY")
            print("=" * 60)
            for name, r in results:
                print(f"  [{'PASS' if r else 'FAIL'}] {name}")
            print(f"\nTotal: {sum(1 for _, r in results if r)}/{len(results)} passed")
            return False

        # Test 3: Navigate down with 'j'. Focus moves from the ui-language row
        # to the candidates row (the panel has two rows plus the Close item).
        print("\n[Test 3] Navigate down (j key)")
        drain(master_fd)
        send(master_fd, b"j")
        ok, buf = poll_until(master_fd, b"", row_focus_re('候选词数量'), timeout=5.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        print(f"  Screen preview: {repr(screen[:150])}")
        m = row_focus_re('候选词数量').search(screen)
        cand_focused = ok and bool(m) and m.group(1) == '9' and m.group(2) == '9'
        ui_unfocused = '界面语言:' in screen and '界面语言: [' not in screen
        print(f"  Candidates row focused: {cand_focused}, ui row unfocused: {ui_unfocused}")
        results.append(("Navigate j", cand_focused and ui_unfocused))

        # Test 4: Arrow navigation. Down walks on to the fuzzy row and then the
        # Close item; up walks back to the candidates row.
        print("\n[Test 4] Navigate with arrow keys")
        drain(master_fd)
        send(master_fd, b"\x1b[B")  # ESC [ B (down arrow)
        ok, buf = poll_until(master_fd, b"", row_focus_re('模糊音'), timeout=5.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        print(f"  Screen preview: {repr(screen[:150])}")
        fuzzy_focused = ok and bool(row_focus_re('模糊音').search(screen))
        drain(master_fd)
        send(master_fd, b"\x1b[B")  # down again -> Close
        ok, buf = poll_until(master_fd, b"", r'>\s*关闭\s*<', timeout=5.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        close_focused = ok and bool(re.search(r'>\s*关闭\s*<', screen))
        drain(master_fd)
        send(master_fd, b"\x1b[A\x1b[A")  # up twice -> candidates row
        ok, buf = poll_until(master_fd, b"", row_focus_re('候选词数量'), timeout=5.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        back_ok = ok and bool(row_focus_re('候选词数量').search(screen))
        print(f"  Fuzzy row: {fuzzy_focused}, close row: {close_focused}, back: {back_ok}")
        results.append(("Arrow down", fuzzy_focused and close_focused and back_ok))

        # Test 5: 'h' on the candidates row lowers the cap (focus is already on
        # that row after test 4).
        print("\n[Test 5] Change value (h key)")
        drain(master_fd)
        send(master_fd, b"h")
        ok, buf = poll_until(master_fd, b"", row_focus_re('候选词数量'), timeout=5.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        print(f"  Screen preview: {repr(screen[:150])}")
        m = row_focus_re('候选词数量').search(screen)
        lowered = ok and bool(m) and m.group(1) == '8' and m.group(2) == '8'
        print(f"  Candidate cap lowered to 8: {lowered}")
        results.append(("Change value h", lowered))

        # Test 6: 'k' back up to the ui-language row, then 'h' switches the UI
        # language; the panel re-labels itself in English.
        print("\n[Test 6] Navigate up (k key) and switch UI language")
        drain(master_fd)
        send(master_fd, b"k")
        poll_until(master_fd, b"", row_focus_re('界面语言'), timeout=5.0)
        drain(master_fd)
        send(master_fd, b"h")
        ok, buf = poll_until(master_fd, b"", row_focus_re('UI Language'), timeout=10.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        print(f"  Screen preview: {repr(screen[:150])}")
        m = row_focus_re('UI Language').search(screen)
        switched = ok and bool(m) and m.group(1) == 'English' and m.group(2) == '1'
        print(f"  Ui language switched to English: {switched}")
        results.append(("Navigate k", switched))

        # Test 7: Close settings with ESC. ESC closes the panel and redraws
        # the shell view. The panel content must be gone and the shell prompt
        # visible again; the changed setting must also be persisted.
        print("\n[Test 7] Close settings (ESC)")
        drain(master_fd)
        send(master_fd, b"\x1b")
        drain(master_fd)
        # Probe the shell rather than matching a prompt string: $SHELL and its
        # startup files decide what a prompt looks like, so only the evaluated
        # probe proves the shell view is live again.
        send(master_fd, b"echo SHELLVIEW$((6*7))\r")
        ok, buf = poll_until(master_fd, b"", r'SHELLVIEW42', timeout=10.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        print(f"  Output length: {len(buf)} bytes")
        print(f"  Screen preview: {repr(screen[:200])}")
        panel_gone = ('Up/Down' not in screen
                      and 'UI Language' not in screen
                      and '界面语言' not in screen
                      and 'Close' not in screen)
        shell_back = ok and 'SHELLVIEW42' in screen
        config_file = os.path.join(config_home, "term-ime", "config.json")
        persisted = False
        end_time = time.time() + 5.0
        while time.time() < end_time:
            try:
                with open(config_file) as f:
                    saved = f.read()
                if '"ui_language": "en"' in saved:
                    persisted = True
                    break
            except OSError:
                pass
            time.sleep(0.1)
        closed = shell_back and panel_gone and persisted
        print(f"  Shell view back: {shell_back}, panel gone: {panel_gone}, "
              f"config persisted: {persisted}")
        results.append(("Close settings", closed))

        # Summary
        print("\n" + "=" * 60)
        print("RESULTS SUMMARY")
        print("=" * 60)
        passed = sum(1 for _, r in results if r)
        total = len(results)
        for name, result in results:
            status = "PASS" if result else "FAIL"
            print(f"  [{status}] {name}")
        print(f"\nTotal: {passed}/{total} passed")

        return passed == total

    finally:
        try:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        except OSError:
            pass
        try:
            os.close(master_fd)
        except OSError:
            pass
        shutil.rmtree(tmp_home, ignore_errors=True)

if __name__ == "__main__":
    success = test_settings_panel()
    sys.exit(0 if success else 1)
