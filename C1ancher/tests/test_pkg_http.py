#!/usr/bin/env python3
"""Loopback-only package HTTP regression suite; builds an isolated host driver.

Usage: python3 tests/test_pkg_http.py --build-dir build/lifecycle-fixes
Add --repo-regression to run the existing repository/cache suite with a unique
state root, avoiding the legacy executable's fixed /tmp directory collision.
No production URLs, package state, signing artifacts or private keys are used.
"""
import argparse
import errno
import http.server
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = bytes(range(256)) * 512
PREFIX = 16384
DRIVER = None


class Fixture(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, scenario):
        super().__init__(("127.0.0.1", 0), Handler)
        self.scenario = scenario
        self.requests = []

    def __enter__(self):
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *unused):
        self.shutdown()
        self.server_close()
        self.thread.join()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *unused):
        pass

    def reply(self, status, body, headers=None, advertised=None):
        self.send_response(status)
        self.send_header("Connection", "close")
        self.send_header("Content-Length", str(len(body) if advertised is None else advertised))
        for name, value in headers or []:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        self.close_connection = True

    def do_GET(self):
        try:
            self.handle_request()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def handle_request(self):
        scenario = self.server.scenario
        range_value = self.headers.get("Range")
        self.server.requests.append(range_value)
        number = len(self.server.requests)
        if scenario == "busy" or (scenario == "busy-then-ok" and number <= 2):
            self.reply(429 if number == 1 else 503, b"busy", [("Retry-After", "0")])
            return
        if scenario in ("long-wait", "date-wait") and number == 1:
            headers = [("Retry-After", "75")]
            if scenario == "date-wait":
                headers = [("Retry-After", "Sun, 06 Nov 1994 08:50:52 GMT"),
                           ("Date", "Sun, 06 Nov 1994 08:49:37 GMT")]
            self.reply(503, b"busy", headers)
            return
        if scenario == "success-cooldown" and number == 1:
            self.reply(200, PAYLOAD, [("Retry-After", "75")])
            return
        if scenario in ("transient-then-ok", "headerless-busy") and number <= 2:
            self.reply(502 if scenario == "transient-then-ok" else 429, b"busy")
            return
        if scenario == "stall":
            time.sleep(3)
            self.reply(200, PAYLOAD)
            return
        if scenario == "oversize":
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            # Deliberately omit Content-Length: the parent must count bytes.
            self.wfile.write(PAYLOAD * 4)
            self.wfile.flush()
            self.close_connection = True
            return
        if scenario == "unsolicited":
            self.reply(206, PAYLOAD, [("Content-Range", f"bytes 0-{len(PAYLOAD)-1}/{len(PAYLOAD)}")])
            return
        resumable = scenario not in ("ok", "busy-then-ok", "long-wait", "date-wait",
                                     "success-cooldown", "transient-then-ok", "headerless-busy")
        if resumable and number == 1:
            self.reply(200, PAYLOAD[:PREFIX], advertised=len(PAYLOAD))
            self.connection.shutdown(socket.SHUT_RDWR)
            return
        if range_value:
            offset = int(range_value.removeprefix("bytes=").removesuffix("-"))
            if scenario == "ignore":
                self.reply(200, PAYLOAD)
                return
            if scenario == "416":
                self.reply(416, b"", [("Content-Range", f"bytes */{len(PAYLOAD)}")])
                return
            if scenario == "resume-busy" and number == 2:
                self.reply(503, b"busy", [("Retry-After", "0")])
                return
            body = PAYLOAD[offset:]
            content_range = f"bytes {offset}-{len(PAYLOAD)-1}/{len(PAYLOAD)}"
            if scenario == "wrong-start":
                content_range = f"bytes 0-{len(PAYLOAD)-1}/{len(PAYLOAD)}"
            elif scenario == "overflow":
                content_range = f"bytes {offset}-{len(PAYLOAD)-1}/184467440737095516160"
            elif scenario == "large-total":
                content_range = f"bytes {offset}-{len(PAYLOAD)-1}/{len(PAYLOAD)*2}"
            elif scenario == "range-oversize":
                body += PAYLOAD
            elif (scenario == "multi-partial" and number < 4) or scenario == "always-partial":
                body = PAYLOAD[offset:offset + PREFIX]
                self.reply(206, body, [("Content-Range", content_range)], advertised=len(PAYLOAD)-offset)
                return
            headers = [("Content-Range", content_range)]
            if scenario == "duplicate":
                headers += [("Content-Range", content_range)]
            if scenario == "encoded":
                headers += [("Content-Encoding", "gzip")]
            self.reply(206, body, headers)
            return
        self.reply(200, PAYLOAD)


