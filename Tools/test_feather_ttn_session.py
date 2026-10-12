#!/usr/bin/env python3
"""Check session comparison and redaction using public synthetic keys only."""

import contextlib
import getpass
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import check_feather_ttn_session as checker


class SessionTests(unittest.TestCase):
    def run_check(self, network="11" * 16, application="22" * 16, terminal=True):
        header = (
            "static const u1_t NWKSKEY[] = {" + ",".join(["0x11"] * 16) + "};\n"
            "static const u1_t APPSKEY[] = {" + ",".join(["0x22"] * 16) + "};\n"
        )
        output = io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "LoraSettings.h"
            path.write_text(header)
            with patch.object(checker.sys.stdin, "isatty", return_value=terminal), \
                 patch.object(checker.getpass, "getpass", side_effect=[network, application]) as prompt, \
                 contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                status = checker.main(["--feather", str(path)])
        return status, output.getvalue(), prompt.call_count

    def test_matching_keys_are_redacted(self):
        status, output, calls = self.run_check()
        self.assertEqual(status, 0)
        self.assertEqual(calls, 2)
        self.assertEqual(output.count("YES"), 2)
        for key in ("11" * 16, "22" * 16):
            self.assertNotIn(key, output)

    def test_network_mismatch_is_identified_without_revealing_keys(self):
        status, output, _ = self.run_check(network="33" * 16)
        self.assertEqual(status, 1)
        self.assertIn("NwkSKey matches the Feather: NO", output)
        self.assertIn("AppSKey matches the Feather: YES", output)
        self.assertNotIn("33" * 16, output)

    def test_swapped_keys_fail_both_comparisons(self):
        status, output, _ = self.run_check(network="22" * 16, application="11" * 16)
        self.assertEqual(status, 1)
        self.assertEqual(output.count("NO"), 2)

    def test_malformed_input_is_not_printed(self):
        for value in ("not-a-key", "GG" * 16, "11" * 15):
            with self.subTest(value=value):
                status, output, calls = self.run_check(network=value)
                self.assertEqual(status, 2)
                self.assertEqual(calls, 1)
                self.assertNotIn(value, output)

    def test_noninteractive_input_is_refused_before_prompting(self):
        status, output, calls = self.run_check(terminal=False)
        self.assertEqual(status, 2)
        self.assertEqual(calls, 0)
        self.assertIn("interactive Terminal", output)

    def test_echoing_getpass_fallback_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "LoraSettings.h"
            path.write_text("static const u1_t NWKSKEY[]={" + ",".join(["1"]*16) + "};\n"
                            "static const u1_t APPSKEY[]={" + ",".join(["2"]*16) + "};\n")
            output = io.StringIO()
            with patch.object(checker.sys.stdin, "isatty", return_value=True), \
                 patch.object(checker.getpass, "getpass", side_effect=getpass.GetPassWarning), \
                 contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                self.assertEqual(checker.main(["--feather", str(path)]), 2)
            self.assertIn("cannot hide key entry", output.getvalue())


if __name__ == "__main__":
    unittest.main()
