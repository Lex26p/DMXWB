#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import re
import sys
import tarfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath


ROOT = "dmxwb-wb8-bullseye-arm64"
PAYLOAD_FILES = {
    "payload/usr/local/bin/dmxwb": 0o755,
    "payload/etc/dmxwb/config.example.json": 0o644,
    "payload/etc/systemd/system/dmxwb.service": 0o644,
    "payload/var/www/dmxwb/index.html": 0o644,
    "payload/var/www/dmxwb/app.js": 0o644,
    "payload/var/www/dmxwb/model.js": 0o644,
    "payload/var/www/dmxwb/mqtt-client.js": 0o644,
    "payload/var/www/dmxwb/styles.css": 0o644,
}
CONTROL_FILES = {"install.sh": 0o755, "uninstall.sh": 0o755}
METADATA_FILES = {"MANIFEST.txt": 0o644, "SHA256SUMS": 0o644}
EXPECTED_FILES = PAYLOAD_FILES | CONTROL_FILES | METADATA_FILES
EXPECTED_DIRECTORIES = {
    "",
    "payload",
    "payload/usr",
    "payload/usr/local",
    "payload/usr/local/bin",
    "payload/etc",
    "payload/etc/dmxwb",
    "payload/etc/systemd",
    "payload/etc/systemd/system",
    "payload/var",
    "payload/var/www",
    "payload/var/www/dmxwb",
}


def fail(message: str) -> None:
    raise SystemExit(f"DEV-012C4 final bundle contract FAIL: {message}")


def read_file(archive: tarfile.TarFile, member: tarfile.TarInfo) -> bytes:
    stream = archive.extractfile(member)
    if stream is None:
        fail(f"cannot read {member.name}")
    return stream.read()


def parse_manifest(data: bytes) -> dict[str, list[str]]:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        fail(f"MANIFEST.txt is not UTF-8: {error}")
    result: dict[str, list[str]] = {}
    for line in text.splitlines():
        if not line or "=" not in line:
            fail(f"invalid manifest line: {line!r}")
        key, value = line.split("=", 1)
        result.setdefault(key, []).append(value)
    return result


def one(manifest: dict[str, list[str]], key: str) -> str:
    values = manifest.get(key, [])
    if len(values) != 1 or not values[0]:
        fail(f"manifest field {key!r} must occur exactly once")
    return values[0]


