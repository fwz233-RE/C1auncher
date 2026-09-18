#!/usr/bin/env python3
"""Check effective CLI logging configuration with the real executable."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("TERM_IME_BINARY", "./build/term-ime")


class CliConfigurationTest(unittest.TestCase):
    def invoke(self, root, config):
        path = root / "custom.json"
        path.write_text(json.dumps(config), encoding="utf-8")
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / "config"),
                   XDG_DATA_HOME=str(root / "data"), TERM="xterm-256color")
        # No terminal: initialization intentionally fails, exercising config
        # and logger setup without spawning a shell or deploying a dictionary.
        result = subprocess.run([BINARY, str(path)], input=b"", stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=env, timeout=10)
        self.assertEqual(result.returncode, 1)

    def test_explicit_log_path_and_level_are_honored_without_truncation(self):
        with tempfile.TemporaryDirectory(prefix="term-ime-cli-") as temp:
            root = Path(temp)
            log = root / "logs" / "configured.log"
            self.invoke(root, {"log_file": str(log), "log_level": "error"})
            first = log.read_text()
            self.assertIn("App init failed", first)
            self.assertNotIn("[debug]", first)
            self.assertNotIn("[info]", first)
            self.invoke(root, {"log_file": str(log), "log_level": "error"})
            self.assertEqual(log.read_text().count("App init failed"), 2)
            self.assertFalse((root / ".cache" / "term-ime" / "term-ime.log").exists())

    def test_empty_log_path_disables_file_logging(self):
        with tempfile.TemporaryDirectory(prefix="term-ime-cli-") as temp:
            root = Path(temp)
            self.invoke(root, {"log_file": "", "log_level": "debug"})
            self.assertFalse(list(root.rglob("*.log")))


if __name__ == "__main__":
    unittest.main()
