"""Host process regressions; compiles the real launcher, never accesses hardware."""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LauncherProcessTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="c1-launcher-process-")
        self.root = Path(self.directory.name)
        self.launcher = self.root / "C1ancher-launcher"
        subprocess.run([
            "cc", "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra",
            "-Wpedantic", "-Werror", "-I" + str(ROOT / "src"),
            str(ROOT / "src/launcher/main.c"), str(ROOT / "src/launcher/policy.c"),
            str(ROOT / "src/platform/liveness.c"),
            "-o", str(self.launcher),
        ], check=True)
        self.process = None

    def tearDown(self):
        if self.process is not None:
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.process.wait(timeout=5)
        self.directory.cleanup()

    def start(self, body, *args):
        app = self.root / "C1ancher"
        app.write_text("#!/bin/sh\n" + body)
        app.chmod(0o700)
        self.process = subprocess.Popen([str(self.launcher), *args], start_new_session=True)

    def wait_for(self, predicate):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.01)
        self.fail("launcher fixture timed out")

    def start_python(self, body, *args, **kwargs):
        app = self.root / "C1ancher"
        app.write_text("#!/usr/bin/python3\n" + body)
        app.chmod(0o700)
        env = dict(os.environ, C1_HEARTBEAT_STARTUP_MS="500",
                   C1_HEARTBEAT_TIMEOUT_MS="300")
        env.update(kwargs.pop("env", {}))
        self.process = subprocess.Popen([str(self.launcher), *args],
                                        start_new_session=True, env=env, **kwargs)

    def test_missing_startup_heartbeat_revokes_ready(self):
        ready = self.root / "ready"
        self.start_python(f"from pathlib import Path\nimport time\n"
                          f"Path({str(ready)!r}).write_text('digest')\ntime.sleep(60)\n",
                          "--ready-file", str(ready))
        self.wait_for(ready.exists)
        self.wait_for(lambda: not ready.exists())
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_heartbeat_hang_before_and_during_ready(self):
        ready = self.root / "ready"
        marker = self.root / "pulse"
        self.start_python(
            "import os,time\nfrom pathlib import Path\n"
            "os.write(int(os.environ['C1_UI_HEARTBEAT_FD']), b'H')\n"
            f"Path({str(ready)!r}).write_text('digest')\n"
            f"Path({str(marker)!r}).touch()\ntime.sleep(60)\n",
            "--ready-file", str(ready))
        self.wait_for(marker.exists)
        self.wait_for(lambda: not ready.exists())
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_continuous_heartbeats_survive_idle_window(self):
        marker = self.root / "pid"
        self.start_python(
            "import os,time\nfrom pathlib import Path\n"
            f"Path({str(marker)!r}).write_text(str(os.getpid()))\n"
            "while True:\n os.write(int(os.environ['C1_UI_HEARTBEAT_FD']), b'H')\n time.sleep(.05)\n")
        self.wait_for(marker.exists)
        child = int(marker.read_text())
        time.sleep(.9)
        os.kill(child, 0)
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_healthy_ui_reaps_adopted_exited_app(self):
        ui, descendant_file, release = (self.root / name for name in
                                         ("ui", "descendant", "release"))
        self.start_python(
            "import os,time\nfrom pathlib import Path\n"
            f"Path({str(ui)!r}).write_text(str(os.getpid()))\n"
            "child=os.fork()\n"
            "if child == 0:\n"
            " if os.fork() == 0:\n"
            "  os.setsid()\n"
            f"  Path({str(descendant_file)!r}).write_text(str(os.getpid()))\n"
            f"  while not Path({str(release)!r}).exists(): time.sleep(.01)\n"
            "  os._exit(75)\n"
            " os._exit(0)\n"
            "os.waitpid(child,0)\n"
            "while True:\n os.write(int(os.environ['C1_UI_HEARTBEAT_FD']),b'H')\n time.sleep(.05)\n")
        self.wait_for(lambda: descendant_file.exists() and descendant_file.stat().st_size > 0)
        descendant = int(descendant_file.read_text())
        ui_pid = int(ui.read_text())
        # Adoption alone must not terminate a still-running application.
        time.sleep(.2)
        self.assertTrue(Path(f"/proc/{descendant}").exists())
        release.touch()
        self.wait_for(lambda: not Path(f"/proc/{descendant}").exists())
        os.kill(ui_pid, 0)
        self.assertIsNone(self.process.poll())
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_crashed_ui_cleans_detached_lock_holder(self):
        import fcntl
        pidfile, lock = self.root / "descendant", self.root / "runlock"
        self.start_python(
            "import os,time,fcntl,signal\nfrom pathlib import Path\n"
            "child=os.fork()\n"
            "if child == 0:\n"
            " os.setsid()\n signal.signal(signal.SIGHUP,signal.SIG_IGN)\n"
            f" fd=os.open({str(lock)!r},os.O_CREAT|os.O_RDWR,0o600)\n"
            " fcntl.flock(fd,fcntl.LOCK_EX)\n"
            f" Path({str(pidfile)!r}).write_text(str(os.getpid()))\n time.sleep(60)\n"
            "else:\n"
            f" while not Path({str(pidfile)!r}).exists(): time.sleep(.01)\n"
            " os._exit(75)\n")
        self.wait_for(pidfile.exists)
        descendant = int(pidfile.read_text())
        self.assertEqual(self.process.wait(timeout=5), 75)
        with self.assertRaises(ProcessLookupError):
            os.kill(descendant, 0)
        with lock.open("r+") as stream:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_killed_ui_cleans_detached_lock_holder(self):
        pidfile, ui = self.root / "descendant", self.root / "ui"
        self.start_python(
            "import os,time,signal\nfrom pathlib import Path\n"
            f"Path({str(ui)!r}).write_text(str(os.getpid()))\n"
            "child=os.fork()\n"
            "if child == 0:\n"
            " os.setsid()\n signal.signal(signal.SIGHUP,signal.SIG_IGN)\n"
            f" Path({str(pidfile)!r}).write_text(str(os.getpid()))\n"
            " time.sleep(60)\n"
            "else:\n"
            " while True:\n  os.write(int(os.environ['C1_UI_HEARTBEAT_FD']),b'H')\n  time.sleep(.05)\n")
        self.wait_for(pidfile.exists)
        descendant = int(pidfile.read_text())
        os.kill(int(ui.read_text()), signal.SIGKILL)
        self.wait_for(lambda: not Path(f"/proc/{descendant}").exists())
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_stopped_ui_is_detected_while_external_app_runs(self):
        ready, ui, app = self.root / "ready", self.root / "ui", self.root / "app"
        self.start_python(
            "import os,time\nfrom pathlib import Path\n"
            f"Path({str(ui)!r}).write_text(str(os.getpid()))\n"
            f"Path({str(ready)!r}).write_text('digest')\n"
            "if os.fork() == 0:\n os.setsid()\n"
            f" Path({str(app)!r}).write_text(str(os.getpid()))\n time.sleep(60)\n"
            "else:\n while True:\n  os.write(int(os.environ['C1_UI_HEARTBEAT_FD']),b'H')\n  time.sleep(.05)\n",
            "--ready-file", str(ready))
        self.wait_for(lambda: app.exists() and app.stat().st_size > 0)
        descendant = int(app.read_text())
        os.kill(int(ui.read_text()), signal.SIGSTOP)
        self.wait_for(lambda: not ready.exists())
        self.wait_for(lambda: not Path(f"/proc/{descendant}").exists())
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_cleanup_does_not_signal_unrelated_process(self):
        sentinel = subprocess.Popen(["sleep", "60"], start_new_session=True)
        try:
            self.start("exit 75\n")
            self.assertEqual(self.process.wait(timeout=5), 75)
            self.assertIsNone(sentinel.poll())
        finally:
            sentinel.kill()
            sentinel.wait(timeout=5)

    def test_upstream_progress_is_bound_to_ui_pid(self):
        import struct
        read_fd, write_fd = os.pipe()
        os.set_blocking(read_fd, False)
        try:
            self.start_python(
                "import os,time\nwhile True:\n os.write(int(os.environ['C1_UI_HEARTBEAT_FD']),b'H')\n time.sleep(.05)\n",
                env={"C1_SUPERVISOR_HEARTBEAT_FD": str(write_fd)}, pass_fds=(write_fd,))
            import select
            self.assertTrue(select.select([read_fd], [], [], 3)[0])
            timestamp, pid = struct.unpack("=qq", os.read(read_fd, 16))
            self.assertGreater(timestamp, 0)
            self.assertGreater(pid, 0)
            self.assertNotEqual(pid, self.process.pid)
            self.process.terminate()
            self.assertEqual(self.process.wait(timeout=5), 0)
        finally:
            os.close(read_fd)
            os.close(write_fd)

    def test_stop_during_backoff_does_not_spawn_again(self):
        calls = self.root / "calls"
        self.start(f"echo started >> '{calls}'\nexit 1\n")
        self.wait_for(calls.exists)
        time.sleep(0.2)  # Real launcher has reaped the immediate failure.
        self.process.send_signal(signal.SIGTERM)
        self.assertEqual(self.process.wait(timeout=5), 0)
        self.assertEqual(calls.read_text().splitlines(), ["started"])

    def test_app_exit_clears_readiness_during_backoff(self):
        ready = self.root / "ready"
        exited = self.root / "exited"
        self.start(f"echo ready > '{ready}'\ntouch '{exited}'\nexit 1\n",
                   "--ready-file", str(ready))
        self.wait_for(exited.exists)
        self.wait_for(lambda: not ready.exists())
        self.process.send_signal(signal.SIGTERM)
        self.assertEqual(self.process.wait(timeout=5), 0)

    def test_stop_reaps_term_ignoring_child(self):
        started = self.root / "started"
        self.start(f"trap '' TERM\ntouch '{started}'\nwhile :; do :; done\n")
        self.wait_for(started.exists)
        self.process.send_signal(signal.SIGTERM)
        self.assertEqual(self.process.wait(timeout=5), 0)


if __name__ == "__main__":
    unittest.main()
