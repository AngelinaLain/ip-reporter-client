#!/usr/bin/env python3
"""IP Reporter JSON 接收示例；生产环境请在反向代理后运行并启用 TLS。"""

import argparse
import datetime as dt
import hmac
import http.server
import ipaddress
import json
from pathlib import Path
import re
import socket
import sys

MAX_BODY_BYTES = 65536
EVENTS = {"startup", "address_change", "heartbeat", "manual"}


def utc_now():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def valid_report(value):
    """校验协议的重要字段，避免把任意请求写入报告文件。"""
    if not isinstance(value, dict):
        return False
    if type(value.get("schema_version")) is not int or value["schema_version"] != 1:
        return False
    if not isinstance(value.get("event"), str) or value["event"] not in EVENTS or value.get("status") != "alive":
        return False
    for name, limit in (("device_id", 128), ("agent_version", 64), ("hostname", 255), ("os", 64)):
        if not isinstance(value.get(name), str) or not 1 <= len(value[name]) <= limit:
            return False
    for name in ("sequence", "uptime_seconds"):
        if type(value.get(name)) is not int or not 0 <= value[name] <= 18446744073709551615:
            return False
    try:
        timestamp = value["timestamp"]
        if not isinstance(timestamp, str) or len(timestamp) > 64:
            return False
        if "T" not in timestamp:
            return False
        parsed = dt.datetime.fromisoformat(timestamp.replace("Z", "+00:00"))
        if parsed.utcoffset() != dt.timedelta(0):
            return False
        for name, version in (("ipv4", 4), ("ipv6", 6)):
            addresses = value[name]
            if not isinstance(addresses, list) or len(addresses) > 1024:
                return False
            for address in addresses:
                if not isinstance(address, str) or ipaddress.ip_address(address).version != version:
                    return False
        interfaces = value["interfaces"]
        if not isinstance(interfaces, list) or len(interfaces) > 1024:
            return False
        for interface in interfaces:
            if not isinstance(interface, dict):
                return False
            if not isinstance(interface.get("name"), str) or not 1 <= len(interface["name"]) <= 256:
                return False
            # 1.1.0 添加的网卡标识和 MAC 为可选字段，兼容旧版客户端。
            if "id" in interface and (not isinstance(interface["id"], str) or len(interface["id"]) > 256):
                return False
            if "mac" in interface:
                mac = interface["mac"]
                if not isinstance(mac, str) or (mac and not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5,19}[0-9a-fA-F]{2}", mac)):
                    return False
            for name, version in (("ipv4", 4), ("ipv6", 6)):
                if not isinstance(interface.get(name), list) or len(interface[name]) > 1024:
                    return False
                for address in interface[name]:
                    if not isinstance(address, str) or ipaddress.ip_address(address).version != version:
                        return False
    except (KeyError, ValueError, TypeError):
        return False
    return True


def strict_json(data):
    """拒绝 JSON 标准之外的 NaN / Infinity。"""
    def reject_constant(value):
        raise ValueError("non-finite JSON constant")
    return json.loads(data, parse_constant=reject_constant)


class ReportHandler(http.server.BaseHTTPRequestHandler):
    server_version = "IpReporterReceiver/1.0"
    sys_version = ""

    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def log_message(self, format, *args):
        # 默认不输出 URL、认证头或报告内容；报告只进入指定 JSONL 输出。
        pass

    def respond(self, status):
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def do_POST(self):
        """接收完整 JSON 报告；peer_ip 只取 TCP 对端，避免伪造代理头。"""
        if self.path != self.server.report_path:
            self.respond(404)
            return
        expected = self.server.bearer_token
        if expected and not hmac.compare_digest(
            self.headers.get("Authorization", "").encode("utf-8"),
            ("Bearer " + expected).encode("utf-8"),
        ):
            self.respond(401)
            return
        if self.headers.get("Transfer-Encoding"):
            self.respond(400)
            return
        lengths = self.headers.get_all("Content-Length", [])
        if len(lengths) != 1 or not lengths[0].isdigit():
            self.respond(411)
            return
        if len(lengths[0]) > 8:
            self.respond(413)
            return
        length = int(lengths[0])
        if length < 1 or length > MAX_BODY_BYTES:
            self.respond(413)
            return
        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if content_type != "application/json":
            self.respond(415)
            return
        try:
            raw = self.rfile.read(length)
            if len(raw) != length:
                self.respond(400)
                return
            report = strict_json(raw.decode("utf-8"))
            if not valid_report(report):
                self.respond(400)
                return
        except (UnicodeDecodeError, ValueError, RecursionError, socket.timeout):
            self.respond(400)
            return
        record = {"received_at": utc_now(), "peer_ip": self.client_address[0], "report": report}
        try:
            self.server.output.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
            self.server.output.flush()
        except OSError:
            self.respond(503)
            return
        self.respond(204)

    def do_GET(self):
        self.respond(405)


def create_server(listen, port, report_path, bearer_token, output):
    """按监听地址选择 IPv4 / IPv6；单线程示例便于审阅和部署。"""
    family = socket.AF_INET6 if ":" in listen else socket.AF_INET

    class Server(http.server.HTTPServer):
        address_family = family

    server = Server((listen, port), ReportHandler)
    server.report_path = report_path
    server.bearer_token = bearer_token
    server.output = output
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--listen", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--path", default="/api/report")
    parser.add_argument("--token-file", type=Path)
    parser.add_argument("--output", type=Path, help="追加写入 JSONL；默认标准输出")
    args = parser.parse_args()
    if not args.path.startswith("/") or "?" in args.path or "#" in args.path:
        parser.error("--path 必须是不含查询参数或片段的绝对路径")
    token = args.token_file.read_text(encoding="utf-8-sig").strip() if args.token_file else ""
    if args.token_file and (not token or any(ord(c) < 33 or ord(c) > 126 for c in token)):
        parser.error("Token 必须是非空的可打印 ASCII 字符，且不含空格")
    output = args.output.open("a", encoding="utf-8", buffering=1) if args.output else sys.stdout
    server = create_server(args.listen, args.port, args.path, token, output)
    print(f"Receiver listening on {args.listen}:{server.server_port}{args.path}", file=sys.stderr)
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if args.output:
            output.close()


if __name__ == "__main__":
    main()