class HTTPTests(unittest.TestCase):
    def run_fetch(self, scenario, mode="package", limit=len(PAYLOAD), cancel_ms=0):
        with tempfile.TemporaryDirectory(prefix="c1pkg-http-") as directory, Fixture(scenario) as server:
            output = Path(directory) / "package.tmp"
            env = dict(os.environ, NO_PROXY="*", no_proxy="*")
            completed = subprocess.run(
                [str(DRIVER), f"http://127.0.0.1:{server.server_port}/payload", str(output),
                 str(limit), mode, str(cancel_ms)], text=True, capture_output=True, env=env, timeout=15)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            match = re.search(r"RESULT (-?\d+) ERRNO (\d+) MAXSIZE (\d+) ELAPSED (\d+)", completed.stdout)
            self.assertIsNotNone(match, completed.stdout)
            result, error_number, largest, elapsed = map(int, match.groups())
            self.assertLessEqual(largest, limit, completed.stdout)
            content = output.read_bytes() if output.exists() else None
            self.assertFalse(list(Path(directory).glob("*.headers.*")), "headers leaked")
            return result, error_number, content, list(server.requests), completed.stdout, elapsed

    def assert_success(self, scenario, requests):
        result, _, body, actual, text, _ = self.run_fetch(scenario)
        self.assertEqual(result, 0, text)
        self.assertEqual(body, PAYLOAD)
        self.assertEqual(actual, requests)
        self.assertIn("正在下载软件包", text)
        return text

    def test_disconnect_then_206(self):
        text = self.assert_success("resume", [None, f"bytes={PREFIX}-"])
        self.assertIn("正在继续下载软件包", text)

    def test_multiple_partial_resumes(self):
        self.assert_success("multi-partial", [None] + [f"bytes={PREFIX*i}-" for i in range(1, 4)])

    def test_interrupted_retry_bounds(self):
        result, _, body, requests, text, _ = self.run_fetch("always-partial")
        self.assertNotEqual(result, 0, text)
        self.assertIsNone(body)
        self.assertEqual(requests, [None] + [f"bytes={PREFIX*i}-" for i in range(1, 4)])
        self.assertIn("重试上限", text)

    def test_ignored_range_restarts_without_concatenation(self):
        self.assert_success("ignore", [None, f"bytes={PREFIX}-", None])

    def test_416_restarts_without_concatenation(self):
        self.assert_success("416", [None, f"bytes={PREFIX}-", None])

    def test_resume_busy_retains_only_valid_prefix(self):
        self.assert_success("resume-busy", [None, f"bytes={PREFIX}-", f"bytes={PREFIX}-"])

    def test_invalid_ranges_are_rejected(self):
        for scenario in ("wrong-start", "overflow", "large-total", "duplicate", "encoded", "unsolicited"):
            with self.subTest(scenario=scenario):
                result, _, body, requests, text, _ = self.run_fetch(scenario)
                self.assertNotEqual(result, 0, text)
                self.assertIsNone(body)
                self.assertEqual(len(requests), 1 if scenario == "unsolicited" else 2)

    def test_streaming_size_limit(self):
        result, error_number, body, requests, text, _ = self.run_fetch("oversize", limit=1000)
        self.assertNotEqual(result, 0, text)
        self.assertEqual(error_number, errno.EFBIG)
        self.assertIsNone(body)
        self.assertEqual(requests, [None])
        self.assertIn("size limit", text)

    def test_resumed_size_limit(self):
        result, error_number, body, requests, text, _ = self.run_fetch("range-oversize")
        self.assertNotEqual(result, 0, text)
        self.assertEqual(error_number, errno.EFBIG)
        self.assertIsNone(body)
        self.assertEqual(requests, [None, f"bytes={PREFIX}-"])

    def test_active_cancel(self):
        result, error_number, body, requests, text, elapsed = self.run_fetch("stall", cancel_ms=250)
        self.assertNotEqual(result, 0, text)
        self.assertEqual(error_number, errno.ECANCELED)
        self.assertIsNone(body)
        self.assertEqual(requests, [None])
        self.assertLess(elapsed, 1500)
        self.assertTrue("操作已取消" in text or "Cancelled" in text, text)

    def test_cancel_before_resume(self):
        result, error_number, body, requests, text, _ = self.run_fetch("resume", mode="cancel-resume")
        self.assertNotEqual(result, 0, text)
        self.assertEqual(error_number, errno.ECANCELED)
        self.assertIsNone(body)
        self.assertEqual(requests, [None])

    def test_metadata_retries_fresh_without_range(self):
        result, _, body, requests, text, _ = self.run_fetch("resume", mode="metadata")
        self.assertEqual(result, 0, text)
        self.assertEqual(body, PAYLOAD)
        self.assertEqual(requests, [None, None])
        self.assertIn("正在检查软件仓库", text)
        self.assertNotIn("正在下载软件包", text)

    def test_metadata_exact_small_limit(self):
        result, error_number, body, _, text, _ = self.run_fetch("oversize", mode="metadata", limit=64)
        self.assertNotEqual(result, 0, text)
        self.assertEqual(error_number, errno.EFBIG)
        self.assertIsNone(body)

    def test_busy_retry_bounds(self):
        result, _, body, requests, text, elapsed = self.run_fetch("busy")
        self.assertNotEqual(result, 0, text)
        self.assertIsNone(body)
        self.assertEqual(requests, [None] * 4)
        self.assertIn("重试上限", text)
        self.assertLess(elapsed, 6000)

    def test_busy_then_success(self):
        self.assert_success("busy-then-ok", [None] * 3)

    def test_cancel_busy_wait(self):
        result, error_number, body, requests, text, elapsed = self.run_fetch("busy", cancel_ms=250)
        self.assertNotEqual(result, 0, text)
        self.assertEqual(error_number, errno.ECANCELED)
        self.assertIsNone(body)
        self.assertEqual(requests, [None])
        self.assertLess(elapsed, 1500)

    def test_long_numeric_and_date_wait_outlive_metadata_timeout(self):
        for scenario in ("long-wait", "date-wait"):
            with self.subTest(scenario=scenario):
                result, _, body, requests, text, elapsed = self.run_fetch(scenario, mode="metadata")
                self.assertEqual(result, 0, text)
                self.assertEqual(body, PAYLOAD)
                self.assertEqual(requests, [None, None])
                self.assertIn("WAITED 75000", text)
                self.assertIn("75 秒后自动继续", text)
                self.assertIn("1 秒后自动继续", text)
                self.assertLess(elapsed, 3000)

    def test_success_header_delays_next_download(self):
        result, _, body, requests, text, _ = self.run_fetch("success-cooldown", mode="consecutive")
        self.assertEqual(result, 0, text)
        self.assertEqual(body, PAYLOAD)
        self.assertEqual(requests, [None, None])
        self.assertIn("WAITED 75000", text)

    def test_headerless_transient_backoff(self):
        for scenario in ("transient-then-ok", "headerless-busy"):
            with self.subTest(scenario=scenario):
                text = self.assert_success(scenario, [None, None, None])
                self.assertIn("WAITED 15000", text)

    def test_persistent_write_errno_survives_cleanup(self):
        result, error_number, _, requests, text, _ = self.run_fetch("ok", mode="io-error")
        self.assertNotEqual(result, 0, text)
        self.assertEqual(error_number, errno.ENOSPC)
        self.assertEqual(requests, [])


