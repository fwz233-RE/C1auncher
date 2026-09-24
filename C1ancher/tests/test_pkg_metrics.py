#!/usr/bin/env python3
"""Isolated client contract tests; HTTP fixtures are NOT a replacement server.

Uses loopback only, disposable /tmp state, real curl, production client/store,
strict C11 compilation, and an injected clock confined to the C test driver.
Run on Linux/WSL: python3 tests/test_pkg_metrics.py
"""
import concurrent.futures
import contextlib
import fcntl
import http.server
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import threading
import time
import unittest

PROJECT = Path(__file__).resolve().parents[1]
COUNTS = b"C1PKG-METRICS 1\nI\tapp\t23\nI\tzero\t0\n"


class Fixture(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        self.requests = []
        self.events = {}
        self.posts = []
        self.get_status = 200
        self.post_status = 200
        self.body = COUNTS
        self.retry = None
        self.bad_ack = False
        self.drop_ack = False
        self.slow = False
        super().__init__(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server_port}/repo"


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def respond(self, status, body):
        self.send_response(status)
        if self.server.retry is not None:
            self.send_header("Retry-After", self.server.retry)
        if status == 302:
            self.send_header("Location", "/should-not-follow")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        with contextlib.suppress(BrokenPipeError, ConnectionResetError):
            self.wfile.write(body)

    def do_GET(self):
        self.server.requests.append(("GET", self.path))
        if self.server.slow:
            time.sleep(1)
        self.respond(self.server.get_status, self.server.body)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.requests.append(("POST", self.path))
        self.server.posts.append(body)
        match = re.fullmatch(rb"C1PKG-INSTALL 1\nE\t([0-9a-f]{32})\t([A-Za-z0-9._-]{1,32})\t([A-Za-z0-9._+-]{1,48})\n", body)
        if match is None:
            self.respond(400, b"bad fixture request")
            return
        token = match[1]
        self.server.events.setdefault(token, body)
        if self.server.drop_ack:
            self.close_connection = True
            return
        ack = b"C1PKG-INSTALL-ACK 1\nE\t" + (b"f" * 32 if self.server.bad_ack else token) + b"\n"
        self.respond(self.server.post_status, ack)


class Metrics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="c1pkg-metrics-")
        cls.base = Path(cls.tmp.name)
        cls.root = cls.base / "state"
        cls.binary = cls.base / "driver"
        cls.store_binary = cls.base / "store"
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = ["-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                 "-O1", "-fsanitize=undefined", "-fno-sanitize-recover=all", "-Isrc", "-Isrc/pkg"]
        for command in (
            cc + flags + [f'-DC1PKG_STATE_ROOT="{cls.root}"', "tests/test_pkg_metrics_driver.c", "src/pkg/util.c", "src/security/sha256.c", "src/security/secure_file.c", "-o", str(cls.binary)],
            cc + flags + ["tests/test_pkg_metrics_store.c", "src/pkg/util.c", "src/pkg/tui_model.c",
                          "src/pkg/launch_mode.c", "src/platform/app_lease.c", "-o", str(cls.store_binary)],
        ):
            compiled = subprocess.run(command, cwd=PROJECT, capture_output=True, text=True)
            if compiled.returncode:
                raise RuntimeError(compiled.stdout + compiled.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        if self.root.exists():
            shutil.rmtree(self.root)
        self.root.mkdir(mode=0o700)
        self.server = Fixture()
        self.addCleanup(self.server.close)
        self.now = 2_000_000_000

    def cli(self, *args, input=None, code=0, now=None):
        env = dict(os.environ, C1PKG_METRICS_TEST_TIME=str(self.now if now is None else now))
        result = subprocess.run([str(self.binary), *args], input=input, capture_output=True, env=env, timeout=35)
        self.assertEqual(result.returncode, code, result.stderr.decode(errors="replace"))
        return result.stdout.decode()

    def record(self, app="app", version="1.0.0", url=None, code=0):
        return self.cli("record", url or self.server.url, app, version, code=code)

    def sync(self, **kwargs):
        return self.cli("sync", self.server.url, **kwargs)

    def events(self):
        return sorted(p for p in (self.root / "metrics").iterdir() if re.fullmatch(r"[a-f0-9]{32}", p.name))

    def test_parser_known_zero_missing_and_u64(self):
        output = self.cli("parse", input=COUNTS)
        self.assertIn("available=1 count=2", output)
        self.assertIn("zero\t0", output)
        self.assertIn(str(2**64 - 1), self.cli("parse", input=b"C1PKG-METRICS 1\nI\tapp\t18446744073709551615\n"))
        self.assertIn("count=0", self.cli("parse", input=b"C1PKG-METRICS 1\n"))

    def test_parser_strict_limits_and_invalid_bytes(self):
        malformed = [b"", b"<html>old server</html>", COUNTS[:-1], COUNTS + b"\0", COUNTS.replace(b"\n", b"\r\n"),
                     b"C1PKG-METRICS 2\n", b"C1PKG-METRICS 1\n\n"]
        for count in (b"-1", b"+1", b"01", b"1 ", b"18446744073709551616", b"nan", b"", b"1\t2"):
            malformed.append(b"C1PKG-METRICS 1\nI\tapp\t" + count + b"\n")
        for app in (b"../app", b"a" * 33, b"", b"bad id"):
            malformed.append(b"C1PKG-METRICS 1\nI\t" + app + b"\t1\n")
        malformed += [COUNTS + b"I\tapp\t1\n", b"C1PKG-METRICS 1\n" + b"".join(f"I\tp{i:03}\t1\n".encode() for i in range(129)), b"x" * 8193]
        for data in malformed:
            with self.subTest(data=data[:80]):
                self.cli("parse", input=data, code=3)

    def test_success_event_fields_and_statistics_are_separate(self):
        (self.root / "highest-sequence").write_bytes(b"signed-state-sentinel")
        self.record()
        event = self.events()[0]
        data = event.read_text()
        self.assertRegex(event.name, "^[a-f0-9]{32}$")
        self.assertEqual(event.stat().st_mode & 0o777, 0o600)
        self.assertEqual(data, f"{self.server.url}\nC1PKG-INSTALL 1\nE\t{event.name}\tapp\t1.0.0\n")
        self.assertIn("app\t23", self.sync())
        self.assertEqual(self.events(), [])
        self.assertEqual(len(self.server.events), 1)
        self.assertEqual((self.root / "highest-sequence").read_bytes(), b"signed-state-sentinel")

    def test_invalid_record_and_endpoint_never_network(self):
        for app, version in (("../a", "1"), ("app", "1/2"), ("a" * 33, "1"), ("a", "1" * 49)):
            self.record(app, version, code=2)
        for url in ("file:///tmp/repo", "https://user:password@example.com/repo", "https://example.com/repo?q=1",
                    "https://example.com/a/../b", "https://example.com/a#b", "https://example.com/%61", "https://example.com/a\nb"):
            self.record(url=url, code=2)
        self.assertEqual(self.server.requests, [])

    def test_queue_is_bounded_and_reinstall_has_fresh_token(self):
        for _ in range(64):
            self.record()
        before = self.events()
        self.assertEqual(len(before), 64)
        self.record(code=2)
        self.assertEqual(self.events(), before)
        self.assertLess(sum(p.stat().st_size for p in before), 64 * 1280)

    def test_unknown_old_server_does_not_mean_zero(self):
        self.record()
        self.server.get_status = 404
        self.assertIn("available=0 count=0", self.sync())
        self.assertEqual(len(self.events()), 1)
        self.assertEqual(self.server.posts, [])

    def test_lost_ack_reuses_id_and_server_fixture_deduplicates(self):
        self.record()
        token = self.events()[0].name
        self.server.drop_ack = True
        self.sync()
        self.assertEqual(len(self.events()), 1)
        self.server.drop_ack = False
        self.sync(now=self.now + 901)
        self.assertEqual(self.events(), [])
        self.assertEqual(len(self.server.events), 1)
        self.assertEqual(len(self.server.posts), 2)
        self.assertEqual(self.server.posts[0], self.server.posts[1])
        self.assertIn(token.encode(), self.server.posts[1])

    def test_bad_ack_or_failed_post_keeps_event(self):
        self.record()
        self.server.bad_ack = True
        self.sync()
        self.assertEqual(len(self.events()), 1)
        self.server.bad_ack = False
        self.server.post_status = 503
        self.sync(now=self.now + 901)
        self.assertEqual(len(self.events()), 1)
        self.server.post_status = 200
        self.sync(now=self.now + 1802)
        self.assertEqual(self.events(), [])

    def test_origin_isolation_and_trailing_slash_normalization(self):
        self.record(url=self.server.url + "/")
        self.record(url=self.server.url + "-other")
        self.sync()
        self.assertEqual(len(self.server.posts), 1)
        self.assertEqual(len(self.events()), 1)
        self.assertIn("-other\n", self.events()[0].read_text())
        self.cli("sync", self.server.url + "-other", now=self.now + 901)
        self.assertEqual(len(self.server.posts), 2)
        self.assertEqual(self.events(), [])

    def test_restart_cooldown_and_cached_snapshot(self):
        self.assertIn("app\t23", self.sync())
        requests = len(self.server.requests)
        self.assertIn("app\t23", self.sync(now=self.now + 899))
        self.assertEqual(len(self.server.requests), requests)
        self.sync(now=self.now + 900)
        self.assertEqual(len(self.server.requests), requests + 1)

    def test_numeric_retry_after_and_no_alias_bypass(self):
        self.record()
        self.server.get_status = 429
        self.server.retry = "1800"
        self.sync()
        self.sync(now=self.now + 1799)
        self.assertEqual(len(self.server.requests), 1)
        self.assertEqual(len(self.events()), 1)
        self.server.get_status = 200
        self.server.retry = None
        self.sync(now=self.now + 1800)
        self.assertEqual(self.events(), [])

    def test_unrecognized_retry_after_pauses_automatic_network(self):
        self.server.retry = "Fri, 31 Dec 9999 23:59:59 GMT"
        self.server.get_status = 503
        self.sync()
        self.sync(now=self.now + 100_000)
        self.assertEqual(len(self.server.requests), 1)

    def test_batch_is_bounded(self):
        for _ in range(7):
            self.record()
        self.sync()
        self.assertEqual(len(self.server.posts), 4)
        self.assertEqual(len(self.events()), 3)

    def test_redirect_and_oversized_or_invalid_counts_do_not_post(self):
        self.record()
        for step, (status, body) in enumerate(((302, b""), (200, b"x" * 20000), (200, b"html"))):
            self.server.get_status, self.server.body = status, body
            self.assertIn("available=0", self.sync(now=self.now + step * 901))
        self.assertEqual(self.server.posts, [])
        self.assertEqual(len(self.server.requests), 3)
        self.assertTrue(all(path.endswith("metrics.v1") for _, path in self.server.requests))

    def test_untrusted_files_and_short_lock_contention(self):
        self.record()
        root = self.root / "metrics"
        with (root / "queue.lock").open("r+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            start = time.monotonic()
            self.record(code=2)
            self.assertLess(time.monotonic() - start, 0.5)
        event = self.events()[0]
        saved = self.base / "linked-data"
        saved.write_bytes(event.read_bytes())
        event.unlink()
        event.symlink_to(saved)
        self.sync()
        self.assertEqual(self.server.posts, [])
        event.unlink()
        os.link(saved, event)
        self.sync(now=self.now + 901)
        self.assertEqual(self.server.posts, [])
        saved.unlink()

    def test_private_root_symlink_rejected(self):
        elsewhere = self.base / "elsewhere"
        elsewhere.mkdir(exist_ok=True)
        (self.root / "metrics").symlink_to(elsewhere, target_is_directory=True)
        self.record(code=2)
        self.assertEqual(list(elsewhere.iterdir()), [])

    def test_parallel_recorders_respect_bound(self):
        env = dict(os.environ, C1PKG_METRICS_TEST_TIME=str(self.now))
        def record(_):
            return subprocess.run([str(self.binary), "record", self.server.url, "app", "1"], env=env).returncode
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(record, range(96)))
        self.assertTrue(set(results) <= {0, 2})
        self.assertLessEqual(len(self.events()), 64)
        self.assertEqual(len(self.events()), results.count(0))

    def test_parallel_workers_share_persistent_gate(self):
        self.server.slow = True
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda _: self.sync(), range(8)))
        self.assertTrue(all("available=" in output for output in results))
        self.assertEqual(self.server.requests, [("GET", "/repo/metrics.v1")])

    def test_worker_releases_inherited_application_lock(self):
        self.server.slow = True
        self.assertIn("available=1", self.cli("inherit", self.server.url))

    def test_corrupt_cache_and_event_fail_without_affecting_install_state(self):
        self.record()
        event = self.events()[0]
        event.write_bytes(b"corrupted event")
        cache = self.root / "metrics/cache"
        cache.write_bytes(b"malformed cache")
        cache.chmod(0o600)
        self.assertIn("available=1", self.sync())
        self.assertEqual(self.server.posts, [])
        self.assertEqual(len(self.events()), 1)

    def test_body_boundaries_and_maximum_sorted_records(self):
        data = b"C1PKG-METRICS 1\n" + b"".join(f"I\tp{i:03}\t{2**64 - 1}\n".encode() for i in range(128))
        self.assertIn("available=1 count=128", self.cli("parse", input=data))
        self.assertEqual(self.server.requests, [])

    def test_cancel_slow_request_reaps_worker(self):
        self.server.slow = True
        start = time.monotonic()
        self.assertIn("cancelled and reaped", self.cli("cancel", self.server.url))
        self.assertLess(time.monotonic() - start, 1)

    def test_real_store_success_only_and_metrics_failure_is_nonfatal(self):
        result = subprocess.run([str(self.store_binary)], cwd=PROJECT, capture_output=True, timeout=45)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn(b"Installation metrics hook tests passed", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
