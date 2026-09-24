#!/usr/bin/env python3
"""Host-only integration checks. The service is a foreground child; no system socket is used."""
import argparse
import hashlib
import os
import pathlib
import signal
import socket
import struct
import subprocess
import tempfile
import time

MAGIC = 0x4331494D
VERSION = 1
HEADER = 32
REPLY = 40
OP_STATUS, OP_MODE, OP_PURPOSE, OP_KEY, OP_SELECT, OP_PAGE, OP_CANCEL = range(1, 8)
PASSWORD = 1
READY, CONSUMED, CHINESE, PASSWORD_FLAG, COMPOSING = 1, 2, 4, 8, 16

def packet(op, seq, keysym=0, modifiers=0, value=0):
    return struct.pack(">IHHIIIIII", MAGIC, VERSION, op, seq, HEADER, keysym, modifiers, value, 0)

def receive(sock, op, seq, expected_keysym=0):
    data = sock.recv(8192)
    assert len(data) >= REPLY
    magic, version, actual_op, actual_seq, length, keysym, flags, status, reserved, pre_len, commit_len, count, reserved2 = struct.unpack(">IHHIIIIIIHHHH", data[:REPLY])
    assert magic == MAGIC and version == VERSION and actual_op == (op | 0x8000) and actual_seq == seq
    assert length == len(data) and status in (0, 1, 2) and reserved == 0 and reserved2 == 0
    assert keysym == expected_keysym
    assert count <= 5 and pre_len < 1024 and commit_len < 1024
    pos = REPLY
    preedit = data[pos:pos + pre_len].decode("utf-8"); pos += pre_len
    commit = data[pos:pos + commit_len].decode("utf-8"); pos += commit_len
    candidates = []
    for _ in range(count):
        size = struct.unpack(">H", data[pos:pos + 2])[0]; pos += 2
        candidates.append(data[pos:pos + size].decode("utf-8")); pos += size
    assert pos == len(data)
    return flags, status, preedit, commit, candidates

def request(sock, op, seq, keysym=0, value=0):
    sock.send(packet(op, seq, keysym=keysym, value=value))
    return receive(sock, op, seq, expected_keysym=keysym)

def wait_for_socket(path, process):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"service exited with {process.returncode}: {process.stderr.read()}")
        if path.exists():
            return
        time.sleep(.05)
    raise TimeoutError("service did not create its private socket")

