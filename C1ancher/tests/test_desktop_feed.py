#!/usr/bin/env python3
"""Isolated desktop feed + real-font tests (Linux host / WSL, loopback only).

  python3 tests/test_desktop_feed.py
  python3 tests/test_desktop_feed.py --sanitize
  python3 tests/test_desktop_feed.py --build-dir build/desktop-feed-tests --cc cc

Builds test_desktop_data.c against production desktop_data/preferences/text and
builds desktop_feed_driver.c against production desktop.c/repo.c/util.c. No
Makefile changes, device paths, deployment config, signing keys or remote URLs.
All runtime caches are compile-time redirected to relative fixture paths. The
curl subprocess alone gets a short test deadline; retries/cooldowns use the
existing local test clock. Each test gets a different TemporaryDirectory.
"""
import argparse
import http.server
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
DATA_DRIVER = None
FEED_DRIVER = None
CURL_HELPER = None


def feed(sequence=41, ids=("alpha", "beta"), date="2026-09-20", zh="学而时习之，不亦说乎？", en="Lost time is never found again."):
    # The default is a legacy translated pair; current same-original cases
    # below verify the revised one-quote server response explicitly.
    text = f"C1DESKTOP 1\nD\t{date}\nS\t{sequence}\nQ\tzh\t{zh}\nQ\ten\t{en}\n"
    return (text + "".join(f"P\t{value}\n" for value in ids)).encode("utf-8")


class Fixture(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.body = feed()
        self.scenario = "ok"
        self.requests = []
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    @property
    def base(self):
        return f"http://127.0.0.1:{self.server_port}/c1/v2"

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *unused):
        pass

    def do_GET(self):
        self.server.requests.append((self.path, self.headers.get("Range"), self.headers.get("If-None-Match")))
        try:
            if not self.path.endswith("/desktop.v1"):
                self.reply(500, b"only desktop.v1 is available")
                return
            scenario = self.server.scenario
            if scenario == "404":
                self.reply(404, b"not found")
            elif scenario == "timeout":
                time.sleep(1.5)
                self.reply(200, self.server.body)
            elif scenario == "cut":
                self.reply(200, self.server.body[:24], advertised=len(self.server.body))
                self.connection.shutdown(socket.SHUT_RDWR)
            elif scenario == "oversize":
                self.reply(200, b"x" * 17000, no_length=True)
            elif scenario == "304":
                self.reply(304, b"")
            else:
                self.reply(200, self.server.body)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def reply(self, status, body, advertised=None, no_length=False):
        self.send_response(status)
        self.send_header("Connection", "close")
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Cache-Control", "public, max-age=0, must-revalidate")
        self.send_header("ETag", '"fixture"')
        if not no_length:
            self.send_header("Content-Length", str(len(body) if advertised is None else advertised))
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        self.close_connection = True


class DesktopDataTests(unittest.TestCase):
    def run_group(self, group):
        completed = subprocess.run([str(DATA_DRIVER), group], capture_output=True, text=True, timeout=15)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)

    def test_real_font_and_utf8(self):
        self.run_group("width")

    def test_strict_wire_protocol(self):
        self.run_group("protocol")

    def test_private_cache(self):
        self.run_group("cache")

    def test_seen_set_algorithm(self):
        self.run_group("seen")


