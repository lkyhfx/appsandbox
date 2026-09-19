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
import shutil
import subprocess
import tempfile


def safe_rel(path: pathlib.Path) -> str:
    value = path.as_posix()
    if value.startswith("/") or ".." in path.parts or any(p in ("", ".") for p in path.parts):
        raise ValueError(f"unsafe payload path: {value}")
    return value


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--payload", type=pathlib.Path, required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--commit", required=True)
    ap.add_argument("--graphics-version", default="")
    ap.add_argument("--signing-key", type=pathlib.Path, required=True)
    ap.add_argument("--output", type=pathlib.Path, required=True)
    ap.add_argument("--reboot-required", action="store_true")
    args = ap.parse_args()

    files = []
    for source in sorted(args.payload.rglob("*")):
        if not source.is_file():
            continue
        rel = safe_rel(source.relative_to(args.payload))
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

    manifest = {
        "schema": 1,
        "version": args.version,
        "commit": args.commit,
        "arch": "amd64",
        "os": "ubuntu-26.04",
        "host_protocol_min": 1,
        "host_protocol_max": 1,
        "updater_min_version": "1.0.0",
        "graphics_version": args.graphics_version,
        "reboot_required": bool(args.reboot_required),
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
        with tempfile.NamedTemporaryFile() as signed:
            signed.write(manifest_bytes)
            signed.flush()
            with signature.open("wb") as output:
                subprocess.run(["openssl", "pkeyutl", "-sign", "-inkey", str(args.signing_key),
                                "-in", signed.name], check=True, stdout=output)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(suffix=".tar") as tar:
            subprocess.run(["tar", "--format=ustar", "--sort=name", "--owner=0", "--group=0",
                            "--numeric-owner", "-C", str(root), "-cf", tar.name,
                            "manifest.json", "manifest.sig", "payload"], check=True)
            with args.output.open("wb") as output:
                subprocess.run(["zstd", "--quiet", "--no-progress", "-T0", "-c", tar.name],
                               check=True, stdout=output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