def run(args):
    with tempfile.TemporaryDirectory(prefix="c1-ime-test-") as root:
        root = pathlib.Path(root)
        runtime = root / "runtime"; runtime.mkdir(mode=0o700)
        path = runtime / "socket"
        user = root / "user"; user.mkdir(mode=0o700)
        command = [args.service, "--socket", str(path), "--shared-data", str(pathlib.Path(args.shared_data).resolve()), "--user-data", str(user)]
        service = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            wait_for_socket(path, service)
            assert path.stat().st_mode & 0o777 == 0o600
            assert runtime.stat().st_mode & 0o777 == 0o700
            # The C17 SDK executes the real nihao/commit/mode/password flow.
            env = dict(os.environ, C1_IME_TEST_SOCKET=str(path))
            result = subprocess.run([args.sdk_test], env=env, capture_output=True, text=True, timeout=60)
            assert result.returncode == 0, (result.stdout, result.stderr)

            if args.demo:
                demo = subprocess.run([args.demo, str(path)], capture_output=True, text=True, timeout=10)
                assert demo.returncode == 0 and 'commit: 你好' in demo.stdout, (demo.stdout, demo.stderr)

            # One focused client only: a second connection is closed immediately.
            first = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            first.connect(str(path))
            first.settimeout(10)
            first.send(packet(OP_STATUS, 1))
            receive(first, OP_STATUS, 1)
            second = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            second.connect(str(path))
            second.settimeout(2)
            try:
                assert second.recv(8192) == b""
            except ConnectionResetError:
                pass
            second.close(); first.close()

            # Invalid length and oversized records are rejected without Rime input.
            bad = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); bad.connect(str(path))
            bad.send(packet(OP_STATUS, 1) + b"x")
            bad.settimeout(2)
            assert bad.recv(8192) == b""
            bad.close()
            huge = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); huge.connect(str(path))
            huge.send(b"x" * 8193)
            huge.settimeout(2)
            assert huge.recv(8192) == b""
            huge.close()

            # Focus loss must cancel composition; a fresh session starts English.
            stale = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); stale.connect(str(path))
            stale.send(packet(OP_MODE, 1, value=1)); receive(stale, OP_MODE, 1)
            stale.send(packet(OP_KEY, 2, keysym=ord("n"))); flags, _, preedit, _, _ = receive(stale, OP_KEY, 2, expected_keysym=ord("n"))
            assert flags & CONSUMED and preedit
            stale.close(); time.sleep(.1)
            fresh = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); fresh.connect(str(path))
            flags, _, preedit, _, _ = request(fresh, OP_STATUS, 1)
            assert flags & READY and not flags & CHINESE and not preedit
            fresh.close()

            # A second process never unlinks/replaces a live socket.
            inode = path.stat().st_ino
            duplicate = subprocess.run(command, capture_output=True, timeout=10)
            assert duplicate.returncode != 0 and path.stat().st_ino == inode
            alternate = runtime / 'other-socket'
            locked = subprocess.run([args.service, '--socket', str(alternate), '--shared-data', args.shared_data,
                                     '--user-data', str(user)], capture_output=True, timeout=10)
            assert locked.returncode != 0 and not alternate.exists()
            assert path.stat().st_ino == inode
            # Existing regular files and insecure directories are never replaced.
            existing = runtime / 'existing'
            existing.write_text('keep me')
            collision = subprocess.run([args.service, '--socket', str(existing), '--shared-data', args.shared_data,
                                        '--user-data', str(user)], capture_output=True, timeout=10)
            assert collision.returncode != 0 and existing.read_text() == 'keep me'
            insecure = root / 'insecure'; insecure.mkdir(mode=0o755)
            rejected = subprocess.run([args.service, '--socket', str(insecure / 'socket'),
                                       '--shared-data', args.shared_data, '--user-data', str(user)],
                                      capture_output=True, timeout=10)
            assert rejected.returncode != 0 and not (insecure / 'socket').exists()

            # Stateful operations are checked against the actual Rime engine.
            active = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            active.settimeout(10)
            active.connect(str(path))
            sequence = 0
            def call(op, keysym=0, value=0):
                nonlocal sequence
                sequence += 1
                return request(active, op, sequence, keysym=keysym, value=value)
            call(OP_STATUS)
            call(OP_MODE, value=1)
            _, _, _, _, page_one = call(OP_KEY, keysym=ord('n'))
            assert len(page_one) == 5
            _, _, _, _, page_two = call(OP_PAGE, value=1)
            assert len(page_two) == 5 and page_two != page_one
            _, _, _, _, previous = call(OP_PAGE, value=0)
            assert previous == page_one
            _, _, _, commit, _ = call(OP_SELECT, value=4)
            assert commit == page_one[4]
            call(OP_KEY, keysym=ord('n'))
            flags, _, preedit, commit, _ = call(OP_KEY, keysym=0xff08)
            assert flags & CONSUMED and not preedit and not commit
            call(OP_KEY, keysym=ord('n'))
            flags, _, preedit, commit, _ = call(OP_KEY, keysym=0xff1b)
            assert flags & CONSUMED and not preedit and not commit
            for c in 'nihao': call(OP_KEY, keysym=ord(c))
            _, _, _, commit, _ = call(OP_KEY, keysym=0xff0d)
            assert commit == '你好'
            call(OP_KEY, keysym=ord('n'))
            _, _, preedit, commit, _ = call(OP_CANCEL)
            assert not preedit and not commit
            call(OP_KEY, keysym=ord('n'))
            call(OP_PURPOSE, value=PASSWORD)
            def userdb_snapshot():
                return {str(p.relative_to(user)): hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in user.rglob('*') if p.is_file() and
                        any('.userdb' in part for part in p.parts)}
            before = userdb_snapshot()
            assert before, 'real Rime user database must exist for this check'
            for c in 'nihao password 123!':
                flags, _, preedit, commit, candidates = call(OP_KEY, keysym=ord(c))
                assert flags & PASSWORD_FLAG and flags & CHINESE
                assert not flags & (CONSUMED | COMPOSING)
                assert not preedit and not commit and not candidates
            assert before == userdb_snapshot(), 'password input changed user dictionary'
            _, _, preedit, commit, candidates = call(OP_PURPOSE, value=0)
            assert not preedit and not commit and not candidates
            active.close()

            # Unsupported versions, flags, operations, duplicate/out-of-order
            # sequences are rejected before any key can reach Rime.
            malformed = [packet(OP_STATUS, 0), packet(OP_STATUS, 2),
                         packet(99, 1), packet(OP_KEY, 1, keysym=ord('a'), modifiers=0x80000000)]
            wrong_version = bytearray(packet(OP_STATUS, 1)); wrong_version[5] = 2
            malformed.append(wrong_version)
            for payload in malformed:
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as bad:
                    bad.settimeout(2); bad.connect(str(path)); bad.send(payload)
                    try: assert bad.recv(8192) == b''
                    except ConnectionResetError: pass
        finally:
            service.send_signal(signal.SIGTERM)
            try: service.wait(timeout=10)
            except subprocess.TimeoutExpired: service.kill(); service.wait()
            if service.returncode not in (0, -signal.SIGTERM):
                raise RuntimeError(service.stderr.read())
        assert not path.exists(), 'service did not remove its own socket'

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", required=True)
    parser.add_argument("--sdk-test", required=True)
    parser.add_argument("--demo")
    parser.add_argument("--shared-data", required=True)
    run(parser.parse_args())