class DesktopFeedTests(unittest.TestCase):
    def setUp(self):
        self.runtime = tempfile.TemporaryDirectory(prefix="c1-desktop-feed-state-")
        self.directory = Path(self.runtime.name)
        self.server = Fixture()
        self.addCleanup(self.runtime.cleanup)
        self.addCleanup(self.server.close)

    def run_driver(self, *args):
        environment = dict(os.environ, NO_PROXY="*", no_proxy="*", C1_TEST_DESKTOP_CURL=str(CURL_HELPER))
        start = time.monotonic()
        completed = subprocess.run([str(FEED_DRIVER), *args], cwd=self.directory, env=environment,
                                   capture_output=True, text=True, timeout=15)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        report = {}
        for line in completed.stdout.splitlines():
            key, _, value = line.partition(" ")
            if key in ("P", "SEEN_P"):
                report.setdefault(key, []).append(value)
            else:
                report[key] = value
        self.assertIn("RESULT", report, completed.stdout)
        report["elapsed"] = time.monotonic() - start
        # No package indexes or signature material may be created by this call.
        self.assertFalse(list(self.directory.rglob("index.v1*")))
        self.assertFalse(list(self.directory.rglob("*.headers.*")))
        return report

    def summary(self, base=None):
        return self.run_driver("summary", base or self.server.base)

    def seen(self, sequence=41, ids=("alpha", "beta"), base=None):
        return self.run_driver("seen", base or self.server.base, str(sequence), *ids)

    def cache_bytes(self):
        return (self.directory / "desktop-summary.cache").read_bytes()

    def assert_desktop_requests_only(self):
        self.assertTrue(self.server.requests)
        self.assertTrue(all(path == "/c1/v2/desktop.v1" for path, _, _ in self.server.requests), self.server.requests)

    def test_success_is_one_small_request_without_index_or_seen_write(self):
        report = self.summary()
        self.assertEqual(report["RESULT"], "0", report)
        self.assertEqual(report["CACHE"], "1")
        self.assertEqual(report["SEEN"], "0")
        self.assertEqual(report["NEW"], "0", "first contact is not a new-app alert")
        self.assertEqual(report.get("SOURCE"), self.server.base)
        self.assertEqual(report["P"], ["alpha", "beta"])
        self.assertEqual(report["ZH"], "学而时习之，不亦说乎？")
        self.assertEqual(self.server.requests, [("/c1/v2/desktop.v1", None, None)])
        self.assertEqual((self.directory / "desktop-summary.cache").stat().st_mode & 0o777, 0o600)
        self.assertFalse((self.directory / "desktop-seen.cache").exists())

    def test_same_chinese_original_is_cached_in_both_compatibility_slots(self):
        self.server.body = feed(zh="中文原句", en="中文原句")
        report = self.summary()
        self.assertEqual(report["RESULT"], "0", report)
        self.assertEqual(report["ZH"], "中文原句")
        self.assertEqual(report["EN"], "中文原句")

    def test_same_english_original_is_cached_in_both_compatibility_slots(self):
        self.server.body = feed(zh="English original.", en="English original.")
        report = self.summary()
        self.assertEqual(report["RESULT"], "0", report)
        self.assertEqual(report["ZH"], "English original.")
        self.assertEqual(report["EN"], "English original.")

    def test_summary_hides_internal_service_from_home_catalog(self):
        self.server.body = feed(ids=("alpha", "beta", "c1-ime"))
        report = self.summary()
        self.assertEqual(report["P"], ["alpha", "beta"], report)
        self.assertEqual(report["COUNT"], "2")

    def test_empty_catalog_succeeds(self):
        self.server.body = feed(ids=())
        report = self.summary()
        self.assertEqual(report["RESULT"], "0")
        self.assertEqual(report.get("COUNT"), "0")
        self.assertNotIn("P", report)

    def test_v1_and_trailing_slashes_normalize_to_v2(self):
        report = self.summary(self.server.base[:-1] + "1///")
        self.assertEqual(report["RESULT"], "0", report)
        self.assertEqual(report.get("SOURCE"), self.server.base)
        self.assertEqual(self.server.requests, [("/c1/v2/desktop.v1", None, None)])
        self.seen(base=self.server.base[:-1] + "1/")
        self.server.body = feed(42, ("alpha", "beta", "gamma"))
        report = self.summary(self.server.base + "/")
        self.assertEqual(report["NEW"], "1", report)

    def test_seen_write_is_offline_and_sorts_index(self):
        report = self.seen(ids=("gamma", "alpha", "beta"))
        self.assertEqual(report["SEEN"], "1", report)
        self.assertEqual(report["SEEN_P"], ["alpha", "beta", "gamma"])
        self.assertEqual(self.server.requests, [])
        self.assertFalse((self.directory / "desktop-summary.cache").exists())
        self.assertEqual((self.directory / "desktop-seen.cache").stat().st_mode & 0o777, 0o600)

    def test_seen_hides_internal_service_from_home_baseline(self):
        report = self.seen(ids=("alpha", "c1-ime", "beta"))
        self.assertEqual(report["SEEN_P"], ["alpha", "beta"], report)

    def test_new_application_does_not_advance_seen_until_viewed(self):
        self.seen()
        before = (self.directory / "desktop-seen.cache").read_bytes()
        self.server.body = feed(42, ("alpha", "beta", "gamma"))
        report = self.summary()
        self.assertEqual(report["NEW"], "1", report)
        self.assertEqual((self.directory / "desktop-seen.cache").read_bytes(), before)
        report = self.seen(42, ("alpha", "beta", "gamma"))
        self.assertEqual(report["NEW"], "0", report)

    def test_delete_version_only_and_same_count_replacement(self):
        self.seen()
        for sequence, ids, expected in [(42, ("alpha", "beta"), "0"), (43, ("alpha",), "0"),
                                        (44, ("alpha", "gamma"), "1")]:
            with self.subTest(sequence=sequence, ids=ids):
                self.server.body = feed(sequence, ids)
                report = self.summary()
                self.assertEqual(report["RESULT"], "0", report)
                self.assertEqual(report["NEW"], expected, report)

    def test_viewed_empty_catalog_then_first_app_is_new(self):
        self.seen(41, ())
        self.server.body = feed(42, ("alpha",))
        self.assertEqual(self.summary()["NEW"], "1")

    def test_source_change_suppresses_old_baseline_badge(self):
        self.seen()
        self.summary()
        other = Fixture()
        try:
            other.body = feed(1, ("delta",))
            report = self.summary(other.base)
            self.assertEqual(report["RESULT"], "0", report)
            self.assertEqual(report.get("SOURCE"), other.base)
            self.assertEqual(report["NEW"], "0")
            report = self.seen(1, ("delta",), other.base)
            self.assertEqual(report["SEEN_SOURCE"], other.base)
            self.assertEqual(report.get("SEEN_SEQUENCE"), "1")
        finally:
            other.close()

    def test_sequence_rollback_preserves_summary_and_seen(self):
        self.seen(50)
        self.server.body = feed(51, ("alpha", "beta", "gamma"))
        self.summary()
        cache, seen = self.cache_bytes(), (self.directory / "desktop-seen.cache").read_bytes()
        self.server.body = feed(49, ("alpha",))
        report = self.summary()
        self.assertNotEqual(report["RESULT"], "0", report)
        self.assertEqual(self.cache_bytes(), cache)
        self.assertEqual(report["NEW"], "1")
        self.seen(48, ("alpha",))
        self.assertEqual((self.directory / "desktop-seen.cache").read_bytes(), seen)

    def test_rollback_below_seen_when_summary_cache_missing(self):
        self.seen(50)
        self.server.body = feed(49, ("alpha", "gamma"))
        report = self.summary()
        self.assertEqual(report["NEW"], "0", report)
        self.assertEqual(report.get("SEEN_SEQUENCE"), "50")

    def assert_failure_preserves_cache(self, scenario):
        self.assertEqual(self.summary()["RESULT"], "0")
        self.seen()
        cache, seen = self.cache_bytes(), (self.directory / "desktop-seen.cache").read_bytes()
        self.server.requests.clear()
        self.server.scenario = scenario
        report = self.summary()
        self.assertNotEqual(report["RESULT"], "0", report)
        self.assertEqual(self.cache_bytes(), cache)
        self.assertEqual((self.directory / "desktop-seen.cache").read_bytes(), seen)
        self.assert_desktop_requests_only()
        return report

    def test_old_server_404_keeps_cache_without_index_fallback(self):
        self.assert_failure_preserves_cache("404")
        self.assertEqual(len(self.server.requests), 1)

    def test_timeout_keeps_cache_and_reaps_download_helpers(self):
        report = self.assert_failure_preserves_cache("timeout")
        self.assertLess(report["elapsed"], 10)
        self.assertLessEqual(len(self.server.requests), 5)

    def test_transport_truncation_keeps_cache(self):
        self.assert_failure_preserves_cache("cut")

    def test_streamed_body_limit_keeps_cache(self):
        self.assert_failure_preserves_cache("oversize")

    def test_unrequested_304_does_not_replace_cached_body(self):
        self.assert_failure_preserves_cache("304")

    def test_malformed_protocol_keeps_cache(self):
        self.summary()
        cache = self.cache_bytes()
        for body in (feed()[:-1], feed()+b"\x00", feed()+b"P\talpha\n", feed(ids=("beta", "alpha")),
                     feed(0), feed(date="2026-02-30"), feed(en="中文"),
                     feed(zh="中文原句", en="另一条中文原句"),
                     feed(zh="学" * 18, en="学" * 18),
                     feed(zh="a\x01", en="a\x01"), feed(zh="a\u00adb")):
            with self.subTest(body=body):
                self.server.body = body
                report = self.summary()
                self.assertNotEqual(report["RESULT"], "0", report)
                self.assertEqual(self.cache_bytes(), cache)

    def test_maximum_ids_and_uppercase(self):
        self.server.body = feed(18446744073709551615, tuple(f"app-{i:028d}" for i in range(128)))
        report = self.summary()
        self.assertEqual(report["RESULT"], "0", report)
        self.assertEqual(report.get("COUNT"), "128")
        self.server.body = feed(18446744073709551615, ("0-app", "App", "Z.app", "a-app"))
        report = self.summary()
        self.assertEqual(report["RESULT"], "0", report)
        self.assertEqual(report["P"], ["0-app", "App", "Z.app", "a-app"])

    def test_oversized_catalog_and_bad_ids_do_not_replace_cache(self):
        self.summary()
        original = self.cache_bytes()
        for ids in (tuple(f"app-{i:03d}" for i in range(129)), ("a..b",), ("_app",), ("app.",),
                    ("c1pkg",), ("App", "app")):
            with self.subTest(ids=ids):
                self.server.body = feed(42, ids)
                report = self.summary()
                self.assertNotEqual(report["RESULT"], "0", report)
                self.assertEqual(self.cache_bytes(), original)

    def test_invalid_repository_input_never_reaches_http(self):
        for base in ("", "ftp://127.0.0.1/c1/v2", self.server.base+"?x=1", self.server.base+"#x",
                     self.server.base+"/%2e", self.server.base+"/a b", self.server.base+"\\x",
                     "http://user@127.0.0.1/c1/v2", self.server.base+"\n", self.server.base+"é"):
            with self.subTest(base=base):
                report = self.run_driver("summary", base)
                self.assertNotEqual(report["RESULT"], "0", report)
                self.assertEqual(report["CACHE"], "0")
        self.assertEqual(self.server.requests, [])


