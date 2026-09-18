#!/usr/bin/env python3
"""End-to-end test for the term-ime settings panel."""

import os
import sys
import time
import pty
import select
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

def test_settings():
    print("=== Settings Panel E2E Test ===\n")

    term_ime_path = './build/term-ime'

    if not os.path.exists(term_ime_path):
        print(f"Error: {term_ime_path} not found")
        print("Run: make -j$(nproc)")
        return False

    # Hermetic environment: keep config/logs out of the real HOME.
    tmp_home = tempfile.mkdtemp(prefix="term-ime-settings-e2e-")
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

    pid, master_fd = pty.fork()
    if pid == 0:
        # Child process - pty.fork() already made us a session leader,
        # so no os.setsid() here (it would fail with EPERM).
        os.execvp(term_ime_path, [term_ime_path])

    # Set terminal size
    winsize = struct.pack('HHHH', 24, 80, 0, 0)
    fcntl.ioctl(master_fd, termios.TIOCSWINSZ, winsize)

    # Non-blocking
    flags = fcntl.fcntl(master_fd, fcntl.F_GETFL)
    fcntl.fcntl(master_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    results = []

    def drain():
        """Discard unread output left over from a previous frame."""
        read_all(master_fd, 0.2)

    try:
        # Test 1: Startup. The first frame is painted only after librime has
        # deployed into the fresh HOME, so poll for the status bar.
        print("Test 1: Startup")
        ok, buf = poll_until(master_fd, b"", r'\[EN\]', timeout=30.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        print(f"  Screen preview: {repr(screen[:120])}")
        has_mode = ok and '[EN]' in screen
        print(f"  Status bar [EN] visible: {has_mode}")
        results.append(has_mode)

        # Test 2: Open settings (Ctrl+A, S). '界面语言' only exists inside the
        # panel; the status bar hint '^A S 设置' makes a bare '设置' ambiguous.
        # Retried a few times: a keystroke that lands while the app is still
        # starting up can be swallowed, and a retry pair (close + open) always
        # converges back to the open state.
        print("\nTest 2: Open settings (Ctrl+A, S)")
        has_panel = False
        for attempt in range(3):
            drain()
            send(master_fd, b"\x01s")  # Ctrl+A + S
            ok, buf = poll_until(master_fd, b"", r'界面语言', timeout=10.0)
            screen = clean_ansi(buf.decode('utf-8', errors='replace'))
            has_panel = ok and '设置' in screen and '界面语言' in screen
            print(f"  Attempt {attempt + 1}: panel visible: {has_panel}")
            if has_panel:
                break
        print(f"  Screen preview: {repr(screen[:200])}")
        results.append(has_panel)

        if not has_panel:
            print("  ERROR: settings panel not opened, aborting")
            print(f"\n=== Results: {sum(results)}/{len(results)} passed ===")
            return False

        # Test 3: The focused row renders the current value and the option
        # count. Default ui_language is zh-CN (the second option), so the
        # value shows '简体中文' at '2/2'.
        print("\nTest 3: Current value in focused row")
        m = re.search(r'界面语言:\s*\[([^\]]*)\]\s*<\s*(\d+)/(\d+)', screen)
        value = m.group(1) if m else None
        print(f"  Focused value: {value!r}")
        value_ok = bool(m) and value == '简体中文' and m.group(2) == '2' and m.group(3) == '2'
        print(f"  Value row matches contract: {value_ok}")
        results.append(value_ok)

        # Test 4: 'h' (left) moves zh-CN -> en; on_change re-labels the whole
        # panel, so the English labels are the observable proof it took effect.
        print("\nTest 4: Change value (h key)")
        drain()
        send(master_fd, b"h")
        ok, buf = poll_until(master_fd, b"", r'UI Language', timeout=10.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        print(f"  Screen preview: {repr(screen[:200])}")
        m = re.search(r'UI Language:\s*\[([^\]]*)\]\s*<\s*(\d+)/(\d+)', screen)
        changed = ok and bool(m) and m.group(1) == 'English' and m.group(2) == '1'
        print(f"  Value switched to English: {changed}")
        results.append(changed)

        # Test 5: ESC closes the panel; redraw_shell repaints the shell view.
        print("\nTest 5: Close settings (ESC)")
        drain()
        send(master_fd, b"\x1b")
        drain()
        # Probe the shell instead of matching a prompt string: the typed line and
        # the evaluated result differ, so only a live shell view prints the
        # marker. A prompt regex would depend on $SHELL and its startup files.
        send(master_fd, b"echo SHELLVIEW$((6*7))\r")
        ok, buf = poll_until(master_fd, b"", r'SHELLVIEW42', timeout=10.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        print(f"  Screen preview: {repr(screen[:200])}")
        # Panel-only markers (footer 'Up/Down', item labels) must be gone.
        panel_gone = ('Up/Down' not in screen
                      and 'UI Language' not in screen
                      and '界面语言' not in screen)
        shell_back = ok and 'SHELLVIEW42' in screen
        closed = panel_gone and shell_back
        print(f"  Shell view back (probe): {shell_back}, panel content gone: {panel_gone}")
        results.append(closed)

        # Test 6: Closing persists the changed setting into the hermetic
        # config. redraw_shell runs before save, so poll for the file.
        print("\nTest 6: Setting persisted to config")
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
        print(f"  {config_file} has ui_language=en: {persisted}")
        results.append(persisted)

        print(f"\n=== Results: {sum(results)}/{len(results)} passed ===")
        return all(results)

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
    sys.exit(0 if test_settings() else 1)
