#!/usr/bin/env python3
"""Device metrics v2: production C client, synthetic loopback HTTP, no deployment.

Run in WSL Ubuntu-22.04: python3 tests/test_pkg_device_metrics.py
Every test driver is compiled with strict warnings and fail-fast UBSan.
"""
import concurrent.futures
import contextlib
import fcntl
import hashlib
import hmac
import http.server
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import tempfile
import threading
import time
import unittest

PROJECT = Path(__file__).resolve().parents[1]
COUNTS = b"C1PKG-METRICS 2\nI\tapp\t0\nI\tzero\t0\n"
# Identity-only/local-queue fixtures: these addresses are NEVER contacted.
OFFICIAL_BASES = (
    "http://www.fwz233.com/c1/v1", "http://www.fwz233.com/c1/v2",
    "http://123.56.214.77/c1/v1", "http://123.56.214.77/c1/v2",
)
EVENT = re.compile(rb"C1PKG-INSTALL 2\nE\t([0-9a-f]{32})\t([A-Za-z0-9._-]{1,32})\t([A-Za-z0-9._+-]{1,48})\t([0-9a-f]{64})\n")


class Fixture(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        self.requests, self.posts = [], []
        self.events, self.members = {}, set()
        self.get_status, self.post_status = 200, 200
        self.get_retry, self.post_retry = None, None
        self.body = COUNTS
        self.bad_ack = None
        self.drop_ack = self.slow = False
        self.return_count = None
        super().__init__(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server_port}/repo"

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def respond(self, status, body, retry):
        self.send_response(status)
        if retry is not None:
            self.send_header("Retry-After", retry)
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
        self.respond(self.server.get_status, self.server.body, self.server.get_retry)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.requests.append(("POST", self.path))
        self.server.posts.append(body)
        event = EVENT.fullmatch(body)
        if event is None:
            self.respond(400, b"bad request", None)
            return
        token, app, version, key = event.groups()
        if self.server.post_status != 200:
            self.respond(self.server.post_status, b"temporarily unavailable", self.server.post_retry)
            return
        if token in self.server.events and self.server.events[token] != body:
            self.respond(409, b"token conflict", None)
            return
        self.server.events[token] = body
        self.server.members.add((app, key))
        if self.server.drop_ack:
            self.close_connection = True
            return
        count = self.server.return_count
        if count is None:
            count = sum(a == app for a, _ in self.server.members)
        ack = b"C1PKG-INSTALL-ACK 2\nE\t" + token + b"\nI\t" + app + b"\t" + str(count).encode() + b"\n"
        if self.server.bad_ack == "token":
            ack = ack.replace(token, b"f" * 32)
        elif self.server.bad_ack == "app":
            ack = ack.replace(b"I\t" + app, b"I\twrong")
        elif self.server.bad_ack == "count":
            ack = ack[:-2] + b"01\n"
        elif self.server.bad_ack == "extra":
            ack += b"I\textra\t1\n"
        elif self.server.bad_ack == "v1":
            ack = b"C1PKG-INSTALL-ACK 1\nE\t" + token + b"\n"
        self.respond(200, ack, self.server.post_retry)


class DeviceMetrics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="c1pkg-device-metrics-")
        cls.base = Path(cls.tmp.name)
        cls.root = cls.base / "state"
        cls.binary = cls.base / "driver"
        cc = shlex.split(os.environ.get("CC", "cc"))
        command = cc + ["-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                        "-O1", "-g", "-fsanitize=undefined", "-fno-sanitize-recover=all", "-Isrc", "-Isrc/pkg",
                        f'-DC1PKG_STATE_ROOT="{cls.root}"', "tests/test_pkg_device_metrics_driver.c",
                        "src/pkg/util.c", "src/security/sha256.c", "src/security/secure_file.c", "-o", str(cls.binary)]
        compiled = subprocess.run(command, cwd=PROJECT, capture_output=True, text=True)
        if compiled.returncode:
            raise RuntimeError(compiled.stdout + compiled.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def reset_state(self):
        if self.root.exists():
            shutil.rmtree(self.root)
        self.root.mkdir(mode=0o700)

    def setUp(self):
        self.reset_state()
        self.server = Fixture()
        self.addCleanup(self.server.close)
        self.now = 2_000_000_000

    @property
    def state(self):
        return self.root / "metrics"

    def cli(self, *args, input=None, code=0, now=None):
        env = dict(os.environ, C1PKG_METRICS_TEST_TIME=str(self.now if now is None else now),
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        result = subprocess.run([str(self.binary), *args], input=input, capture_output=True, env=env, timeout=35)
        self.assertEqual(result.returncode, code, result.stderr.decode(errors="replace"))
        self.assertNotIn(b"runtime error", result.stderr)
        return result.stdout.decode()

    def record(self, app="app", version="1.0.0", url=None, code=0):
        return self.cli("record", url or self.server.url, app, version, code=code)

    def sync(self, url=None, **kwargs):
        return self.cli("sync", url or self.server.url, **kwargs)

    def events(self):
        return sorted(p for p in self.state.iterdir() if re.fullmatch(r"[a-f0-9]{32}", p.name))

    def fields(self, path):
        repo, body = path.read_bytes().split(b"\n", 1)
        event = EVENT.fullmatch(body)
        self.assertIsNotNone(event)
        return repo, event.groups()

    def test_seed_hmac_stable_across_processes_reinstall_and_versions(self):
        self.record()
        seed = (self.state / "device-seed").read_bytes()
        self.assertEqual(len(seed), 32)
        _, (token, app, version, key) = self.fields(self.events()[0])
        repo = self.server.url.encode()
        message = b"C1PKG-DEVICE-APP-2\0" + struct.pack(">I", len(repo)) + repo + struct.pack(">I", len(app)) + app
        self.assertEqual(key, hmac.new(seed, message, hashlib.sha256).hexdigest().encode())
        self.assertIn("app\t1", self.sync())
        apps = self.root / "apps"
        (apps / "app").mkdir(parents=True)
        shutil.rmtree(apps / "app")  # Ordinary uninstall never touches optional metrics state.
        self.record(version="2.0.0")
        _, (new_token, _, new_version, new_key) = self.fields(self.events()[0])
        self.assertNotEqual(new_token, token)
        self.assertNotEqual(new_version, version)
        self.assertEqual(new_key, key)
        self.assertEqual((self.state / "device-seed").read_bytes(), seed)
        self.assertIn("app\t1", self.sync(now=self.now + 30))
        self.assertEqual(len(self.server.members), 1)
        self.assertEqual(len(self.server.events), 2)
        for p in (self.state / "device-seed", self.state / "device-identity.init"):
            self.assertEqual(p.stat().st_mode & 0o777, 0o600)

    def test_distinct_app_and_repository_scopes(self):
        self.record()
        self.record(app="other")
        self.record(url=self.server.url + "-other")
        keys = {self.fields(p)[1][3] for p in self.events()}
        self.assertEqual(len(keys), 3)
        self.sync()
        self.assertEqual(len(self.events()), 1)
        self.assertIn(b"-other", self.events()[0].read_bytes())

    def test_official_aliases_and_prefixes_share_only_canonical_identity(self):
        for url in OFFICIAL_BASES:
            self.record(url=url)
            self.record(app="other", url=url)
        events = [self.fields(path) for path in self.events()]
        self.assertEqual({repo.decode() for repo, _ in events}, set(OFFICIAL_BASES))
        keys_by_app = {app: {fields[3] for _, fields in events if fields[1] == app}
                       for app in (b"app", b"other")}
        self.assertEqual(len(keys_by_app[b"app"]), 1)
        self.assertEqual(len(keys_by_app[b"other"]), 1)
        self.assertNotEqual(keys_by_app[b"app"], keys_by_app[b"other"])
        seed = (self.state / "device-seed").read_bytes()
        scope = b"http://123.56.214.77/c1"
        for app, keys in keys_by_app.items():
            message = (b"C1PKG-DEVICE-APP-2\0" + struct.pack(">I", len(scope)) + scope
                       + struct.pack(">I", len(app)) + app)
            self.assertEqual(keys, {hmac.new(seed, message, hashlib.sha256).hexdigest().encode()})
        # Exercise the actual worker only against loopback: official-origin
        # records must stay byte-for-byte unchanged, never become local reports.
        pending = {path.name: path.read_bytes() for path in self.events()}
        self.record()
        self.sync()
        self.assertEqual(len(self.server.posts), 1)
        self.assertEqual({path.name: path.read_bytes() for path in self.events()}, pending)
        self.assertEqual(self.server.requests,
                         [("GET", "/repo/metrics.v2"), ("POST", "/repo/install-events.v2")])

    def test_official_scope_mapping_does_not_generalize_custom_https_or_suffixes(self):
        self.record(url=OFFICIAL_BASES[0])
        official_key = self.fields(self.events()[0])[1][3]
        custom_bases = (
            "http://custom.example/c1/v1", "http://custom.example/c1/v2",
            "https://www.fwz233.com/c1/v1", "https://www.fwz233.com/c1/v2",
            "https://123.56.214.77/c1/v1", "https://123.56.214.77/c1/v2",
            "http://www.fwz233.com/c1/v1/extra", "http://123.56.214.77/c1/v2/extra",
            "http://www.fwz233.com:8080/c1/v1", "http://123.56.214.77:8080/c1/v2",
            "http://www.fwz233.com.example/c1/v1", "http://123.56.214.77.example/c1/v2",
            "http://www.fwz233.com/c1/v10", "http://123.56.214.77/C1/v2",
        )
        for url in custom_bases:
            self.record(url=url)
        events = [self.fields(path) for path in self.events()]
        custom_keys = {fields[3] for repo, fields in events if repo.decode() in custom_bases}
        self.assertEqual(len(custom_keys), len(custom_bases))
        self.assertNotIn(official_key, custom_keys)
        self.assertEqual(self.server.requests, [])

    def test_official_identity_aliases_do_not_change_exact_queue_selection(self):
        source = OFFICIAL_BASES[0]
        self.record(url=source)
        pending = {path.name: path.read_bytes() for path in self.events()}
        for selected in OFFICIAL_BASES[1:]:
            self.cli("next", selected, code=2)
        self.assertEqual({path.name: path.read_bytes() for path in self.events()}, pending)
        self.assertIn(self.events()[0].name, self.cli("next", source))
        # Legacy records also remain untouched when another official alias or
        # prefix is selected, despite having the same eventual identity scope.
        self.reset_state()
        self.cli("legacy", "record", source, "app", "1")
        legacy = self.events()[0]
        original = legacy.read_bytes()
        for selected in OFFICIAL_BASES[1:]:
            self.cli("next", selected, code=2)
        self.assertEqual(legacy.read_bytes(), original)
        self.assertIn(legacy.name, self.cli("next", source))
        _, fields = self.fields(legacy)
        self.record(url=OFFICIAL_BASES[3])
        self.assertEqual({self.fields(path)[1][3] for path in self.events()}, {fields[3]})
        self.assertEqual(self.server.requests, [])

    def test_trailing_slash_is_only_normalization(self):
        self.record(url=self.server.url + "/")
        self.record()
        self.assertEqual(len({self.fields(p)[1][3] for p in self.events()}), 1)

    def test_identity_corruption_missing_and_unsafe_fail_closed(self):
        for fault in ("same-length", "short", "missing", "symlink", "hardlink", "mode", "marker", "missing-marker", "both-missing"):
            with self.subTest(fault=fault):
                self.reset_state()
                self.record()
                seed = self.state / "device-seed"
                marker = self.state / "device-identity.init"
                original = seed.read_bytes()
                extra = self.base / "seed-copy"
                if extra.exists():
                    extra.unlink()
                if fault == "same-length":
                    seed.write_bytes(bytes([original[0] ^ 1]) + original[1:])
                elif fault == "short":
                    seed.write_bytes(b"short")
                elif fault == "missing":
                    seed.unlink()
                elif fault in ("symlink", "hardlink"):
                    seed.rename(extra)
                    if fault == "symlink":
                        seed.symlink_to(extra)
                    else:
                        os.link(extra, seed)
                elif fault == "mode":
                    seed.chmod(0o644)
                elif fault == "marker":
                    marker.write_bytes(b"broken")
                elif fault == "missing-marker":
                    marker.unlink()
                elif fault == "both-missing":
                    marker.unlink()
                    seed.unlink()
                self.record(code=2)
                self.assertIn("available=0", self.sync())
                self.assertEqual(self.server.requests, [])
                self.assertEqual(len(self.events()), 1)
                if extra.exists():
                    extra.unlink()

    def test_lost_ack_retries_identical_token_key_without_read_interval(self):
        self.record()
        self.server.drop_ack = True
        self.sync()
        retained = self.events()[0].read_bytes()
        self.assertEqual(len(self.server.members), 1)
        self.sync(now=self.now + 59)
        self.assertEqual(len(self.server.posts), 1)
        self.server.drop_ack = False
        self.assertIn("app\t1", self.sync(now=self.now + 60))
        self.assertEqual(self.events(), [])
        self.assertEqual(self.server.posts[0], self.server.posts[1])
        self.assertIn(self.server.posts[0], retained)
        self.assertEqual(sum(method == "GET" for method, _ in self.server.requests), 1)

    def test_get_cache_does_not_freeze_install_reports_but_minimum_is_durable(self):
        self.assertIn("app\t0", self.sync())
        self.record()
        self.sync(now=self.now + 29)
        self.assertEqual(len(self.server.requests), 1)
        self.assertIn("app\t1", self.sync(now=self.now + 30))
        self.assertEqual(self.server.requests, [("GET", "/repo/metrics.v2"), ("POST", "/repo/install-events.v2")])
        self.sync(now=self.now + 899)
        self.assertEqual(len(self.server.requests), 2)
        self.sync(now=self.now + 900)
        self.assertEqual(len(self.server.requests), 3)

    def test_count_from_ack_is_authoritative_never_plus_one(self):
        self.server.body = b"C1PKG-METRICS 2\nI\tapp\t800\n"
        self.server.return_count = 41
        self.record()
        self.assertIn("app\t41", self.sync())
        self.assertIn("app\t41", self.sync(now=self.now + 1))
        self.record()
        self.assertIn("app\t41", self.sync(now=self.now + 30))
        self.server.return_count = 2**64 - 1
        self.record()
        self.assertIn(f"app\t{2**64 - 1}", self.sync(now=self.now + 60))

    def test_ack_new_app_is_inserted_sorted_and_known_zero_is_preserved(self):
        self.record(app="aaa")
        self.record(app="zzz")
        output = self.sync()
        self.assertLess(output.index("aaa\t1"), output.index("app\t0"))
        self.assertLess(output.index("zero\t0"), output.index("zzz\t1"))

    def test_wrong_ack_token_app_decimal_extra_or_v1_retains_event(self):
        for fault in ("token", "app", "count", "extra", "v1"):
            with self.subTest(fault=fault):
                self.reset_state()
                self.record()
                self.server.bad_ack = fault
                self.sync()
                self.assertEqual(len(self.events()), 1)
                self.server.bad_ack = None
                self.sync(now=self.now + 60)
                self.assertEqual(self.events(), [])

    def test_retry_after_on_post_blocks_get_post_restart_and_repo_switch(self):
        self.record()
        self.server.post_status, self.server.post_retry = 503, "1800"
        self.sync()
        self.record(version="2")
        for offset in (30, 60, 900, 1799):
            self.sync(now=self.now + offset)
            self.sync(url=self.server.url + "-other", now=self.now + offset)
        self.assertEqual(len(self.server.requests), 2)
        self.server.post_status, self.server.post_retry = 200, None
        self.sync(now=self.now + 1800)
        self.assertEqual(self.events(), [])
        self.assertEqual(len(self.server.members), 1)

    def test_retry_after_on_successful_get_blocks_reports(self):
        self.record()
        self.server.get_retry = "1800"
        self.sync()
        self.sync(now=self.now + 900)
        self.assertEqual(len(self.server.requests), 1)
        self.server.get_retry = None
        self.sync(now=self.now + 1800)
        self.assertEqual(self.events(), [])

    def test_unknown_retry_after_and_overflow_pause_all_automatic_network(self):
        for retry in ("Fri, 31 Dec 9999 23:59:59 GMT", str(2**64 - 1), str(2**64), "-1"):
            with self.subTest(retry=retry):
                self.reset_state()
                self.server.requests.clear()
                self.server.get_retry, self.server.get_status = retry, 429
                self.record()
                self.sync()
                self.record()
                self.sync(now=self.now + 100_000)
                self.sync(url=self.server.url + "-new", now=self.now + 100_000)
                self.assertEqual(len(self.server.requests), 1)

    def test_exponential_failure_backoff_survives_restarts(self):
        self.record()
        self.server.post_status = 503
        self.sync()
        self.sync(now=self.now + 59)
        self.assertEqual(len(self.server.posts), 1)
        self.sync(now=self.now + 60)
        self.sync(now=self.now + 179)
        self.assertEqual(len(self.server.posts), 2)
        self.server.post_status = 200
        self.sync(now=self.now + 180)
        self.assertEqual(self.events(), [])

    def test_legacy_gate_imported_once_without_old_count_reinterpretation(self):
        self.cli("legacy-gate", self.server.url, str(self.now + 900))
        old = (self.state / "cache").read_bytes()
        self.record()
        self.assertIn("available=0", self.sync())
        self.assertIn("available=0", self.sync(now=self.now + 899))
        self.assertEqual(self.server.requests, [])
        self.assertIn("app\t1", self.sync(now=self.now + 900))
        self.assertEqual((self.state / "cache").read_bytes(), old)
        self.cli("legacy-gate", self.server.url, str(2**64 - 1))
        self.record()
        self.sync(now=self.now + 930)  # Old cache is not imported a second time.
        self.assertEqual(self.events(), [])

    def test_legacy_maximum_gate_and_other_repo_are_conservatively_imported(self):
        self.cli("legacy-gate", self.server.url + "-old", str(2**64 - 1))
        self.record()
        self.assertIn("available=0", self.sync())
        self.sync(now=self.now + 100_000)
        self.assertEqual(self.server.requests, [])

    def test_pending_legacy_event_exact_repo_adopts_original_token_only(self):
        self.cli("legacy", "record", self.server.url, "app", "1")
        same = self.events()[0]
        self.cli("legacy", "record", self.server.url + "-old", "app", "2")
        other = next(p for p in self.events() if p != same)
        other_data = other.read_bytes()
        self.server.drop_ack = True
        self.sync()
        _, fields = self.fields(same)
        self.assertEqual(fields[0].decode(), same.name)
        self.assertEqual(other.read_bytes(), other_data)
        self.server.drop_ack = False
        self.sync(now=self.now + 60)
        self.assertEqual(self.events(), [other])
        self.assertTrue(all(path.startswith("/repo/") and path.endswith(".v2") for _, path in self.server.requests))

    def test_old_domain_event_never_migrated_or_sent(self):
        self.cli("legacy", "record", "http://old.example.invalid/c1/v1", "app", "1")
        old = self.events()[0].read_bytes()
        self.sync()
        self.assertEqual(self.server.posts, [])
        self.assertEqual(self.events()[0].read_bytes(), old)

    def test_no_installed_directory_history_inference(self):
        (self.root / "apps/app/versions/1").mkdir(parents=True)
        self.sync()
        self.assertEqual(self.server.posts, [])
        self.assertEqual(self.events(), [])

    def test_v1_counts_or_unsupported_v2_never_downgrades(self):
        self.record()
        for i, (status, body) in enumerate(((404, b"old server"), (200, b"C1PKG-METRICS 1\nI\tapp\t900\n"),
                                           (302, b""), (200, b"x" * 20000))):
            self.server.get_status, self.server.body = status, body
            self.assertIn("available=0", self.sync(now=self.now + i * 1000))
        self.assertEqual(len(self.events()), 1)
        self.assertEqual(self.server.posts, [])
        self.assertTrue(all(path.endswith("/metrics.v2") for _, path in self.server.requests))

    def test_cache_corruption_missing_or_link_cannot_reset_gate(self):
        for fault in ("corrupt", "bit-flip", "missing", "symlink", "hardlink", "marker"):
            with self.subTest(fault=fault):
                self.reset_state()
                self.server.requests.clear()
                self.server.get_retry, self.server.get_status = "1800", 429
                self.sync()
                cache = self.state / "device-cache"
                extra = self.base / "cache-copy"
                if extra.exists():
                    extra.unlink()
                if fault == "corrupt":
                    cache.write_bytes(b"broken")
                elif fault == "bit-flip":
                    data = bytearray(cache.read_bytes())
                    # Flip the global deadline's low byte without breaking structure.
                    data[1048] ^= 1
                    cache.write_bytes(data)
                elif fault == "missing":
                    cache.unlink()
                elif fault in ("symlink", "hardlink"):
                    cache.rename(extra)
                    if fault == "symlink":
                        cache.symlink_to(extra)
                    else:
                        os.link(extra, cache)
                else:
                    (self.state / "device-cache.init").unlink()
                self.record()
                self.assertIn("available=0", self.sync(now=self.now + 1801))
                self.assertEqual(len(self.server.requests), 1)
                if extra.exists():
                    extra.unlink()

    def test_corrupt_legacy_cache_is_not_treated_as_zero_gate(self):
        self.cli("legacy-gate", self.server.url, str(2**64 - 1))
        (self.state / "cache").write_bytes(b"broken")
        self.record()
        self.assertIn("available=0", self.sync())
        self.assertEqual(self.server.requests, [])

    def test_queue_bounds_and_four_event_batch(self):
        for _ in range(64):
            self.record()
        self.record(code=2)
        self.assertEqual(len(self.events()), 64)
        self.sync()
        self.assertEqual(len(self.server.posts), 4)
        self.assertEqual(len(self.events()), 60)
        self.sync(now=self.now + 29)
        self.assertEqual(len(self.server.posts), 4)
        self.sync(now=self.now + 30)
        self.assertEqual(len(self.server.posts), 8)
        self.assertEqual(len(self.server.members), 1)

    def test_private_event_links_and_corrupt_key_are_not_sent(self):
        for fault in ("symlink", "hardlink", "key"):
            with self.subTest(fault=fault):
                self.reset_state()
                self.record()
                event = self.events()[0]
                other = self.base / "event-copy"
                if other.exists():
                    other.unlink()
                if fault == "key":
                    data = event.read_bytes()
                    event.write_bytes(data[:-65] + b"f" * 64 + b"\n")
                else:
                    event.rename(other)
                    if fault == "symlink":
                        event.symlink_to(other)
                    else:
                        os.link(other, event)
                self.sync()
                self.assertEqual(self.server.posts, [])
                if other.exists():
                    other.unlink()

    def test_parallel_recorders_and_workers_share_bounds_gate(self):
        def record(_):
            env = dict(os.environ, C1PKG_METRICS_TEST_TIME=str(self.now))
            return subprocess.run([str(self.binary), "record", self.server.url, "app", "1"], env=env).returncode
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(record, range(96)))
        self.assertTrue(set(results) <= {0, 2})
        self.assertEqual(len(self.events()), results.count(0))
        self.assertLessEqual(len(self.events()), 64)
        expected_posts = min(4, len(self.events()))
        self.server.slow = True
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(lambda _: self.sync(), range(8)))
        self.assertEqual(sum(method == "GET" for method, _ in self.server.requests), 1)
        self.assertEqual(len(self.server.posts), expected_posts)

    def test_private_root_symlinks_and_nonblocking_queue_lock(self):
        elsewhere = self.base / "elsewhere"
        elsewhere.mkdir(exist_ok=True)
        (self.root / "metrics").symlink_to(elsewhere, target_is_directory=True)
        self.record(code=2)
        self.assertIn("available=0", self.sync())
        self.assertEqual(list(elsewhere.iterdir()), [])
        (self.root / "metrics").unlink()
        self.record()
        with (self.state / "queue.lock").open("r+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            started = time.monotonic()
            self.record(code=2)
            self.assertLess(time.monotonic() - started, 0.5)
        self.assertEqual(self.server.requests, [])

    def test_conflict_retains_exact_persisted_event_without_downgrade(self):
        self.record()
        queued = self.events()[0].read_bytes()
        self.server.post_status = 409
        self.sync()
        self.assertEqual(self.events()[0].read_bytes(), queued)
        self.sync(now=self.now + 60)
        self.assertEqual(self.server.posts[0], self.server.posts[1])
        self.assertTrue(all(path.endswith(".v2") for _, path in self.server.requests))

    def test_initial_partial_identity_initialization_stays_unavailable(self):
        self.state.mkdir(mode=0o700)
        marker = self.state / "device-identity.init"
        marker.write_bytes(b"partial")
        marker.chmod(0o600)
        self.record(code=2)
        self.assertIn("available=0", self.sync())
        self.assertFalse((self.state / "device-seed").exists())
        self.assertEqual(marker.read_bytes(), b"partial")
        self.assertEqual(self.server.requests, [])

    def test_cancel_reaps_and_preserves_preflight_gate(self):
        self.server.slow = True
        self.assertIn("cancelled and reaped", self.cli("cancel", self.server.url))
        self.sync(now=self.now + 29)
        self.assertEqual(len(self.server.requests), 1)

    def test_parser_strict_v2_known_zero_limits_and_u64(self):
        self.assertIn("zero\t0", self.cli("parse", input=COUNTS))
        self.assertIn("count=0", self.cli("parse", input=b"C1PKG-METRICS 2\n"))
        data = b"C1PKG-METRICS 2\n" + b"".join(f"I\tp{i:03}\t{2**64 - 1}\n".encode() for i in range(128))
        self.assertIn("count=128", self.cli("parse", input=data))
        malformed = [COUNTS.replace(b" 2", b" 1"), COUNTS + b"\0", COUNTS[:-1], COUNTS.replace(b"\n", b"\r\n"),
                     data + b"I\tzzz\t1\n", COUNTS + b"I\tapp\t1\n", b"x" * 8193]
        for count in (b"01", b"-1", b"+1", b"18446744073709551616", b"1 ", b"1\t2"):
            malformed.append(b"C1PKG-METRICS 2\nI\tapp\t" + count + b"\n")
        for data in malformed:
            self.cli("parse", input=data, code=3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
