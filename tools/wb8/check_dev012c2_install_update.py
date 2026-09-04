#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path


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
    raise SystemExit(f"DEV-012C2 install/update regression FAIL: {message}")


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


def run_installer(
    bundle: Path,
    destination_root: Path,
    *,
    expected_success: bool,
    failure_stage: str | None = None,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["DESTDIR"] = str(destination_root)
    if failure_stage is not None:
        environment["DMXWB_INSTALL_TEST_FAIL_AFTER"] = failure_stage
    result = subprocess.run(
        ["bash", str(bundle / "install.sh")],
        cwd=bundle,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if (result.returncode == 0) != expected_success:
        fail(
            f"installer returned {result.returncode}, expected success={expected_success}:\n"
            f"{result.stdout}"
        )
    return result


def snapshot_managed(root: Path) -> dict[str, str]:
    return {relative: digest(root / relative) for relative in MANAGED_PATHS}


def require_layout(root: Path) -> None:
    for relative in MANAGED_PATHS:
        path = root / relative
        if not path.is_file() or path.is_symlink():
            fail(f"managed file is missing or unsafe: {relative}")
    expected_modes = {MANAGED_PATHS[0]: 0o755}
    for relative in MANAGED_PATHS[1:]:
        expected_modes[relative] = 0o644
    for relative, expected_mode in expected_modes.items():
        actual_mode = stat.S_IMODE((root / relative).stat().st_mode)
        if actual_mode != expected_mode:
            fail(f"{relative} mode is {actual_mode:o}, expected {expected_mode:o}")
    if not (root / "etc/dmxwb/config.json").is_file():
        fail("first installation did not create config.json")
    if not (root / "var/lib/dmxwb").is_dir():
        fail("first installation did not create the state directory")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(
            "Usage: check_dev012c2_install_update.py PAYLOAD_ARCHIVE INSTALLER"
        )
    archive = Path(sys.argv[1]).resolve()
    installer = Path(sys.argv[2]).resolve()

    installer_text = installer.read_text(encoding="utf-8")
    forbidden_patterns = (
        r"\b(?:curl|wget|docker|npm)\b",
        r"\bgit\s+clone\b",
        r"\bapt(?:-get)?\s+(?:update|install)\b",
    )
    for pattern in forbidden_patterns:
        if re.search(pattern, installer_text, flags=re.IGNORECASE):
            fail(f"installer contains forbidden operation matching {pattern!r}")

    with tempfile.TemporaryDirectory(prefix="dmxwb-dev012c2-") as temporary:
        temp = Path(temporary)
        bundle_v1 = prepare_bundle(archive, installer, temp / "bundle-v1")
        install_root = temp / "target-root"

        first = run_installer(
            bundle_v1,
            install_root,
            expected_success=True,
        )
        if "fresh installation completed" not in first.stdout:
            fail("first install did not report fresh mode")
        require_layout(install_root)

        config_path = install_root / "etc/dmxwb/config.json"
        state_path = install_root / "var/lib/dmxwb/state.json"
        config_bytes = b'{"user":"preserve-config"}\n'
        state_bytes = b'{"user":"preserve-state"}\n'
        config_path.write_bytes(config_bytes)
        state_path.write_bytes(state_bytes)

        bundle_v2 = prepare_bundle(archive, installer, temp / "bundle-v2")
        app_v2 = bundle_v2 / "payload/var/www/dmxwb/app.js"
        app_v2.write_bytes(app_v2.read_bytes() + b"\n// DEV-012C2 update payload\n")
        rewrite_checksums(bundle_v2)
        update = run_installer(
            bundle_v2,
            install_root,
            expected_success=True,
        )
        if "update installation completed" not in update.stdout:
            fail("repeat install did not report update mode")
        if not (install_root / "var/www/dmxwb/app.js").read_bytes().endswith(
            b"// DEV-012C2 update payload\n"
        ):
            fail("update did not replace the managed Web payload")
        if config_path.read_bytes() != config_bytes or state_path.read_bytes() != state_bytes:
            fail("update changed user config/state bytes")
        stable_snapshot = snapshot_managed(install_root)

        incompatible_bundle = prepare_bundle(
            archive, installer, temp / "bundle-incompatible"
        )
        incompatible_manifest = incompatible_bundle / "MANIFEST.txt"
        incompatible_manifest.write_text(
            incompatible_manifest.read_text(encoding="utf-8").replace(
                "target=wb8-bullseye-arm64", "target=unsupported-target"
            ),
            encoding="utf-8",
        )
        rewrite_checksums(incompatible_bundle)
        run_installer(
            incompatible_bundle,
            install_root,
            expected_success=False,
        )
        if snapshot_managed(install_root) != stable_snapshot:
            fail("incompatible manifest changed installed managed files")

        corrupt_bundle = prepare_bundle(archive, installer, temp / "bundle-corrupt")
        corrupt_styles = corrupt_bundle / "payload/var/www/dmxwb/styles.css"
        corrupt_styles.write_bytes(corrupt_styles.read_bytes() + b"\ncorrupt\n")
        run_installer(
            corrupt_bundle,
            install_root,
            expected_success=False,
        )
        if snapshot_managed(install_root) != stable_snapshot:
            fail("checksum failure changed installed managed files")

        rollback_bundle = prepare_bundle(archive, installer, temp / "bundle-rollback")
        rollback_app = rollback_bundle / "payload/var/www/dmxwb/app.js"
        rollback_app.write_bytes(rollback_app.read_bytes() + b"\n// must rollback\n")
        rewrite_checksums(rollback_bundle)
        run_installer(
            rollback_bundle,
            install_root,
            expected_success=False,
            failure_stage="web",
        )
        if snapshot_managed(install_root) != stable_snapshot:
            fail("injected update failure did not restore managed files")
        if config_path.read_bytes() != config_bytes or state_path.read_bytes() != state_bytes:
            fail("failed update changed user config/state bytes")

        run_installer(
            bundle_v2,
            install_root,
            expected_success=True,
        )
        if snapshot_managed(install_root) != stable_snapshot:
            fail("idempotent repeat update changed the installed payload")

    print("dev012c2_fresh_install_layout: PASS")
    print("dev012c2_update_replaces_managed_payload: PASS")
    print("dev012c2_config_state_byte_preservation: PASS")
    print("dev012c2_checksum_preflight_no_mutation: PASS")
    print("dev012c2_incompatible_manifest_no_mutation: PASS")
    print("dev012c2_failed_update_rollback: PASS")
    print("dev012c2_idempotent_repeat_update: PASS")
    print("dev012c2_no_online_or_target_build_operations: PASS")


if __name__ == "__main__":
    main()
