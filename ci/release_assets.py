"""Prepare and verify the binary assets used by GitHub Releases and the package mirror."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import tarfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]
VERSION = r"(?:0|[1-9][0-9]{0,8})(?:\.(?:0|[1-9][0-9]{0,8})){2}"
PLATFORM = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}-(?:x86_64|arm64)")
MAX_PACKAGE_SIZE = 128 * 1024 * 1024


def project_version() -> str:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(rf"^project\(ip-reporter VERSION ({VERSION}) LANGUAGES CXX\)$", cmake, re.MULTILINE)
    if not match:
        raise ValueError("Cannot read the project version from CMakeLists.txt.")
    return match[1]


def read_plan() -> list[dict[str, str]]:
    plan = json.loads((ROOT / "ci" / "platforms.json").read_text(encoding="utf-8"))
    platforms = [entry["platform"] for entry in plan]
    if not platforms or len(platforms) != len(set(platforms)) or any(not PLATFORM.fullmatch(p) for p in platforms):
        raise ValueError("The build matrix contains an invalid or duplicate platform.")
    return plan


def verify_tag(tag: str, version: str) -> None:
    if tag != f"v{version}":
        raise ValueError(f"Release tag must equal v{version}, the version in CMakeLists.txt.")


def archive_name(version: str, platform: str) -> str:
    extension = ".zip" if platform.startswith("windows-") else ".tar.gz"
    return f"ip-reporter-{version}-{platform}{extension}"


def asset_names(version: str, target: dict[str, str]) -> list[str]:
    platform = target["platform"]
    names = [archive_name(version, platform)]
    native = target.get("native")
    if native == "DEB":
        names.append(f"ip-reporter_{version}_{platform}.deb")
    elif native == "RPM":
        names.append(f"ip-reporter-{version}-{platform}.rpm")
    elif native:
        raise ValueError(f"Unsupported native package format: {native}")
    return names


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_archive(path: Path, version: str, platform: str) -> None:
    """Verify the paths that the server's one-step installer uses."""
    top = f"ip-reporter-{version}-{platform}"
    if platform.startswith("windows-"):
        paths = (f"{top}/bin/ip-reporter.exe", f"{top}/share/ip-reporter/scripts/install.ps1")
        with zipfile.ZipFile(path) as archive:
            for required in paths:
                matches = [member for member in archive.infolist() if member.filename == required]
                if len(matches) != 1 or matches[0].is_dir() or not matches[0].file_size:
                    raise ValueError(f"Archive {path.name} is missing {required}.")
        return

    prefix = top if platform.startswith("macos-") else f"{top}/usr"
    paths = (f"{prefix}/bin/ip-reporter", f"{prefix}/share/ip-reporter/scripts/install.sh")
    with tarfile.open(path, "r:gz") as archive:
        found = set()
        for member in archive:
            if member.name in paths:
                if member.name in found or not member.isfile() or not member.size:
                    raise ValueError(f"Archive {path.name} has an invalid {member.name} entry.")
                found.add(member.name)
        if found != set(paths):
            raise ValueError(f"Archive {path.name} is missing {sorted(set(paths) - found)}.")


def verify_artifact(directory: Path, version: str, target: dict[str, str], *, build_output: bool = False) -> dict[str, tuple[str, int]]:
    names = asset_names(version, target)
    if not directory.is_dir():
        raise ValueError(f"Build artifact directory missing: {directory}")
    expected = set(names) | {f"{name}.sha256" for name in names}
    actual = {path.name for path in directory.iterdir()}
    if build_output and (directory / "_CPack_Packages").is_dir():
        actual.remove("_CPack_Packages")
    if actual != expected:
        raise ValueError(f"Unexpected files in {directory.name}. Missing {sorted(expected - actual)}. Extra {sorted(actual - expected)}.")
    result = {}
    for name in names:
        path = directory / name
        checksum = directory / f"{name}.sha256"
        if any(file.is_symlink() or not file.is_file() for file in (path, checksum)):
            raise ValueError(f"Expected regular files for {name}.")
        size = path.stat().st_size
        if not 0 < size <= MAX_PACKAGE_SIZE or checksum.stat().st_size > 4096:
            raise ValueError(f"Unexpected asset size for {name}.")
        digest = sha256(path)
        cpack_record = checksum.read_text(encoding="ascii").split()
        if len(cpack_record) != 2 or cpack_record[0] != digest or cpack_record[1].lstrip("*") != name:
            raise ValueError(f"CPack checksum does not match {name}.")
        result[name] = (digest, size)
    verify_archive(directory / names[0], version, target["platform"])
    return result


def prepare_release(artifacts: Path, output: Path, version: str, tag: str, plan: list[dict[str, str]]) -> None:
    """Require all matrix packages and preserve the CPack checksums across artifact transfer."""
    verify_tag(tag, version)
    expected = {f'client-{target["platform"]}' for target in plan}
    actual = {path.name for path in artifacts.iterdir()}
    if actual != expected:
        raise ValueError(f"Build matrix artifacts do not match. Missing {sorted(expected - actual)}. Extra {sorted(actual - expected)}.")

    packages = []
    files = {}
    for target in sorted(plan, key=lambda item: item["platform"]):
        platform = target["platform"]
        directory = artifacts / f"client-{platform}"
        verified = verify_artifact(directory, version, target)
        filename = archive_name(version, platform)
        digest, size = verified[filename]
        packages.append({"platform": platform, "file": filename, "sha256": digest, "size": size})
        for name, (digest, _) in verified.items():
            files[name] = (directory / name, digest)

    output.mkdir(parents=True, exist_ok=False)
    for name, (source, _) in files.items():
        shutil.copyfile(source, output / name)
    index = output / "INSTALL-INDEX.json"
    index.write_bytes((json.dumps({"schemaVersion": 1, "version": version, "tag": tag, "packages": packages}, indent=2) + "\n").encode("utf-8"))
    hashes = {name: digest for name, (_, digest) in files.items()}
    hashes[index.name] = sha256(index)
    checksums = "".join(f"{digest}  {name}\n" for name, digest in sorted(hashes.items()))
    (output / "SHA256SUMS.txt").write_bytes(checksums.encode("ascii"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    matrix = commands.add_parser("matrix")
    matrix.add_argument("--tag")
    verify = commands.add_parser("verify")
    verify.add_argument("--platform", required=True)
    verify.add_argument("--packages", type=Path, required=True)
    prepare = commands.add_parser("prepare")
    prepare.add_argument("--tag", required=True)
    prepare.add_argument("--artifacts", type=Path, required=True)
    prepare.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        version = project_version()
        plan = read_plan()
        if args.command == "matrix":
            if args.tag:
                verify_tag(args.tag, version)
            print(json.dumps({"include": plan}, separators=(",", ":")))
        elif args.command == "verify":
            target = next((item for item in plan if item["platform"] == args.platform), None)
            if target is None:
                raise ValueError("Unknown build platform.")
            verify_artifact(args.packages, version, target, build_output=True)
        else:
            prepare_release(args.artifacts, args.output, version, args.tag, plan)
    except (OSError, ValueError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
