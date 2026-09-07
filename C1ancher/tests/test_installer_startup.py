"""Execute only production readiness functions with virtual time and fake launch.
No filesystem mutation, ADB, real mount, service start or process control.
"""
from pathlib import Path
import subprocess
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'installer/device-setup.sh'


class InstallerStartupTests(unittest.TestCase):
    def run_case(self, ready_at, writable=False, launch_fails=False, start=True):
        text = SOURCE.read_text()
        functions = text[text.index('wait_bootstrap() {'):text.index('start_core() {')]
        call = '/bin/busybox start-stop-daemon -S -b -x "$DAEMON"'
        self.assertEqual(functions.count(call), 1)
        functions = functions.replace(call, 'fixture_launch')
        script = f'''set -eu
ticks=0
launches=0
chain_stopped=1
DAEMON=/unused
bootstrap_pids() {{ [ "$ticks" -lt {ready_at} ] || printf '123\n'; }}
root_ro() {{ return {1 if writable else 0}; }}
sleep() {{ [ "$1" = 1 ] || exit 99; ticks=$((ticks + 1)); }}
fixture_launch() {{ launches=$((launches + 1)); return {1 if launch_fails else 0}; }}
{functions}
status=0
{'start_chain' if start else 'wait_bootstrap'} || status=$?
printf '%s %s %s %s\n' "$status" "$ticks" "$launches" "$chain_stopped"
'''
        result = subprocess.run(['/bin/sh', '-c', script], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return tuple(map(int, result.stdout.split()))

    def test_background_acknowledgement_waits_for_actual_start(self):
        self.assertEqual(self.run_case(3), (0, 3, 1, 0))

    def test_running_bootstrap_is_not_duplicated(self):
        self.assertEqual(self.run_case(0), (0, 0, 0, 0))

    def test_missing_bootstrap_has_bounded_failure(self):
        self.assertEqual(self.run_case(999), (1, 20, 1, 1))

    def test_writable_root_never_launches(self):
        self.assertEqual(self.run_case(3, writable=True), (1, 0, 0, 1))

    def test_failed_background_launch_is_rejected(self):
        self.assertEqual(self.run_case(3, launch_fails=True), (1, 0, 1, 1))

    def test_verify_waits_without_starting_any_service(self):
        self.assertEqual(self.run_case(4, start=False), (0, 4, 0, 1))


if __name__ == '__main__':
    unittest.main(verbosity=2)
