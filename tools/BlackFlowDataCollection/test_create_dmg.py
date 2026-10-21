import subprocess
import unittest
from unittest.mock import patch

from create_dmg import create_dmg


class CreateDmgTests(unittest.TestCase):
    def run_sequence(self, results):
        calls = []
        def run(command, **kwargs):
            calls.append(command)
            if command[:2] == ["hdiutil", "create"]:
                code, output = next(results)
                return subprocess.CompletedProcess(command, code, output)
            return subprocess.CompletedProcess(command, 0)
        return calls, run

    @patch("create_dmg.time.sleep")
    def test_busy_retries_same_image_then_verifies(self, sleep):
        calls, run = self.run_sequence(iter([(16, "Resource busy\n"), (0, "created\n")]))
        with patch("create_dmg.subprocess.run", side_effect=run):
            create_dmg("stage", "out.dmg", "MAA")
        self.assertEqual(calls[0], ["sync"])
        self.assertEqual(calls[1], calls[3])
        self.assertEqual(calls[-1], ["hdiutil", "verify", "out.dmg"])
        sleep.assert_called_once_with(5)

    @patch("create_dmg.time.sleep")
    def test_other_errors_fail_without_retry_or_verification(self, sleep):
        calls, run = self.run_sequence(iter([(1, "No space left on device\n")]))
        with patch("create_dmg.subprocess.run", side_effect=run):
            with self.assertRaises(subprocess.CalledProcessError):
                create_dmg("stage", "out.dmg", "MAA")
        self.assertEqual(len(calls), 2)
        sleep.assert_not_called()

    @patch("create_dmg.time.sleep")
    def test_busy_exhaustion_cannot_publish_partial_image(self, sleep):
        calls, run = self.run_sequence(iter([(16, "Resource busy\n")] * 5))
        with patch("create_dmg.subprocess.run", side_effect=run):
            with self.assertRaises(subprocess.CalledProcessError):
                create_dmg("stage", "out.dmg", "MAA")
        self.assertEqual(sum(c[:2] == ["hdiutil", "create"] for c in calls), 5)
        self.assertFalse(any(c[:2] == ["hdiutil", "verify"] for c in calls))
        self.assertEqual(sleep.call_count, 4)

    def test_verification_failure_is_fatal(self):
        def run(command, **kwargs):
            if command[:2] == ["hdiutil", "verify"]:
                raise subprocess.CalledProcessError(1, command)
            return subprocess.CompletedProcess(command, 0, "created\n")
        with patch("create_dmg.subprocess.run", side_effect=run):
            with self.assertRaises(subprocess.CalledProcessError):
                create_dmg("stage", "out.dmg", "MAA")
