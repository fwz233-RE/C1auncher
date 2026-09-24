#!/usr/bin/env python3
"""Durable repository cooldown tests, loopback only and isolated /tmp state.

Run: python3 tests/test_pkg_cooldown.py
The driver injects a monotonic clock only for tests. Each invocation is a new
process, so no test relies on repo.c's process-global deadline surviving.
"""
import errno
import http.server
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.requests.append(self.path)
        body = b"test package bytes"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Retry-After", "75" if len(self.server.requests) == 1 else "0")
        self.end_headers()
        self.wfile.write(body)


class Cooldown(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="c1pkg-cooldown-")
        cls.base = Path(cls.temporary.name)
        cls.state = cls.base / "state"
        cls.driver = cls.base / "driver"
        sources = ["tests/test_pkg_cooldown.c", "src/pkg/util.c", "src/pkg/text.c"] + [
            f"third_party/ed25519/{name}.c" for name in ("fe", "ge", "sc", "sha512", "verify")]
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O1", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-D_POSIX_C_SOURCE=200809L",
            f'-DC1PKG_STATE_ROOT="{cls.state}"', "-Isrc", "-Isrc/pkg", "-Ithird_party/ed25519",
            *sources, "-o", str(cls.driver)]
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        if self.state.exists():
            shutil.rmtree(self.state)
        self.repo = "http://127.0.0.1:19001/repo"

    def invoke(self, mode="get", scope=None, url=None, now=100000, seconds=None, expected=0):
        args = [str(self.driver), mode, scope or self.repo, url or self.repo + "/packages/app.tar.gz", str(now)]
        if seconds is not None:
            args.append(str(seconds))
        result = subprocess.run(args, capture_output=True, text=True, timeout=12)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        if expected:
            return None
        match = re.search(r"RESULT (-?\d+) ERRNO (\d+) WAITED (\d+) UNTIL (\d+)", result.stdout)
        self.assertIsNotNone(match, result.stdout + result.stderr)
        return tuple(map(int, match.groups()))

    def records(self):
        return sorted((self.state / "cooldown").glob("*.state"))

    def test_configuration_binding_has_no_io_or_network(self):
        self.assertEqual(self.invoke("bind")[0], 0)
        self.assertFalse(self.state.exists())
        for repo in ("http://user:secret@example.invalid/repo", "http://example.invalid/repo?x=1",
                     "http://example.invalid/a/../b", "http://example.invalid/%72epo"):
            self.invoke("bind", scope=repo, expected=2)
        self.assertFalse(self.state.exists())

    def test_waiting_same_scope_reloads_completed_response(self):
        holder = subprocess.Popen([str(self.driver), "hold", self.repo, self.repo + "/index.v1", "real"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        follower = None
        try:
            self.assertEqual(holder.stdout.readline().strip(), "LOCKED")
            follower = subprocess.Popen([str(self.driver), "cancel", self.repo, self.repo + "/packages/app.tar.gz", "real"],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            holder.communicate("release\n", timeout=5)
            output, diagnostic = follower.communicate(timeout=5)
            self.assertEqual(follower.returncode, 0, diagnostic)
            self.assertIn(f"RESULT -1 ERRNO {errno.ECANCELED}", output)
        finally:
            for process in (holder, follower):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.communicate()

    def test_untrusted_cooldown_directory_cannot_redirect_writes(self):
        self.state.mkdir(mode=0o700)
        target = self.base / "elsewhere"
        target.mkdir(exist_ok=True)
        (self.state / "cooldown").symlink_to(target, target_is_directory=True)
        self.assertNotEqual(self.invoke("set", seconds=75)[0], 0)
        self.assertEqual(list(target.iterdir()), [])

    def test_child_to_parent_without_memory_deadline(self):
        result, _, waited, _ = self.invoke("fork")
        self.assertEqual(result, 0)
        self.assertEqual(waited, 75000)

    def test_new_process_honors_remaining_not_full_interval(self):
        self.assertEqual(self.invoke("set", seconds=75)[0], 0)
        self.assertEqual(self.invoke(now=120000)[2], 55000)
        self.assertEqual(self.invoke(now=175000)[2], 0)

    def test_refresh_style_metadata_to_parent_nested_package(self):
        self.invoke("set", url=self.repo + "/index.v1", seconds=75)
        self.assertEqual(self.invoke(scope="-", url=self.repo + "/packages/subdir/app.tar.gz")[2], 75000)

    def test_different_repositories_on_same_origin_are_isolated(self):
        self.invoke("set", seconds=75)
        other = "http://127.0.0.1:19001/other"
        self.assertEqual(self.invoke(scope=other, url=other + "/index.v1")[2], 0)
        self.assertEqual(self.invoke()[2], 75000)
        self.assertEqual(len(self.records()), 2)

    def test_nested_repository_explicit_binding_wins(self):
        self.invoke("set", seconds=75)
        nested = self.repo + "/nested"
        self.invoke("set", scope=nested, url=nested + "/index.v1", seconds=10)
        self.assertEqual(self.invoke(scope=nested, url=nested + "/app")[2], 10000)
        self.assertEqual(self.invoke(scope=self.repo, url=nested + "/app")[2], 75000)

    def test_official_domain_default_port_and_ip_share_deadline(self):
        primary = "http://www.fwz233.com:80/c1/v2/"
        alias = "http://123.56.214.77/c1/v2"
        self.invoke("set", scope=primary, url=primary + "index.v1", seconds=75)
        self.assertEqual(self.invoke(scope=alias, url=alias + "/app.tar.gz")[2], 75000)
        self.assertEqual(len(self.records()), 1)

    def test_normalization_preserves_path_case_and_scheme(self):
        self.invoke("set", scope="HTTP://EXAMPLE.INVALID:80/Repo/", url="http://example.invalid/Repo/index.v1", seconds=75)
        self.assertEqual(self.invoke(scope="http://example.invalid/Repo", url="http://example.invalid/Repo/app")[2], 75000)
        for repo in ("https://example.invalid/Repo", "http://example.invalid/repo"):
            self.assertEqual(self.invoke(scope=repo, url=repo + "/app")[2], 0)

    def test_cancelled_wait_does_not_clear_deadline(self):
        self.invoke("set", seconds=75)
        result, error, waited, _ = self.invoke("cancel")
        self.assertNotEqual(result, 0)
        self.assertEqual(error, errno.ECANCELED)
        self.assertEqual(waited, 0)
        self.assertEqual(self.invoke(now=110000)[2], 65000)

    def test_overflow_is_persistent_and_never_retried_early(self):
        self.invoke("set", seconds=2**64 - 1)
        for now in (100000, 200000, 999999999):
            result, error, waited, deadline = self.invoke(now=now)
            self.assertNotEqual(result, 0)
            self.assertEqual(error, errno.EAGAIN)
            self.assertEqual(waited, 0)
            self.assertEqual(deadline, 2**64 - 1)

    def test_reboot_restarts_full_duration_and_persists_conversion(self):
        self.invoke("set", seconds=75)
        self.invoke("reboot")
        self.assertEqual(self.invoke(now=5000)[2], 75000)
        self.assertEqual(self.invoke(now=10000)[2], 70000)

    def test_clock_rollback_or_failure_blocks(self):
        self.invoke("set", seconds=75)
        for now in (99999, 2**64 - 1):
            result, error, waited, _ = self.invoke(now=now)
            self.assertNotEqual(result, 0)
            self.assertEqual(error, errno.EAGAIN)
            self.assertEqual(waited, 0)

    def test_killed_request_recovers_once_without_permanent_lock(self):
        self.invoke("pending", expected=77)
        result, _, waited, deadline = self.invoke()
        self.assertEqual(result, 0)
        self.assertEqual(waited, 60000)
        self.assertEqual(deadline, 160000)
        self.assertEqual(self.invoke(now=110000)[2], 50000)
        self.assertEqual(self.invoke(now=160000)[2], 0)

    def test_cancelled_recovery_does_not_restart_or_clear_wait(self):
        self.invoke("pending", expected=77)
        result, error, waited, deadline = self.invoke("cancel")
        self.assertNotEqual(result, 0)
        self.assertEqual(error, errno.ECANCELED)
        self.assertEqual(waited, 0)
        self.assertEqual(deadline, 160000)
        self.assertEqual(self.invoke(now=110000)[2], 50000)

    def test_pending_recovery_never_shortens_known_server_deadline(self):
        self.invoke("set", seconds=75)
        self.invoke("pending", expected=77)
        self.assertEqual(self.invoke()[2], 75000)
        self.assertEqual(self.invoke(now=110000)[2], 65000)
        self.invoke("set", now=175000, seconds=7200)
        self.invoke("pending", now=175000, expected=77)
        result, error, waited, deadline = self.invoke(now=175000)
        self.assertNotEqual(result, 0)
        self.assertEqual(error, errno.EAGAIN)
        self.assertEqual(waited, 0)
        self.assertEqual(deadline, 7375000)

    def test_pending_after_reboot_recovers_once_and_keeps_known_delay(self):
        self.invoke("pending-reboot", expected=77)
        self.assertEqual(self.invoke(now=5000)[2], 60000)
        self.assertEqual(self.invoke(now=10000)[2], 55000)
        self.invoke("set", now=65000, seconds=75)
        self.invoke("pending-reboot", now=65000, expected=77)
        self.assertEqual(self.invoke(now=5000)[2], 75000)
        self.assertEqual(self.invoke(now=10000)[2], 70000)

    def test_shorter_response_cannot_replace_known_deadline(self):
        self.invoke("set", seconds=75)
        self.invoke("set", now=110000, seconds=1)
        self.assertEqual(self.invoke(now=110000)[2], 65000)

    def test_corruption_and_unsafe_state_fail_closed(self):
        self.invoke("set", seconds=75)
        record = self.records()[0]
        original = record.read_bytes()
        for corrupt in (b"", original[:-1], original + b"x", original[:100] + bytes([original[100] ^ 1]) + original[101:]):
            record.write_bytes(corrupt)
            result, error, waited, _ = self.invoke()
            self.assertNotEqual(result, 0)
            self.assertEqual(error, errno.EAGAIN)
            self.assertEqual(waited, 0)
        record.write_bytes(original)
        record.chmod(0o644)
        self.assertNotEqual(self.invoke()[0], 0)
        record.chmod(0o600)
        duplicate = self.state / "hardlink"
        os.link(record, duplicate)
        self.assertNotEqual(self.invoke()[0], 0)
        duplicate.unlink()
        saved = self.state / "saved"
        record.rename(saved)
        record.symlink_to(saved)
        self.assertNotEqual(self.invoke()[0], 0)

    def test_missing_assigned_state_or_lock_is_not_a_fresh_slot(self):
        self.invoke("set", seconds=75)
        record = self.records()[0]
        content = record.read_bytes()
        record.unlink()
        self.assertNotEqual(self.invoke()[0], 0)
        record.write_bytes(content)
        record.chmod(0o600)
        record.with_suffix(".lock").unlink()
        self.assertNotEqual(self.invoke()[0], 0)

    def test_storage_scope_count_and_files_are_bounded(self):
        for i in range(32):
            repo = f"http://127.0.0.1:19001/repo-{i}"
            self.assertEqual(self.invoke("set", scope=repo, url=repo + "/index.v1", seconds=75)[0], 0)
        repo = "http://127.0.0.1:19001/extra"
        self.assertNotEqual(self.invoke(scope=repo, url=repo + "/index.v1")[0], 0)
        self.assertEqual(len(self.records()), 32)
        self.assertEqual(len(list((self.state / "cooldown").iterdir())), 65)
        self.assertLess(sum(p.stat().st_size for p in (self.state / "cooldown").iterdir()), 64 * 1024)

    def test_signature_state_is_untouched(self):
        self.state.mkdir(mode=0o700)
        (self.state / "cache").mkdir(mode=0o700)
        sentinels = {"highest-sequence": b"123\n", "cache/verified.v1": b"signed index sentinel", "repo.lock": b"lock sentinel"}
        for name, data in sentinels.items():
            (self.state / name).write_bytes(data)
        self.invoke("set", seconds=75)
        self.invoke()
        for name, data in sentinels.items():
            self.assertEqual((self.state / name).read_bytes(), data)

    def test_actual_http_success_retry_after_survives_exec(self):
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.requests = []
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            repo = f"http://127.0.0.1:{server.server_port}/repo"
            self.assertEqual(self.invoke("fetch", scope=repo, url=repo + "/index.v1")[0], 0)
            result, _, waited, _ = self.invoke("fetch", scope="-", url=repo + "/packages/app.tar.gz", now=105000)
            self.assertEqual(result, 0)
            self.assertEqual(waited, 70000)
            self.assertEqual(server.requests, ["/repo/index.v1", "/repo/packages/app.tar.gz"])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_different_scope_can_progress_while_one_guard_is_held(self):
        args = [str(self.driver), "hold", self.repo, self.repo + "/index.v1", "100000"]
        holder = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            self.assertEqual(holder.stdout.readline().strip(), "LOCKED")
            other = self.repo + "-other"
            self.assertEqual(self.invoke(scope=other, url=other + "/index.v1")[0], 0)
            holder.communicate("release\n", timeout=5)
            self.assertEqual(holder.returncode, 0)
            self.assertEqual(self.invoke()[2], 75000)
        finally:
            if holder.poll() is None:
                holder.kill()
                holder.communicate()


if __name__ == "__main__":
    unittest.main(verbosity=2)
