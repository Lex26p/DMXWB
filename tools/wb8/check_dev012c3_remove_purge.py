#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path
from typing import Optional, Sequence


ROOT_NAME = "dmxwb-wb8-bullseye-arm64"
MANAGED_PATHS = (
    "usr/local/bin/dmxwb",
    "etc/systemd/system/dmxwb.service",
    "var/www/dmxwb/index.html",
    "var/www/dmxwb/app.js",
    "var/www/dmxwb/model.js",
    "var/www/dmxwb/mqtt-client.js",
    "var/www/dmxwb/styles.css",
)


def fail(message: str) -> None:
    raise SystemExit(f"DEV-012C3 remove/purge regression FAIL: {message}")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rewrite_checksums(bundle: Path) -> None:
    checksum_path = bundle / "SHA256SUMS"
    files = sorted(
        path for path in bundle.rglob("*")
        if path.is_file() and path != checksum_path
    )
    checksum_path.write_text(
        "".join(
            f"{digest(path)}  {path.relative_to(bundle).as_posix()}\n"
            for path in files
        ),
        encoding="ascii",
    )


def prepare_bundle(archive: Path, installer: Path, destination: Path) -> Path:
    destination.mkdir()
    with tarfile.open(archive, "r:gz") as source:
        source.extractall(destination)
    bundle = destination / ROOT_NAME
    shutil.copy2(installer, bundle / "install.sh")
    (bundle / "install.sh").chmod(0o755)
    rewrite_checksums(bundle)
    return bundle


