#!/usr/bin/env python3
"""使用本机 HTTP 接收器验证编译后的客户端，无需外网或第三方 Python 包。"""

from __future__ import annotations

import argparse
import http.server
import json
import os
from pathlib import Path
import queue
import re
import socket
import socketserver
import ssl
import subprocess
import tempfile
import threading
import time
import unittest


class Receiver(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def server_bind(self):
        # 测试监听数字回环地址，直接使用地址，避免 macOS 反向 DNS 查询拖慢启动。
        socketserver.TCPServer.server_bind(self)
        self.server_name = self.server_address[0]
        self.server_port = self.server_address[1]

    def __init__(self, address, handler):
        super().__init__(address, handler)
        self.requests = queue.Queue()
        self.response_status = 204
        self.delay = 0.0


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        status = self.server.response_status
        self.server.requests.put((self.path, dict(self.headers), body))
        if self.server.delay:
            time.sleep(self.server.delay)
        try:
            self.send_response(status)
            if status in (301, 302, 307, 308):
                self.send_header("Location", "/should-not-be-followed")
            self.send_header("Content-Length", "0")
            self.end_headers()
        except (ConnectionResetError, BrokenPipeError):
            # 超时测试中客户端会主动断开连接，这是预期结果。
            pass

    def log_message(self, *_args):
        pass


class ClientIntegration(unittest.TestCase):
    binary: str

    @classmethod
    def setUpClass(cls):
        cls.server = Receiver(("127.0.0.1", 0), Handler)
        cls.worker = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.worker.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.worker.join(timeout=5)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ip-reporter-integration-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name) / "配置 with spaces"
        self.directory.mkdir()
        self.config_path = self.directory / "config.json"
        self.token_path = self.directory / "token.txt"
        self.token = "integration-test-bearer-secret"
        self.token_path.write_text(self.token + "\n", encoding="utf-8")
        self.endpoint = f"http://127.0.0.1:{self.server.server_port}/api/report"
        self.server.response_status = 204
        self.server.delay = 0.0
        while not self.server.requests.empty():
            self.server.requests.get_nowait()
        self.initialize()

    @staticmethod
    def environment():
        # 测试不得继承代理设置，保证请求只发往本机测试接收器。
        environment = dict(os.environ)
        for key in list(environment):
            if key.lower() in {"http_proxy", "https_proxy", "all_proxy"}:
                del environment[key]
        environment["NO_PROXY"] = "*"
        return environment

    def command(self, *args, expected=0, timeout=12):
        result = subprocess.run(
            [self.binary, *map(str, args)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            env=self.environment(),
        )
        if expected is not None:
            if expected == 0:
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            else:
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn(self.token, result.stdout + result.stderr)
        return result

    def initialize(self):
        return self.command(
            "--init", "--config", self.config_path, "--endpoint", self.endpoint,
            "--token-file", self.token_path, "--device-id", "integration-device",
            "--allow-http", "--poll-seconds", "5", "--heartbeat-seconds", "0",
            "--timeout-seconds", "1",
        )

    def test_init_validation_and_overwrite_protection(self):
        before = self.config_path.read_bytes()
        config = json.loads(before)
        self.assertEqual(config["endpoint"], self.endpoint)
        self.assertEqual(config["device_id"], "integration-device")
        self.assertEqual(config["bearer_token"], self.token)
        self.command("--check-config", "--config", self.config_path)
        self.command("--init", "--config", self.config_path, "--endpoint", self.endpoint,
                     "--allow-http", expected=1)
        self.assertEqual(self.config_path.read_bytes(), before)
        self.assertTrue(self.server.requests.empty(), "init/check must not connect")

    def test_print_payload_and_once(self):
        result = self.command("--print-payload", "--config", self.config_path)
        preview = json.loads(result.stdout)
        self.assertEqual(preview["schema_version"], 1)
        for interface in preview["interfaces"]:
            self.assertIsInstance(interface["id"], str)
            self.assertTrue(interface["id"], "collected interfaces must have a stable selection key")
            self.assertIsInstance(interface["mac"], str)
            self.assertTrue(not interface["mac"] or re.fullmatch(r"(?:[0-9a-f]{2}:){5,19}[0-9a-f]{2}", interface["mac"]))
        self.assertTrue(self.server.requests.empty(), "preview must not connect")
        delivered = self.command("--once", "--config", self.config_path)
        self.assertEqual(delivered.stdout, "", "successful --once should not print payloads")
        self.assertEqual(delivered.stderr, "", "default error logging should stay silent on success")
        path, headers, body = self.server.requests.get(timeout=2)
        self.assertEqual(path, "/api/report")
        normalized_headers = {key.lower(): value for key, value in headers.items()}
        self.assertTrue(normalized_headers["content-type"].startswith("application/json"))
        self.assertEqual(normalized_headers["authorization"], "Bearer " + self.token)
        payload = json.loads(body)
        self.assertEqual(payload["device_id"], "integration-device")
        self.assertEqual(payload["event"], "manual")
        self.assertEqual(payload["status"], "alive")
        self.assertIsInstance(payload["ipv4"], list)
        self.assertIsInstance(payload["ipv6"], list)
        self.assertIsInstance(payload["interfaces"], list)
        self.assertNotIn(self.token, body.decode("utf-8"))

    def test_concurrent_initialization_never_overwrites_a_winner(self):
        race_path = self.directory / "concurrent-config.json"
        processes = []
        try:
            for index in range(12):
                process = subprocess.Popen(
                    [self.binary, "--init", "--config", str(race_path),
                     "--endpoint", self.endpoint, "--allow-http", "--device-id", f"racer-{index}"],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                )
                processes.append(process)
            successful = []
            for index, process in enumerate(processes):
                stdout, stderr = process.communicate(timeout=10)
                if process.returncode == 0:
                    successful.append(index)
                self.assertNotIn(self.token.encode(), stdout + stderr)
            self.assertEqual(len(successful), 1, "exactly one initializer may create the config")
            winner = json.loads(race_path.read_text(encoding="utf-8-sig"))
            self.assertEqual(winner["device_id"], f"racer-{successful[0]}")
            self.assertEqual(list(self.directory.glob("*.tmp")), [], "failed initializers must remove temporary files")
            self.command("--init", "--force", "--config", race_path, "--endpoint", self.endpoint,
                         "--allow-http", "--device-id", "forced-replacement")
            replaced = json.loads(race_path.read_text(encoding="utf-8-sig"))
            self.assertEqual(replaced["device_id"], "forced-replacement")
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=8)

    def test_http_errors_and_redirects_are_not_success(self):
        for status in (401, 503, 302, 307):
            with self.subTest(status=status):
                self.server.response_status = status
                self.command("--once", "--config", self.config_path, expected=1)
                path, _, _ = self.server.requests.get(timeout=2)
                self.assertEqual(path, "/api/report")
                self.assertTrue(self.server.requests.empty(), "redirect must not forward credentials")

    def test_network_timeout_is_bounded(self):
        self.server.delay = 3.0
        started = time.monotonic()
        self.command("--once", "--config", self.config_path, expected=1, timeout=8)
        self.assertLess(time.monotonic() - started, 7.0)
        self.server.requests.get(timeout=2)

    def test_invalid_configuration_is_rejected_before_network(self):
        base = json.loads(self.config_path.read_text(encoding="utf-8-sig"))
        variants = [
            {**base, "allow_http": False},
            {**base, "unknown_option": True},
            {**base, "poll_interval_seconds": "5"},
            {**base, "bearer_token": "secret\r\nInjected: true"},
        ]
        for config in variants:
            with self.subTest(config=config):
                self.config_path.write_text(json.dumps(config), encoding="utf-8")
                self.command("--check-config", "--config", self.config_path, expected=1)
                self.command("--once", "--config", self.config_path, expected=1)
                self.assertTrue(self.server.requests.empty())

    def test_startup_and_unchanged_address_suppression(self):
        process = subprocess.Popen(
            [self.binary, "--config", str(self.config_path)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=self.environment(),
        )
        try:
            _, _, body = self.server.requests.get(timeout=8)
            self.assertEqual(json.loads(body)["event"], "startup")
            time.sleep(5.7)
            self.assertIsNone(process.poll(), "background loop must remain alive")
            self.assertTrue(self.server.requests.empty(), "unchanged addresses should wait for heartbeat")
        finally:
            process.terminate()
            stdout, stderr = process.communicate(timeout=8)
            self.assertNotIn(self.token.encode(), stdout + stderr)
            if os.name != "nt":
                self.assertEqual(process.returncode, 0, "SIGTERM must stop the worker cleanly")

    def test_failed_startup_is_retried_without_waiting_for_poll(self):
        config = json.loads(self.config_path.read_text(encoding="utf-8-sig"))
        config["poll_interval_seconds"] = 60
        self.config_path.write_text(json.dumps(config), encoding="utf-8")
        self.server.response_status = 503
        process = subprocess.Popen(
            [self.binary, "--config", str(self.config_path)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=self.environment(),
        )
        try:
            _, _, first = self.server.requests.get(timeout=8)
            self.assertEqual(json.loads(first)["event"], "startup")
            self.server.response_status = 204
            _, _, retry = self.server.requests.get(timeout=10)
            self.assertEqual(json.loads(retry)["event"], "startup")
            self.assertIsNone(process.poll(), "failed delivery must not kill the worker")
        finally:
            process.terminate()
            stdout, stderr = process.communicate(timeout=8)
            self.assertNotIn(self.token.encode(), stdout + stderr)

    def test_ipv6_literal_endpoint(self):
        class IPv6Receiver(Receiver):
            address_family = socket.AF_INET6

        try:
            server = IPv6Receiver(("::1", 0), Handler)
        except OSError as error:
            self.skipTest(f"IPv6 loopback unavailable: {error}")
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            config = json.loads(self.config_path.read_text(encoding="utf-8-sig"))
            config["endpoint"] = f"http://[::1]:{server.server_port}/api/report"
            self.config_path.write_text(json.dumps(config), encoding="utf-8")
            self.command("--once", "--config", self.config_path)
            path, _, _ = server.requests.get(timeout=2)
            self.assertEqual(path, "/api/report")
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=5)

    def test_untrusted_tls_certificate_is_rejected(self):
        # 固定的测试专用自签名证书避免依赖外网、系统信任库修改或运行时 OpenSSL。
        fixtures = Path(__file__).resolve().parent / "fixtures"
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(fixtures / "localhost-test.crt", fixtures / "localhost-test.key")
        server = Receiver(("127.0.0.1", 0), Handler)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            config = json.loads(self.config_path.read_text(encoding="utf-8-sig"))
            config["endpoint"] = f"https://127.0.0.1:{server.server_port}/api/report"
            config["allow_http"] = False
            config["request_timeout_seconds"] = 3
            self.config_path.write_text(json.dumps(config), encoding="utf-8")
            self.command("--once", "--config", self.config_path, expected=1)
            self.assertTrue(server.requests.empty(), "untrusted TLS must not receive the bearer token")
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", help="path to compiled ip-reporter executable")
    options = parser.parse_args()
    ClientIntegration.binary = str(Path(options.binary).resolve())
    unittest.main(argv=[__file__], verbosity=2)
