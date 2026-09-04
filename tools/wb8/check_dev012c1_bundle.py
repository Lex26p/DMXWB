#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import re
import sys
import tarfile
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
METADATA_FILES = {"MANIFEST.txt": 0o644, "SHA256SUMS": 0o644}
EXPECTED_FILES = PAYLOAD_FILES | METADATA_FILES
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
    raise SystemExit(f"DEV-012C1 bundle contract FAIL: {message}")


def read_file(archive: tarfile.TarFile, member: tarfile.TarInfo) -> bytes:
    stream = archive.extractfile(member)
    if stream is None:
        fail(f"cannot read {member.name}")
    return stream.read()


def parse_manifest(data: bytes) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        fail(f"MANIFEST.txt is not UTF-8: {error}")
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
    if len(sys.argv) != 3:
        raise SystemExit(
            "Usage: check_dev012c1_bundle.py ARCHIVE EXPECTED_SOURCE_ID"
        )
    archive_path, expected_source_id = sys.argv[1:]

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
                f"unexpected directory set; missing={sorted(EXPECTED_DIRECTORIES - directories)}, "
                f"extra={sorted(directories - EXPECTED_DIRECTORIES)}"
            )

        content: dict[str, bytes] = {}
        for path, expected_mode in EXPECTED_FILES.items():
            member = members[path]
            if member.mode & 0o777 != expected_mode:
                fail(
                    f"{path} mode is {member.mode & 0o777:o}, expected {expected_mode:o}"
                )
            if member.uid != 0 or member.gid != 0:
                fail(f"{path} archive owner must be root:root")
            content[path] = read_file(archive, member)

    if not content["payload/usr/local/bin/dmxwb"].startswith(b"\x7fELF"):
        fail("production binary is not ELF")

    manifest = parse_manifest(content["MANIFEST.txt"])
    expected_manifest = {
        "bundle_format": "1",
        "bundle_kind": "dev012c1-payload",
        "product": "DMXWB",
        "target": "wb8-bullseye-arm64",
        "architecture": "AArch64",
        "source_id": expected_source_id,
    }
    for key, expected in expected_manifest.items():
        if one(manifest, key) != expected:
            fail(f"manifest {key!r} does not match {expected!r}")
    for key in (
        "application_version",
        "build_epoch",
        "generated_utc",
        "artifact_sha256",
        "max_glibc",
    ):
        one(manifest, key)
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
    if build_epoch < 0 or archive_mtimes != {build_epoch}:
        fail("archive timestamps do not match the recorded reproducible epoch")

    artifact_hash = hashlib.sha256(
        content["payload/usr/local/bin/dmxwb"]
    ).hexdigest()
    if one(manifest, "artifact_sha256") != artifact_hash:
        fail("manifest artifact SHA256 does not match payload binary")
    max_glibc = one(manifest, "max_glibc")
    if not re.fullmatch(r"GLIBC_[0-9]+(?:\.[0-9]+)*", max_glibc):
        fail("manifest max_glibc is malformed")
    glibc_version = tuple(int(part) for part in max_glibc.removeprefix("GLIBC_").split("."))
    if glibc_version > (2, 31):
        fail("payload requires a GLIBC newer than Bullseye 2.31")

    dependencies = manifest.get("dynamic_dependency", [])
    if "libmosquitto.so.1" not in dependencies:
        fail("libmosquitto.so.1 dependency is not recorded")
    if any(name.startswith(("libstdc++", "libgcc_s")) for name in dependencies):
        fail("dynamic GNU C++ runtime must not be required")
    if dependencies != sorted(set(dependencies)):
        fail("dynamic dependencies must be unique and sorted")

    checksum_entries: dict[str, str] = {}
    try:
        checksum_text = content["SHA256SUMS"].decode("ascii")
    except UnicodeDecodeError as error:
        fail(f"SHA256SUMS is not ASCII: {error}")
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
        actual_digest = hashlib.sha256(content[path]).hexdigest()
        if actual_digest != expected_digest:
            fail(f"checksum mismatch for {path}")

    try:
        config = json.loads(content["payload/etc/dmxwb/config.example.json"])
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"example config is invalid JSON: {error}")
    expected_config = {
        "version": 1,
        "revision": 0,
        "dmx": {"port": "/dev/ttyRS485-1"},
        "artnet": {"universe": 0},
        "fixtures": {"count": 0, "start_address": 1, "items": []},
        "groups": [],
        "scenes": [],
        "id_counters": {
            "next_fixture_id": 1,
            "next_group_id": 1,
            "next_scene_id": 1,
        },
    }
    if config != expected_config:
        fail("example config does not match the application defaults")

    forbidden_parts = {"src", "include", "tests", "build", ".git", "node_modules"}
    for path in files:
        if forbidden_parts.intersection(PurePosixPath(path).parts):
            fail(f"development content leaked into payload: {path}")

    builder_path = Path(__file__).with_name("build_dev012c1_offline_payload.sh")
    builder_text = builder_path.read_text(encoding="utf-8")
    forbidden_builder_patterns = {
        r"\b(?:curl|wget|docker|npm)\b": "network/build command",
        r"\bgit\s+clone\b": "git clone",
        r"\bapt(?:-get)?\s+(?:update|install)\b": "online package operation",
    }
    for pattern, description in forbidden_builder_patterns.items():
        if re.search(pattern, builder_text, flags=re.IGNORECASE):
            fail(f"builder contains forbidden {description}")

    print("dev012c1_archive_layout: PASS")
    print("dev012c1_manifest_identity: PASS")
    print("dev012c1_sha256_coverage: PASS")
    print("dev012c1_default_config: PASS")
    print("dev012c1_payload_modes_and_safety: PASS")
    print("dev012c1_offline_builder_contract: PASS")


if __name__ == "__main__":
    main()
