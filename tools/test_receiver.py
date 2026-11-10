"""通过实际 TCP 请求验证接收示例的协议和输入边界。"""

import copy
import http.client
import io
import json
import threading
import unittest

import receiver


SAMPLE = {
    "schema_version": 1,
    "agent_version": "1.0.0",
    "device_id": "test-device",
    "timestamp": "2026-11-10T08:00:00Z",
    "sequence": 1,
    "event": "startup",
    "hostname": "test-host",
    "os": "windows",
    "ipv4": ["192.0.2.10"],
    "ipv6": ["2001:db8::10"],
    "interfaces": [{"name": "Ethernet", "ipv4": ["192.0.2.10"], "ipv6": ["2001:db8::10"]}],
    "uptime_seconds": 0,
    "status": "alive",
}


class ReceiverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.output = io.StringIO()
        cls.server = receiver.create_server("127.0.0.1", 0, "/api/report", "test-token", cls.output)
        cls.thread = threading.Thread(target=cls.server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)

    def request(self, body=None, path="/api/report", headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=3)
        request_headers = {"Content-Type": "application/json", "Authorization": "Bearer test-token"}
        request_headers.update(headers or {})
        data = json.dumps(SAMPLE).encode() if body is None else body
        connection.request("POST", path, body=data, headers=request_headers)
        response = connection.getresponse()
        response.read()
        status = response.status
        connection.close()
        return status

    def test_valid_report_preserves_both_families_and_peer(self):
        self.assertEqual(self.request(), 204)
        value = json.loads(self.output.getvalue().splitlines()[-1])
        self.assertEqual(value["peer_ip"], "127.0.0.1")
        self.assertEqual(value["report"]["ipv4"], SAMPLE["ipv4"])
        self.assertEqual(value["report"]["ipv6"], SAMPLE["ipv6"])
        self.assertNotIn("test-token", self.output.getvalue())

    def test_authentication_required(self):
        self.assertEqual(self.request(headers={"Authorization": "Bearer wrong"}), 401)

    def test_optional_interface_identity_and_mac(self):
        report = copy.deepcopy(SAMPLE)
        report["interfaces"][0].update({"id": "adapter-guid", "mac": "02:00:00:00:00:01"})
        self.assertEqual(self.request(json.dumps(report).encode()), 204)
        saved = json.loads(self.output.getvalue().splitlines()[-1])["report"]["interfaces"][0]
        self.assertEqual(saved["id"], "adapter-guid")
        self.assertEqual(saved["mac"], "02:00:00:00:00:01")
        report["interfaces"][0]["mac"] = ""
        self.assertEqual(self.request(json.dumps(report).encode()), 204)
        for value in ("not-a-mac", "00:11:22:33:44", 123, None):
            report["interfaces"][0]["mac"] = value
            self.assertEqual(self.request(json.dumps(report).encode()), 400)
        report["interfaces"][0]["mac"] = "02:00:00:00:00:01"
        report["interfaces"][0]["id"] = 123
        self.assertEqual(self.request(json.dumps(report).encode()), 400)

    def test_path_and_content_type(self):
        self.assertEqual(self.request(path="/wrong"), 404)
        self.assertEqual(self.request(headers={"Content-Type": "text/plain"}), 415)

    def test_malformed_json_and_large_body(self):
        self.assertEqual(self.request(b"{broken"), 400)
        self.assertEqual(self.request(b"x" * (receiver.MAX_BODY_BYTES + 1)), 413)
        self.assertEqual(self.request(b"x", headers={"Content-Length": "9" * 5000}), 413)
        self.assertEqual(self.request(b"[" * 1200 + b"]" * 1200), 400)

    def test_wrong_ip_family_and_integer_types(self):
        report = copy.deepcopy(SAMPLE)
        report["ipv6"] = ["192.0.2.1"]
        self.assertEqual(self.request(json.dumps(report).encode()), 400)
        report = copy.deepcopy(SAMPLE)
        report["sequence"] = True
        self.assertEqual(self.request(json.dumps(report).encode()), 400)

    def test_reject_nonstandard_json_and_missing_fields(self):
        self.assertEqual(self.request(b'{"schema_version":NaN}'), 400)
        self.assertEqual(self.request(b"{}"), 400)
        report = copy.deepcopy(SAMPLE)
        report["event"] = []
        self.assertEqual(self.request(json.dumps(report).encode()), 400)

    def test_ambiguous_transfer_encoding_rejected(self):
        self.assertEqual(self.request(headers={"Transfer-Encoding": "chunked"}), 400)


if __name__ == "__main__":
    unittest.main()
