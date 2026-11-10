"""Exercise Unix installers with fake service managers and a temporary filesystem.

Only test copies of the scripts get rewritten paths. Production installers have no
test mode. Every filesystem command used by those copies is additionally replaced
by a mock that rejects paths outside the temporary directory, even when run as root.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


PROJECT = Path(__file__).resolve().parents[1]

MOCK_TOOL = r'''
import json
import os
from pathlib import Path
import shutil
import sys
import xml.etree.ElementTree as ET

root = Path(os.environ["IP_REPORTER_TEST_ROOT"]).resolve()
state_path = root / "mock-state.json"
state = json.loads(state_path.read_text())
name = Path(sys.argv[0]).name
args = sys.argv[1:]
state["events"].append([name, *args])

def save():
    state_path.write_text(json.dumps(state), encoding="utf-8")

def guarded(raw):
    path = Path(raw).resolve()
    if not path.is_relative_to(root):
        raise RuntimeError("TEST SAFETY: path outside sandbox: " + str(path))
    return path

def finish(code=0):
    save()
    raise SystemExit(code)

if name == "uname":
    print(state["os"])
elif name == "id":
    if args == ["-u"]:
        print("0")
    elif args[0] == "-u":
        if args[1] not in state["users"]:
            finish(1)
        print("23456")
    elif args[0] == "-gn":
        print(args[1])
    else:
        raise RuntimeError("Unexpected id arguments")
elif name in ("useradd", "adduser"):
    state["users"].append(args[-1])
elif name == "pw":
    assert args[0] == "useradd"
    state["users"].append(args[1])
elif name in ("addgroup", "chown"):
    if name == "chown":
        for raw in args[1:]:
            guarded(raw)
elif name == "mkdir":
    assert args[0] == "-p"
    for raw in args[1:]:
        guarded(raw).mkdir(parents=True, exist_ok=True)
elif name == "chmod":
    for raw in args[1:]:
        os.chmod(guarded(raw), int(args[0], 8))
elif name == "cp":
    assert len(args) == 2
    shutil.copyfile(guarded(args[0]), guarded(args[1]))
elif name == "mv":
    assert args[0] == "-f" and len(args) == 3
    os.replace(guarded(args[1]), guarded(args[2]))
elif name == "rm":
    assert args[0] == "-f"
    for raw in args[1:]:
        guarded(raw).unlink(missing_ok=True)
elif name == "systemctl":
    if args[0] == "is-active":
        finish(0 if state["active"] else 3)
    elif args[0] in ("stop", "disable"):
        state["active"] = False
    elif args[0] == "enable":
        assert "--now" in args
        state["active"] = True
    elif args[0] not in ("daemon-reload", "reset-failed"):
        raise RuntimeError("Unexpected systemctl arguments")
elif name == "rc-service":
    if args[1] == "status":
        finish(0 if state["active"] else 3)
    elif args[1] == "stop":
        state["active"] = False
    elif args[1] == "start":
        state["active"] = True
    else:
        raise RuntimeError("Unexpected rc-service arguments")
elif name == "rc-update":
    assert args[0] in ("add", "del")
elif name == "launchctl":
    if args[0] == "print":
        finish(0 if state["active"] else 1)
    elif args[0] == "bootout":
        state["active"] = False
    elif args[0] == "bootstrap":
        ET.parse(guarded(args[-1]))
        state["active"] = True
    else:
        raise RuntimeError("Unexpected launchctl arguments")
elif name == "plutil":
    assert args[0] == "-lint"
    ET.parse(guarded(args[1]))
elif name == "service":
    if args[1] == "onestatus":
        finish(0 if state["active"] else 1)
    elif args[1] == "onestop":
        state["active"] = False
    elif args[1] == "start":
        state["active"] = True
    else:
        raise RuntimeError("Unexpected service arguments")
elif name == "sysrc":
    assert args[0] in ("ip_reporter_enable=YES", "-x")
elif name == "ip-reporter":
    options = dict(zip(args[::2], args[1::2])) if args[0] not in ("--init", "--check-config") else dict(zip(args[1::2], args[2::2]))
    config = guarded(options["--config"])
    if args[0] == "--init":
        if options["--endpoint"] == "invalid":
            finish(2)
        payload = {"endpoint": options["--endpoint"], "device_id": options.get("--device-id", "generated"), "bearer_token": ""}
        if "--token-file" in options:
            payload["bearer_token"] = guarded(options["--token-file"]).read_text().strip()
        config.write_text(json.dumps(payload), encoding="utf-8")
    elif args[0] == "--check-config":
        assert json.loads(config.read_text())["endpoint"].startswith("https://")
    else:
        raise RuntimeError("Installer must never launch an unsupervised agent")
else:
    raise RuntimeError("Unexpected mock tool: " + name)
finish()
'''


@unittest.skipIf(os.name == "nt", "Unix service-manager lifecycle tests require POSIX sh")
class InstallerLifecycleTest(unittest.TestCase):
    def run_platform(self, manager: str, platform: str) -> None:
        shell = shutil.which("sh")
        self.assertIsNotNone(shell)
        with tempfile.TemporaryDirectory(prefix="ip-reporter-installer-") as temporary:
            root = Path(temporary).resolve()
            scripts = root / "scripts"
            scripts.mkdir()
            mock_bin = root / "mock-bin"
            mock_bin.mkdir()
            runtime_bin = root / "bin"
            runtime_bin.mkdir()
            state_path = root / "mock-state.json"
            state_path.write_text(json.dumps({"os": platform, "active": False, "users": [], "events": []}))
            shutil.copytree(PROJECT / "packaging" / "services", root / "packaging" / "services")

            # 只改写临时副本；下面的文件系统命令守卫提供第二层隔离。
            system_prefixes = ("/usr/local", "/etc", "/Library", "/run/systemd/system")
            pattern = re.compile("|".join(re.escape(prefix) for prefix in system_prefixes))
            for name in ("install.sh", "uninstall.sh"):
                source = (PROJECT / "scripts" / name).read_text(encoding="utf-8")
                # 持久写入必须经受守卫的命令，禁止未经检查的文件重定向。
                redirections = re.findall(r"(?<![<>])(?:\d*)>{1,2}(?![>&])\s*([^\s;]+)", source)
                self.assertTrue(all(destination == "/dev/null" for destination in redirections), redirections)
                rewritten = pattern.sub(lambda match: str(root) + match.group(), source)
                # 剥去沙盒前缀后所有生产路径均须带沙盒，防止未来新增路径遗漏。
                for match in pattern.finditer(rewritten):
                    self.assertEqual(rewritten[max(0, match.start() - len(str(root))):match.start()], str(root))
                (scripts / name).write_text(rewritten, encoding="utf-8")

            tool_text = "#!" + sys.executable + "\n" + MOCK_TOOL
            for name in ("uname", "id", "useradd", "adduser", "addgroup", "pw", "mkdir", "chmod", "chown", "cp", "mv", "rm", "systemctl", "rc-service", "rc-update", "launchctl", "plutil", "service", "sysrc", "install", "touch", "tee", "dd", "truncate", "ln", "rmdir", "mount", "umount", "tar", "cpio"):
                path = mock_bin / name
                path.write_text(tool_text, encoding="utf-8")
                path.chmod(0o755)
            agent = runtime_bin / "ip-reporter"
            agent.write_text(tool_text, encoding="utf-8")
            agent.chmod(0o755)
            (root / "etc" / "systemd" / "system").mkdir(parents=True)
            (root / "etc" / "init.d").mkdir()
            (root / "Library" / "LaunchDaemons").mkdir(parents=True)
            if manager == "systemd":
                (root / "run" / "systemd" / "system").mkdir(parents=True)
            env = os.environ.copy()
            env["IP_REPORTER_TEST_ROOT"] = str(root)
            env["PATH"] = str(mock_bin) + os.pathsep + os.defpath
            token = root / "token.txt"
            token.write_text("example-test-token\n")

            def invoke(script: str, *args: str, success: bool = True) -> subprocess.CompletedProcess:
                result = subprocess.run([shell, str(scripts / script), *args], cwd=runtime_bin, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
                if success:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0)
                return result

            def state() -> dict:
                return json.loads(state_path.read_text())

            config = root / "etc" / "ip-reporter" / "config.json"
            installed = root / "usr" / "local" / "bin" / "ip-reporter"
            invoke("install.sh", "--endpoint", "https://first.example/report", "--device-id", "first-device", "--token-file", str(token))
            self.assertTrue(state()["active"])
            self.assertTrue(installed.is_file())
            first_config = config.read_bytes()
            self.assertEqual(json.loads(first_config)["bearer_token"], "example-test-token")
            self.assertEqual(config.stat().st_mode & 0o777, 0o640)
            self.assertEqual(config.parent.stat().st_mode & 0o777, 0o750)

            # 无 --reconfigure 的升级保留内容；无斜杠相对二进制路径也应可用。
            invoke("install.sh", "--binary", "ip-reporter", "--endpoint", "https://ignored.example/report")
            self.assertEqual(config.read_bytes(), first_config)
            self.assertTrue(state()["active"])

            event_count = len(state()["events"])
            invoke("install.sh", "--reconfigure", "--endpoint", "invalid", success=False)
            self.assertTrue(state()["active"])
            self.assertEqual(config.read_bytes(), first_config)
            new_events = state()["events"][event_count:]
            self.assertFalse(any(event[0] in ("systemctl", "rc-service", "launchctl", "service") and any(item in ("stop", "bootout", "onestop") for item in event[1:]) for event in new_events))

            invoke("install.sh", "--reconfigure", "--endpoint", "https://second.example/report", "--device-id", "second-device")
            second_config = config.read_bytes()
            self.assertEqual(json.loads(second_config)["device_id"], "second-device")
            self.assertEqual(json.loads(second_config)["bearer_token"], "")
            invoke("uninstall.sh")
            self.assertFalse(state()["active"])
            self.assertFalse(installed.exists())
            self.assertEqual(config.read_bytes(), second_config)
            invoke("install.sh")
            self.assertTrue(state()["active"])
            self.assertEqual(config.read_bytes(), second_config)
            invoke("uninstall.sh", "--purge-config")
            self.assertFalse(installed.exists())
            self.assertFalse(config.exists())
            invoke("uninstall.sh", "--purge-config")

    def test_filesystem_guard_rejects_external_paths(self):
        with tempfile.TemporaryDirectory(prefix="ip-reporter-guard-") as temporary:
            outer = Path(temporary).resolve()
            root = outer / "sandbox"
            root.mkdir()
            sentinel = outer / "untouched.txt"
            sentinel.write_text("must remain")
            (root / "mock-state.json").write_text(json.dumps({"events": []}))
            mock_rm = root / "rm"
            mock_rm.write_text("#!" + sys.executable + "\n" + MOCK_TOOL, encoding="utf-8")
            mock_rm.chmod(0o755)
            env = os.environ.copy()
            env["IP_REPORTER_TEST_ROOT"] = str(root)
            result = subprocess.run([str(mock_rm), "-f", str(sentinel)], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("TEST SAFETY", result.stderr)
            self.assertEqual(sentinel.read_text(), "must remain")

    def test_systemd(self):
        self.run_platform("systemd", "Linux")

    def test_openrc(self):
        self.run_platform("openrc", "Linux")

    def test_launchd(self):
        self.run_platform("launchd", "Darwin")

    def test_freebsd(self):
        self.run_platform("freebsd", "FreeBSD")


if __name__ == "__main__":
    unittest.main(verbosity=2)