def main():
    global DATA_DRIVER, FEED_DRIVER, CURL_HELPER
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", help="optional persistent output directory; default uses a removed temporary directory")
    parser.add_argument("--cc", default=os.environ.get("HOST_CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    options = parser.parse_args()
    if os.name != "posix" or not shutil.which("curl"):
        parser.error("run on Linux/WSL with cc, curl and Python 3")
    with tempfile.TemporaryDirectory(prefix="c1-desktop-feed-build-") as temporary:
        build = (ROOT / options.build_dir).resolve() if options.build_dir else Path(temporary)
        build.mkdir(parents=True, exist_ok=True)
        DATA_DRIVER, FEED_DRIVER = build / "host-desktop-data-tests", build / "host-desktop-feed-driver"
        CURL_HELPER = Path(temporary) / "curl-deadline"
        CURL_HELPER.write_text("#!/usr/bin/python3\nimport os,sys,urllib.parse\na=sys.argv[1:]\n"
                               "urls=[v for v in a if v.startswith(('http://','https://'))]\n"
                               "if not urls or any(urllib.parse.urlsplit(v).hostname!='127.0.0.1' for v in urls): sys.exit(97)\n"
                               "for i,v in enumerate(a[:-1]):\n"
                               " if v in ('--max-time','--connect-timeout'): a[i+1]='0.35'\n"
                               f"os.execv({shutil.which('curl')!r},[{shutil.which('curl')!r}]+a)\n", encoding="utf-8")
        CURL_HELPER.chmod(0o700)
        flags = [options.cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Isrc", "-Isrc/pkg", "-Ithird_party/ed25519"]
        flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if options.sanitize else ["-O2"]
        common = ["src/services/desktop_data.c", "src/ui/preferences.c", "src/pkg/text.c"]
        subprocess.run(flags + ["tests/test_desktop_data.c", *common, "-o", str(DATA_DRIVER)], cwd=ROOT, check=True)
        crypto = [f"third_party/ed25519/{name}.c" for name in ("fe", "ge", "sc", "sha512", "verify")]
        isolated = ['-DC1_DESKTOP_CACHE="desktop-summary.cache"', '-DC1_DESKTOP_SEEN="desktop-seen.cache"', '-DC1PKG_STATE_ROOT="pkg-state"']
        subprocess.run(flags + isolated + ["tests/desktop_feed_driver.c", "src/pkg/desktop.c", "src/pkg/desktop_fetch.c", "src/pkg/util.c", *common, *crypto,
                                           "-Wl,--wrap=c1pkg_helper", "-o", str(FEED_DRIVER)], cwd=ROOT, check=True)
        suite = unittest.defaultTestLoader.loadTestsFromModule(__import__(__name__))
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