def run_script(
    script: Path,
    destination_root: Path,
    arguments: Sequence[str] = (),
    *,
    expected_success: bool,
    failure_stage: Optional[str] = None,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["DESTDIR"] = str(destination_root)
    if failure_stage is not None:
        environment["DMXWB_UNINSTALL_TEST_FAIL_AFTER"] = failure_stage
    result = subprocess.run(
        ["bash", str(script), *arguments],
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if (result.returncode == 0) != expected_success:
        fail(
            f"{script.name} returned {result.returncode}, "
            f"expected success={expected_success}:\n{result.stdout}"
        )
    return result


def install_fixture(bundle: Path, destination_root: Path) -> None:
    run_script(
        bundle / "install.sh",
        destination_root,
        expected_success=True,
    )
    for relative in MANAGED_PATHS:
        if not (destination_root / relative).is_file():
            fail(f"install fixture is missing {relative}")


def managed_snapshot(destination_root: Path) -> dict[str, str]:
    return {
        relative: digest(destination_root / relative)
        for relative in MANAGED_PATHS
    }


def assert_managed_absent(destination_root: Path) -> None:
    remaining = [
        relative for relative in MANAGED_PATHS
        if (destination_root / relative).exists()
        or (destination_root / relative).is_symlink()
    ]
    if remaining:
        fail(f"managed files remain after removal: {remaining}")


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit(
            "Usage: check_dev012c3_remove_purge.py "
            "PAYLOAD_ARCHIVE INSTALLER UNINSTALLER"
        )
    archive = Path(sys.argv[1]).resolve()
    installer = Path(sys.argv[2]).resolve()
    uninstaller = Path(sys.argv[3]).resolve()

    uninstaller_text = uninstaller.read_text(encoding="utf-8")
    forbidden_patterns = (
        r"\b(?:curl|wget|docker|npm)\b",
        r"\bgit\s+clone\b",
        r"\bapt(?:-get)?\s+(?:update|install)\b",
        r"rm\s+-[^\n]*[rR][^\n]*\s+['\"]?(?:/|\$\{DEST_ROOT\})['\"]?(?:\s|$)",
    )
    for pattern in forbidden_patterns:
        if re.search(pattern, uninstaller_text, flags=re.IGNORECASE):
            fail(f"uninstaller contains forbidden operation matching {pattern!r}")

    with tempfile.TemporaryDirectory(prefix="dmxwb-dev012c3-") as temporary:
        temp = Path(temporary)
        bundle = prepare_bundle(archive, installer, temp / "bundle")
        install_root = temp / "target-root"
        install_fixture(bundle, install_root)

        config_path = install_root / "etc/dmxwb/config.json"
        state_path = install_root / "var/lib/dmxwb/state.json"
        config_bytes = b'{"user":"preserve-config"}\n'
        state_bytes = b'{"user":"preserve-state"}\n'
        config_path.write_bytes(config_bytes)
        state_path.write_bytes(state_bytes)

        foreign_web = install_root / "var/www/dmxwb/user-owned.txt"
        foreign_config = install_root / "etc/dmxwb/user-owned.txt"
        foreign_state = install_root / "var/lib/dmxwb/user-owned.txt"
        foreign_web.write_bytes(b"foreign web\n")
        foreign_config.write_bytes(b"foreign config\n")
        foreign_state.write_bytes(b"foreign state\n")

        before_rollback = managed_snapshot(install_root)
        run_script(
            uninstaller,
            install_root,
            expected_success=False,
            failure_stage="managed",
        )
        if managed_snapshot(install_root) != before_rollback:
            fail("failed removal did not restore all managed files")
        if config_path.read_bytes() != config_bytes or state_path.read_bytes() != state_bytes:
            fail("failed removal changed config/state")

        normal = run_script(
            uninstaller,
            install_root,
            expected_success=True,
        )
        if "config and state were preserved" not in normal.stdout:
            fail("normal removal did not report data preservation")
        assert_managed_absent(install_root)
        if config_path.read_bytes() != config_bytes or state_path.read_bytes() != state_bytes:
            fail("normal removal changed config/state bytes")
        for path in (foreign_web, foreign_config, foreign_state):
            if not path.is_file():
                fail(f"normal removal deleted foreign file: {path}")

        run_script(
            uninstaller,
            install_root,
            expected_success=True,
        )
        assert_managed_absent(install_root)
        if config_path.read_bytes() != config_bytes or state_path.read_bytes() != state_bytes:
            fail("idempotent normal removal changed config/state bytes")

        purged = run_script(
            uninstaller,
            install_root,
            ("--purge",),
            expected_success=True,
        )
        if "user data purge completed" not in purged.stdout:
            fail("explicit purge did not report completion")
        if config_path.exists() or state_path.exists():
            fail("explicit purge did not remove config/state")
        for path in (foreign_web, foreign_config, foreign_state):
            if not path.is_file():
                fail(f"purge deleted foreign file: {path}")
        run_script(
            uninstaller,
            install_root,
            ("--purge",),
            expected_success=True,
        )

        unsafe_root = temp / "unsafe-target-root"
        install_fixture(bundle, unsafe_root)
        sentinel = temp / "sentinel.txt"
        sentinel.write_bytes(b"do not change\n")
        unsafe_app = unsafe_root / "var/www/dmxwb/app.js"
        unsafe_app.unlink()
        unsafe_app.symlink_to(sentinel)
        run_script(
            uninstaller,
            unsafe_root,
            expected_success=False,
        )
        if not unsafe_app.is_symlink() or sentinel.read_bytes() != b"do not change\n":
            fail("symbolic-link target rejection changed external data")
        if not (unsafe_root / "usr/local/bin/dmxwb").is_file():
            fail("symbolic-link rejection mutated other managed files")

        parent_link_root = temp / "parent-link-root"
        parent_link_root.mkdir()
        outside_var = temp / "outside-var"
        outside_web = outside_var / "www/dmxwb"
        outside_web.mkdir(parents=True)
        outside_sentinel = outside_web / "app.js"
        outside_sentinel.write_bytes(b"outside\n")
        (parent_link_root / "var").symlink_to(outside_var, target_is_directory=True)
        run_script(
            uninstaller,
            parent_link_root,
            expected_success=False,
        )
        if outside_sentinel.read_bytes() != b"outside\n":
            fail("symbolic-link parent rejection changed external data")

        run_script(
            uninstaller,
            Path("/"),
            expected_success=False,
        )

    print("dev012c3_failed_removal_rollback: PASS")
    print("dev012c3_normal_removal_exact_managed_files: PASS")
    print("dev012c3_normal_removal_preserves_config_state: PASS")
    print("dev012c3_normal_removal_idempotent: PASS")
    print("dev012c3_explicit_purge_only_known_user_data: PASS")
    print("dev012c3_explicit_purge_idempotent: PASS")
    print("dev012c3_symlink_target_refusal: PASS")
    print("dev012c3_symlink_parent_refusal: PASS")
    print("dev012c3_root_destdir_refusal: PASS")


if __name__ == "__main__":
    main()
