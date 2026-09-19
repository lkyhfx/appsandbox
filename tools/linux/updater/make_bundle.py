#!/usr/bin/env python3
"""Build an AppSandbox Guest Runtime Bundle.

The release key is supplied by CI as an Ed25519 PEM. The private key is never
copied into the guest or checked into the repository.
"""

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile


DISALLOWED_KERNEL_MARKERS = (
    "dxgkrnl",
    "asb_drm.ko",
    "modules/",
)

REQUIRED_RUNTIME = frozenset({
    "bin/appsandbox-agent", "bin/appsandbox-display", "bin/appsandbox-input",
    "bin/appsandbox-audio", "bin/appsandbox-clipboard",
    "systemd/appsandbox-agent.service", "systemd/appsandbox-display.service",
    "systemd/appsandbox-input.service", "systemd/appsandbox-audio.service",
})
REQUIRED_D3D12 = frozenset({
    "libexec/appsandbox-display-d3d12",
    "systemd/appsandbox-display-d3d12.service",
})


def validate_composition(kind: str, paths: set[str], graphics_version: str) -> None:
    if kind == "runtime":
        missing = REQUIRED_RUNTIME - paths
        if missing:
            raise ValueError(f"incomplete runtime bundle: {', '.join(sorted(missing))}")
        if paths & REQUIRED_D3D12 and not REQUIRED_D3D12 <= paths:
            raise ValueError("D3D12 binary and service must be supplied together")
    elif kind == "graphics":
        if not graphics_version or "graphics/wsl-mesa.tar.zst" not in paths:
            raise ValueError("graphics bundle needs a graphics version and Mesa archive")
        if any(p.startswith(("bin/", "libexec/", "systemd/")) for p in paths):
            raise ValueError("graphics bundle contains runtime executable or service")
        if any(not p.startswith(("graphics/", "config/", "gnome/")) for p in paths):
            raise ValueError("graphics bundle contains unsupported payload")
    else:
        raise ValueError(f"unsupported bundle kind: {kind}")
    if graphics_version and "graphics/wsl-mesa.tar.zst" not in paths:
        raise ValueError("graphics version declared without Mesa archive")
    if "graphics/wsl-mesa.tar.zst" in paths and not graphics_version:
        raise ValueError("Mesa archive needs a graphics version")


def safe_rel(path: pathlib.Path) -> str:
    value = path.as_posix()
    if value.startswith("/") or ".." in path.parts or any(p in ("", ".") for p in path.parts):
        raise ValueError(f"unsafe payload path: {value}")
    return value


def validate_runtime_path(rel: str) -> None:
    lowered = rel.lower()
    if any(marker in lowered for marker in DISALLOWED_KERNEL_MARKERS):
        raise ValueError(f"kernel component is not allowed in a runtime bundle: {rel}")


def semver(value: str) -> str:
    pattern = r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
    if not re.fullmatch(pattern, value):
        raise ValueError(f"version must be MAJOR.MINOR.PATCH: {value}")
    return value


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--payload", type=pathlib.Path, required=True)
    ap.add_argument("--version", required=True, type=semver)
    ap.add_argument("--kind", required=True, choices=("runtime", "graphics"))
    ap.add_argument("--commit", required=True)
    ap.add_argument("--graphics-version", default="", type=lambda v: semver(v) if v else "")
    ap.add_argument("--arch", default="amd64", choices=("amd64",))
    ap.add_argument("--os", default="ubuntu-26.04", choices=("ubuntu-26.04",))
    ap.add_argument("--allow-downgrade", action="store_true")
    ap.add_argument("--signing-key", type=pathlib.Path, required=True)
    ap.add_argument("--output", type=pathlib.Path, required=True)
    ap.add_argument("--reboot-required", action="store_true")
    args = ap.parse_args()

    files = []
    for source in sorted(args.payload.rglob("*")):
        if source.is_symlink():
            raise ValueError(f"symlink payload is not allowed: {source}")
        if not source.is_file():
            continue
        rel = safe_rel(source.relative_to(args.payload))
        validate_runtime_path(rel)
        data = source.read_bytes()
        component = rel.split("/", 1)[0]
        files.append({
            "path": rel,
            "sha256": hashlib.sha256(data).hexdigest(),
            "size": len(data),
            "mode": source.stat().st_mode & 0o7777,
            "component": component,
        })
    if not files:
        raise SystemExit("payload is empty")
    validate_composition(args.kind, {item["path"] for item in files}, args.graphics_version)

    manifest = {
        "schema": 1,
        "kind": args.kind,
        "version": args.version,
        "commit": args.commit,
        "arch": args.arch,
        "os": args.os,
        "host_protocol_min": 1,
        "host_protocol_max": 1,
        "updater_min_version": "1.0.0",
        "graphics_version": args.graphics_version,
        "reboot_required": bool(args.reboot_required),
        "allow_downgrade": bool(args.allow_downgrade),
        "kernel_components_present": False,
        "files": files,
    }
    manifest_bytes = (json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n").encode()

    with tempfile.TemporaryDirectory(prefix="appsandbox-bundle-") as temp:
        root = pathlib.Path(temp)
        (root / "payload").mkdir()
        for item in files:
            target = root / "payload" / item["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(args.payload / item["path"], target)
            os.chmod(target, item["mode"])
        (root / "manifest.json").write_bytes(manifest_bytes)
        signature = root / "manifest.sig"
        signed_path = root / "manifest-to-sign.json"
        signed_path.write_bytes(manifest_bytes)
        try:
            with signature.open("wb") as output:
                subprocess.run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(args.signing_key),
                                "-in", str(signed_path)], check=True, stdout=output)
        finally:
            signed_path.unlink(missing_ok=True)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        tar_path = root / "bundle.tar"
        subprocess.run(["tar", "--format=ustar", "--owner=0", "--group=0",
                        "--numeric-owner", "-C", str(root), "-cf", str(tar_path),
                        "manifest.json", "manifest.sig", "payload"], check=True)
        with args.output.open("wb") as output:
            subprocess.run(["zstd", "--quiet", "--no-progress", "-T0", "-c", str(tar_path)],
                           check=True, stdout=output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
