#!/usr/bin/env python3
"""Quick end-to-end test for term-ime."""

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
import tempfile
import shutil
import json

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
        except:
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

def test_quick():
    print("=== Quick E2E Test ===\n")

    term_ime_path = os.environ.get('TERM_IME_BINARY', './build/term-ime')

    if not os.path.exists(term_ime_path):
        print(f"Error: {term_ime_path} not found")
        print("Run: make -j$(nproc)")
        return False

    # Hermetic environment: keep config/logs out of the real HOME.
    tmp_home = tempfile.mkdtemp(prefix="term-ime-e2e-")
    home = os.path.join(tmp_home, "home")
    config_home = os.path.join(tmp_home, "config")
    os.makedirs(home, exist_ok=True)
    os.makedirs(config_home, exist_ok=True)
    os.environ["HOME"] = home
    os.environ["XDG_CONFIG_HOME"] = config_home
    os.environ["XDG_DATA_HOME"] = os.path.join(tmp_home, "data")
    os.environ["TERM"] = "xterm-256color"
    custom_rime = os.path.join(tmp_home, "custom-rime-user")
    os.makedirs(os.path.join(config_home, "term-ime"), exist_ok=True)
    with open(os.path.join(config_home, "term-ime", "config.json"), "w") as config:
        json.dump({"rime_user_data_dir": custom_rime, "fuzzy_pinyin": False}, config)

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

    try:
        # Test 1: Startup. The first frame is painted only after librime has
        # deployed into the fresh HOME (takes ~5s), so poll for it.
        print("Test 1: Startup")
        buf = b""
        ok, buf = poll_until(master_fd, buf, r'\[EN\]|\[拼\]', timeout=20.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        print(f"  Screen length: {len(screen)} chars")
        print(f"  Screen preview: {repr(screen[:100])}")
        mode = re.search(r'\[EN\]|\[拼\]', screen)
        if ok and mode and mode.group(0) == '[EN]':
            print("  ✓ Mode indicator [EN] found (English default)")
            results.append(True)
        else:
            print(f"  ✗ No [EN] mode indicator (found {mode.group(0) if mode else 'none'})")
            results.append(False)

        print("Checking configured Rime user data directory")
        configured_data_exists = os.path.isdir(os.path.join(custom_rime, "build"))
        print(f"  custom data build exists: {configured_data_exists}")
        results.append(configured_data_exists)

        # Test 2: Toggle to Chinese mode
        print("\nTest 2: Toggle to Chinese mode (Ctrl+A, Space)")
        send(master_fd, b"\x01 ")  # Ctrl+A + Space
        buf = b""
        ok, buf = poll_until(master_fd, buf, r'\[拼\]', timeout=5.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        mode = '[拼]' if '[拼]' in screen else 'none'
        print(f"  Mode: {mode}")
        results.append(ok and '[拼]' in screen)

        # Test 3: Input pinyin
        print("\nTest 3: Pinyin input 'nihao'")
        send(master_fd, b"nihao")
        buf = b""
        ok, buf = poll_until(master_fd, buf, r'你好', timeout=5.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        print(f"  Screen preview: {repr(screen[:100])}")
        if ok and '你好' in screen:
            print("  ✓ Input recognized (candidate 你好)")
            results.append(True)
        else:
            print("  ✗ No input recognized")
            results.append(False)

        # Cancel the composition with ESC
        send(master_fd, b"\x1b")
        time.sleep(0.3)
        read_all(master_fd, 0.3)

        # The single batch must both compose and select. No intermediate
        # candidate frame is painted, so seeing the character here verifies
        # actual shell echo of committed text rather than a candidate label.
        print("Checking same-batch ni1 commit")
        send(master_fd, b"ni1")
        ok, buf = poll_until(master_fd, b"", r'你', timeout=5.0)
        print(f"  committed character echoed: {ok}")
        results.append(ok)
        send(master_fd, b"\x03")  # discard the shell line without executing it
        time.sleep(0.1)
        read_all(master_fd, 0.3)

        # Test 4: Toggle back to English
        print("\nTest 4: Toggle to English mode (Ctrl+A, Space)")
        send(master_fd, b"\x01 ")  # Ctrl+A + Space
        buf = b""
        ok, buf = poll_until(master_fd, buf, r'\[EN\]', timeout=5.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        mode = '[EN]' if '[EN]' in screen else ('[拼]' if '[拼]' in screen else 'none')
        print(f"  Mode: {mode}")
        results.append(ok and '[EN]' in screen)

        # Test 5: Settings panel. '界面语言' appears only inside the panel
        # (the help line's '^A S 设置' makes a bare '设置' check ambiguous).
        print("\nTest 5: Settings panel (Ctrl+A, S)")
        send(master_fd, b"\x01s")  # Ctrl+A + S
        buf = b""
        ok, buf = poll_until(master_fd, buf, r'界面语言', timeout=5.0)
        screen = clean_ansi(buf.decode('utf-8', errors='replace'))
        print(f"  Screen preview: {repr(screen[:100])}")
        if ok and '界面语言' in screen:
            print("  ✓ Settings panel opened")
            results.append(True)
        else:
            print("  ✗ Settings not detected")
            results.append(False)

        print(f"\n=== Results: {sum(results)}/{len(results)} passed ===")

    finally:
        try:
            os.kill(pid, signal.SIGTERM)
            os.waitpid(pid, 0)
        except:
            pass
        try:
            os.close(master_fd)
        except:
            pass
        shutil.rmtree(tmp_home, ignore_errors=True)

    return bool(results) and all(results)

if __name__ == "__main__":
    sys.exit(0 if test_quick() else 1)