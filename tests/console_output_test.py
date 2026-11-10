#!/usr/bin/env python3
"""验证重定向 UTF-8 与 Windows 旧代码页控制台的中文输出。"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import unittest


SAMPLE = "接口：以太网 | IPv4 / IPv6".encode("utf-8")


class ConsoleOutputTest(unittest.TestCase):
    binary: str

    @staticmethod
    def hidden_console_options():
        # 每个探针拥有自己的不可见控制台，设置代码页不会影响用户当前终端。
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
        return {"startupinfo": startup, "creationflags": subprocess.CREATE_NEW_CONSOLE}

    def test_redirected_output_is_exact_utf8(self):
        result = subprocess.run([self.binary], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, SAMPLE)

    @unittest.skipUnless(os.name == "nt", "Windows console API only")
    def test_native_console_ignores_legacy_code_page(self):
        for page in ("936", "437"):
            with self.subTest(code_page=page):
                result = subprocess.run(
                    [self.binary, "--verify-console", page],
                    capture_output=True, timeout=10, **self.hidden_console_options(),
                )
                self.assertEqual(
                    result.returncode, 0,
                    f"native console probe code={result.returncode}; stderr={result.stderr!r}",
                )

    @unittest.skipUnless(os.name == "nt", "Windows console API only")
    def test_pipe_preserves_utf8_with_legacy_console_code_page(self):
        for page in ("936", "437"):
            with self.subTest(code_page=page):
                result = subprocess.run(
                    [self.binary, "--pipe-codepage", page],
                    capture_output=True, timeout=10, **self.hidden_console_options(),
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, SAMPLE)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", help="path to the compiled stdout_probe executable")
    options = parser.parse_args()
    ConsoleOutputTest.binary = str(Path(options.binary).resolve())
    unittest.main(argv=[__file__], verbosity=2)