def main():
    global DRIVER
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build/lifecycle-fixes")
    parser.add_argument("--cc", default=os.environ.get("HOST_CC", "cc"))
    parser.add_argument("--repo-regression", action="store_true",
                        help="also compile/run repository cache tests with a unique temporary state root")
    options = parser.parse_args()
    build = (ROOT / options.build_dir).resolve()
    build.mkdir(parents=True, exist_ok=True)
    DRIVER = build / "host-pkg-http-tests"
    sources = ["tests/test_pkg_http.c", "src/pkg/util.c", "src/pkg/text.c"] + [
        f"third_party/ed25519/{name}.c" for name in ("fe", "ge", "sc", "sha512", "verify")]
    subprocess.run([options.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    "-D_POSIX_C_SOURCE=200809L", "-Isrc", "-Isrc/pkg", "-Ithird_party/ed25519",
                    *sources, "-o", str(DRIVER)], cwd=ROOT, check=True)
    if options.repo_regression:
        # The legacy executable uses a fixed /tmp name; never remove another
        # invocation's directory. Compile the same suite against a unique root.
        with tempfile.TemporaryDirectory(prefix="c1pkg-repo-isolated-") as directory:
            repository_driver = build / "host-pkg-repo-isolated-tests"
            repository_sources = ["tests/test_pkg_repo.c", "src/pkg/util.c", "src/pkg/text.c"] + [
                f"third_party/ed25519/{name}.c"
                for name in ("fe", "ge", "sc", "sha512", "verify", "keypair", "sign")]
            subprocess.run([options.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                            "-D_POSIX_C_SOURCE=200809L", f'-DC1PKG_STATE_ROOT="{directory}/state"',
                            "-Isrc", "-Isrc/pkg", "-Ithird_party/ed25519", *repository_sources,
                            "-o", str(repository_driver)], cwd=ROOT, check=True)
            subprocess.run([str(repository_driver)], cwd=ROOT, check=True, timeout=180)
    unittest.main(argv=[__file__], verbosity=2)


if __name__ == "__main__":
    main()
