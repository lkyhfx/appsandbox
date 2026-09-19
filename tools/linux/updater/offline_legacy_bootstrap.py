#!/usr/bin/env python3
"""Populate the updater layout in a stopped Linux VM filesystem.

The caller is responsible for opening a VHDX read/write only after the VM is
stopped and for unmounting it after this command returns. This command never
executes guest commands; it performs a staged filesystem transaction rooted at
the supplied mount point.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import tempfile


BINS = ("appsandbox-agent", "appsandbox-display", "appsandbox-input",
        "appsandbox-audio", "appsandbox-clipboard")
UNITS = ("appsandbox-agent.service", "appsandbox-display.service",
         "appsandbox-input.service", "appsandbox-audio.service",
         "appsandbox-display-d3d12.service")


def copy_if_present(src: pathlib.Path, dst: pathlib.Path, mode: int) -> bool:
    if not src.exists() or src.is_symlink():
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    os.chmod(dst, mode)
    return True


def migrate(root: pathlib.Path, updater: pathlib.Path, service: pathlib.Path,
            watch: pathlib.Path) -> None:
    guest = root / "opt/appsandbox/guest"
    current = guest / "current"
    if current.exists() or current.is_symlink():
        raise RuntimeError("guest runtime layout already exists")
    if (guest / "releases/legacy").exists():
        raise RuntimeError("legacy release already exists")
    if not (root / "usr/local/bin/appsandbox-agent").is_file():
        raise RuntimeError("legacy agent is missing")
    if not updater.is_file() or not service.is_file() or not watch.is_file():
        raise RuntimeError("pinned updater artifacts are missing")

    guest.mkdir(parents=True, exist_ok=True)
    (guest / "releases").mkdir(parents=True, exist_ok=True)
    staging = pathlib.Path(tempfile.mkdtemp(prefix="legacy-", dir=guest))
    release = staging / "legacy"
    try:
        for name in ("bin", "libexec", "systemd", "gnome", "config"):
            (release / name).mkdir(parents=True, exist_ok=True)
        for name in BINS:
            copy_if_present(root / "usr/local/bin" / name, release / "bin" / name, 0o755)
        copy_if_present(root / "usr/local/libexec/appsandbox-display-d3d12",
                        release / "libexec/appsandbox-display-d3d12", 0o755)
        for name in UNITS:
            source = root / "etc/systemd/system" / name
            copy_if_present(source, release / "systemd" / name, 0o644)
        copy_if_present(root / "etc/modprobe.d/asb_drm.conf",
                        release / "config/asb_drm.conf", 0o644)
        old_mesa = root / "opt/wsl-mesa"
        if old_mesa.is_dir() and not old_mesa.is_symlink():
            shutil.copytree(old_mesa, release / "graphics/legacy-mesa",
                            symlinks=True, dirs_exist_ok=True)
        copy_if_present(updater, release / "libexec/appsandbox-guest-updater", 0o755)
        copy_if_present(service, release / "systemd/appsandbox-guest-updater.service", 0o644)
        copy_if_present(watch, release / "systemd/appsandbox-guest-update-watch.service", 0o644)
        mesa_marker = root / "opt/wsl-mesa/current/GRAPHICS"
        graphics = mesa_marker.read_text(errors="replace").strip() if mesa_marker.is_file() else "0.0.0"
        if not graphics:
            graphics = "0.0.0"
        (release / "RELEASE").write_text(
            f"version=0.0.0\ngraphics_version={graphics}\ncommit=legacy-bootstrap\n",
            encoding="utf-8")
        os.replace(release, guest / "releases/legacy")
        current.parent.mkdir(parents=True, exist_ok=True)
        os.symlink("releases/legacy", current)
        for name in BINS:
            target = guest / "current/bin" / name
            if target.exists():
                link = root / "usr/local/bin" / name
                if link.exists() or link.is_symlink():
                    link.unlink()
                link.parent.mkdir(parents=True, exist_ok=True)
                os.symlink("/opt/appsandbox/guest/current/bin/" + name, link)
        libexec = guest / "current/libexec/appsandbox-display-d3d12"
        link = root / "usr/local/libexec/appsandbox-display-d3d12"
        if libexec.exists():
            if link.exists() or link.is_symlink():
                link.unlink()
            link.parent.mkdir(parents=True, exist_ok=True)
            os.symlink("/opt/appsandbox/guest/current/libexec/appsandbox-display-d3d12", link)
        systemd = root / "etc/systemd/system"
        systemd.mkdir(parents=True, exist_ok=True)
        for name in ("appsandbox-guest-updater.service", "appsandbox-guest-update-watch.service"):
            link = systemd / name
            if link.exists() or link.is_symlink():
                link.unlink()
            os.symlink("/opt/appsandbox/guest/current/systemd/" + name, link)
        wants = systemd / "multi-user.target.wants"
        wants.mkdir(parents=True, exist_ok=True)
        for name in ("appsandbox-guest-updater.service", "appsandbox-guest-update-watch.service"):
            link = wants / name
            if link.exists() or link.is_symlink():
                link.unlink()
            os.symlink("../" + name, link)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    finally:
        if staging.exists():
            shutil.rmtree(staging, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=pathlib.Path, required=True)
    ap.add_argument("--updater", type=pathlib.Path, required=True)
    ap.add_argument("--service", type=pathlib.Path, required=True)
    ap.add_argument("--watch", type=pathlib.Path, required=True)
    args = ap.parse_args()
    try:
        migrate(args.root, args.updater, args.service, args.watch)
    except (OSError, RuntimeError) as exc:
        print(f"legacy bootstrap rejected: {exc}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
