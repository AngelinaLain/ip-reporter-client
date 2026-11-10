#!/usr/bin/env python3
"""独立的 30 秒保活计时测试，可作为慢速 CTest 项运行。"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest

import integration_test


class HeartbeatTest(unittest.TestCase):
    binary: str

    def test_heartbeat_is_scheduled_without_an_address_poll(self):
        server = integration_test.Receiver(("127.0.0.1", 0), integration_test.Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            with tempfile.TemporaryDirectory(prefix="ip-reporter-heartbeat-") as directory:
                config = Path(directory) / "config.json"
                result = subprocess.run(
                    [self.binary, "--init", "--config", str(config), "--endpoint",
                     f"http://127.0.0.1:{server.server_port}/api/report", "--allow-http",
                     "--device-id", "heartbeat-test", "--poll-seconds", "60",
                     "--heartbeat-seconds", "30", "--timeout-seconds", "2"],
                    capture_output=True, timeout=8,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                process = subprocess.Popen(
                    [self.binary, "--config", str(config)],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    env=integration_test.ClientIntegration.environment(),
                )
                try:
                    _, _, first_body = server.requests.get(timeout=8)
                    first = json.loads(first_body)
                    started = time.monotonic()
                    self.assertEqual(first["event"], "startup")
                    _, _, second_body = server.requests.get(timeout=37)
                    elapsed = time.monotonic() - started
                    second = json.loads(second_body)
                    self.assertEqual(second["event"], "heartbeat")
                    self.assertGreaterEqual(elapsed, 29.0, "heartbeat must not form a busy request loop")
                    self.assertLess(elapsed, 37.0)
                    self.assertEqual(second["device_id"], first["device_id"])
                    self.assertGreater(second["sequence"], first["sequence"])
                    self.assertGreaterEqual(second["uptime_seconds"], 30)
                    self.assertIsNone(process.poll())
                finally:
                    process.terminate()
                    _, stderr = process.communicate(timeout=8)
                    if os.name != "nt":
                        self.assertEqual(process.returncode, 0, stderr)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    options = parser.parse_args()
    HeartbeatTest.binary = str(Path(options.binary).resolve())
    unittest.main(argv=[__file__], verbosity=2)