def main() -> None:
    if len(sys.argv) != 6:
        raise SystemExit(
            "Usage: check_dev012c4_final_bundle.py "
            "ARCHIVE SOURCE_ID VERSION INSTALLER UNINSTALLER"
        )
    archive_path = Path(sys.argv[1]).resolve()
    expected_source_id = sys.argv[2]
    expected_version = sys.argv[3]
    installer_path = Path(sys.argv[4]).resolve()
    uninstaller_path = Path(sys.argv[5]).resolve()
    repo_root = Path(__file__).resolve().parents[2]

    with tarfile.open(archive_path, mode="r:gz") as archive:
        members: dict[str, tarfile.TarInfo] = {}
        directories: set[str] = set()
        files: set[str] = set()
        all_paths: set[str] = set()
        archive_mtimes: set[int] = set()

        for member in archive.getmembers():
            normalized_name = member.name.rstrip("/")
            path = PurePosixPath(normalized_name)
            if path.is_absolute() or ".." in path.parts:
                fail(f"unsafe archive path: {member.name!r}")
            if not path.parts or path.parts[0] != ROOT:
                fail(f"path outside single bundle root: {member.name!r}")
            relative = PurePosixPath(*path.parts[1:]).as_posix()
            if relative == ".":
                relative = ""
            if relative in all_paths:
                fail(f"duplicate archive member: {member.name!r}")
            all_paths.add(relative)
            archive_mtimes.add(member.mtime)
            if member.issym() or member.islnk() or member.isdev():
                fail(f"links/devices are forbidden: {member.name!r}")
            if member.isdir():
                if member.mode & 0o777 != 0o755:
                    fail(f"directory {member.name!r} mode must be 755")
                if member.uid != 0 or member.gid != 0:
                    fail(f"directory {member.name!r} owner must be root:root")
                directories.add(relative)
            elif member.isfile():
                files.add(relative)
                members[relative] = member
            else:
                fail(f"unsupported archive member type: {member.name!r}")

        if files != set(EXPECTED_FILES):
            fail(
                f"unexpected file set; missing={sorted(set(EXPECTED_FILES) - files)}, "
                f"extra={sorted(files - set(EXPECTED_FILES))}"
            )
        if directories != EXPECTED_DIRECTORIES:
            fail(
                f"unexpected directory set; "
                f"missing={sorted(EXPECTED_DIRECTORIES - directories)}, "
                f"extra={sorted(directories - EXPECTED_DIRECTORIES)}"
            )

        content: dict[str, bytes] = {}
        for path, expected_mode in EXPECTED_FILES.items():
            member = members[path]
            actual_mode = member.mode & 0o777
            if actual_mode != expected_mode:
                fail(f"{path} mode is {actual_mode:o}, expected {expected_mode:o}")
            if member.uid != 0 or member.gid != 0:
                fail(f"{path} archive owner must be root:root")
            content[path] = read_file(archive, member)

    expected_sources = {
        "install.sh": installer_path,
        "uninstall.sh": uninstaller_path,
        "payload/usr/local/bin/dmxwb": (
            repo_root / "artifacts/wb8-bullseye-arm64/dmxwb"
        ),
        "payload/etc/dmxwb/config.example.json": (
            repo_root / "deploy/config.example.json"
        ),
        "payload/etc/systemd/system/dmxwb.service": (
            repo_root / "deploy/dmxwb.service"
        ),
    }
    for web_name in ("index.html", "app.js", "model.js", "mqtt-client.js", "styles.css"):
        expected_sources[f"payload/var/www/dmxwb/{web_name}"] = (
            repo_root / "www/dmxwb" / web_name
        )
    for archive_name, source_path in expected_sources.items():
        if not source_path.is_file():
            fail(f"current source file is missing: {source_path}")
        if content[archive_name] != source_path.read_bytes():
            fail(f"archive does not contain the current {archive_name}")

    binary = content["payload/usr/local/bin/dmxwb"]
    if (
        len(binary) < 20
        or binary[:4] != b"\x7fELF"
        or binary[4] != 2
        or binary[5] != 1
        or int.from_bytes(binary[18:20], "little") != 183
    ):
        fail("production binary is not little-endian ELF64 AArch64")

    manifest = parse_manifest(content["MANIFEST.txt"])
    expected_manifest = {
        "bundle_format": "1",
        "bundle_kind": "production-offline",
        "product": "DMXWB",
        "application_version": expected_version,
        "source_id": expected_source_id,
        "target": "wb8-bullseye-arm64",
        "architecture": "AArch64",
    }
    for key, expected_value in expected_manifest.items():
        if one(manifest, key) != expected_value:
            fail(f"manifest {key!r} does not match {expected_value!r}")
    allowed_manifest_keys = {
        "bundle_format",
        "bundle_kind",
        "product",
        "application_version",
        "source_id",
        "target",
        "build_epoch",
        "generated_utc",
        "artifact_sha256",
        "architecture",
        "max_glibc",
        "dynamic_dependency",
    }
    if set(manifest) != allowed_manifest_keys:
        fail("manifest contains missing or unexpected fields")

    try:
        build_epoch = int(one(manifest, "build_epoch"))
    except ValueError:
        fail("manifest build_epoch is not an integer")
    expected_generated = datetime.fromtimestamp(
        build_epoch, tz=timezone.utc
    ).strftime("%Y-%m-%dT%H:%M:%SZ")
    if build_epoch < 0 or archive_mtimes != {build_epoch}:
        fail("archive timestamps do not match the recorded reproducible epoch")
    if one(manifest, "generated_utc") != expected_generated:
        fail("manifest generated_utc does not match build_epoch")

    binary_hash = hashlib.sha256(binary).hexdigest()
    if one(manifest, "artifact_sha256") != binary_hash:
        fail("manifest artifact SHA256 does not match payload binary")
    max_glibc = one(manifest, "max_glibc")
    if not re.fullmatch(r"GLIBC_[0-9]+(?:\.[0-9]+)*", max_glibc):
        fail("manifest max_glibc is malformed")
    glibc_version = tuple(
        int(part) for part in max_glibc.removeprefix("GLIBC_").split(".")
    )
    if glibc_version > (2, 31):
        fail("payload requires a GLIBC newer than Bullseye 2.31")
    dependencies = manifest.get("dynamic_dependency", [])
    if "libmosquitto.so.1" not in dependencies:
        fail("libmosquitto.so.1 dependency is not recorded")
    if any(name.startswith(("libstdc++", "libgcc_s")) for name in dependencies):
        fail("dynamic GNU C++ runtime must not be required")
    if dependencies != sorted(set(dependencies)):
        fail("dynamic dependencies must be unique and sorted")

    try:
        checksum_text = content["SHA256SUMS"].decode("ascii")
    except UnicodeDecodeError as error:
        fail(f"SHA256SUMS is not ASCII: {error}")
    checksum_entries: dict[str, str] = {}
    for line in checksum_text.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if match is None:
            fail(f"invalid SHA256SUMS line: {line!r}")
        digest, path = match.groups()
        if path in checksum_entries:
            fail(f"duplicate checksum path: {path}")
        checksum_entries[path] = digest
    expected_checksum_paths = set(EXPECTED_FILES) - {"SHA256SUMS"}
    if set(checksum_entries) != expected_checksum_paths:
        fail("SHA256SUMS does not cover exactly every non-checksum file")
    for path, expected_digest in checksum_entries.items():
        if hashlib.sha256(content[path]).hexdigest() != expected_digest:
            fail(f"checksum mismatch for {path}")

    try:
        config = json.loads(content["payload/etc/dmxwb/config.example.json"])
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"example config is invalid JSON: {error}")
    if config.get("fixtures", {}).get("count") != 0:
        fail("example config must start with zero Fixtures")

    forbidden_script_patterns = {
        r"\b(?:curl|wget|docker|npm)\b": "network/build command",
        r"\bgit\s+clone\b": "git clone",
        r"\bapt(?:-get)?\s+(?:update|install)\b": "online package operation",
        r"\b(?:cmake|g\+\+|c\+\+|make)\b": "target compilation command",
    }
    for script_name in CONTROL_FILES:
        script_text = content[script_name].decode("utf-8")
        for pattern, description in forbidden_script_patterns.items():
            if re.search(pattern, script_text, flags=re.IGNORECASE):
                fail(f"{script_name} contains forbidden {description}")

    print("dev012c4_exact_final_archive_layout: PASS")
    print("dev012c4_current_payload_and_control_scripts: PASS")
    print("dev012c4_manifest_artifact_identity: PASS")
    print("dev012c4_aarch64_bullseye_contract: PASS")
    print("dev012c4_complete_sha256_coverage: PASS")
    print("dev012c4_reproducible_timestamp_contract: PASS")
    print("dev012c4_no_target_build_or_online_operations: PASS")


if __name__ == "__main__":
    main()
