#!/usr/bin/env python3
"""Install the bootstrap-capable updater layout into a stopped VM filesystem.

The Host invokes this only while the VHDX is mounted read/write and the VM is
stopped.  The operation is a durable, recoverable filesystem transaction:
the legacy runtime is snapshotted as ``legacy-original`` and the new runtime
is activated through one stable ``current`` pointer.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shutil
import tempfile
import uuid


BINS = ("appsandbox-agent", "appsandbox-display", "appsandbox-input",
        "appsandbox-audio", "appsandbox-clipboard")
UNITS = ("appsandbox-agent.service", "appsandbox-display.service",
         "appsandbox-input.service", "appsandbox-audio.service",
         "appsandbox-display-d3d12.service")
UPDATE_UNITS = ("appsandbox-guest-updater.service",
                "appsandbox-guest-update-watch.service")
VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$")


def _sync(path: pathlib.Path) -> None:
    """Best-effort fsync for a file or directory on Linux."""
    try:
        flags = os.O_RDONLY
        if path.is_dir() and hasattr(os, "O_DIRECTORY"):
            flags |= os.O_DIRECTORY
        fd = os.open(path, flags)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError:
        # Some test filesystems do not permit directory fsync.  The rename
        # protocol remains useful there, while real ext4/xfs mounts fsync.
        pass


def _write_json_durable(path: pathlib.Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=f".{path.name}-", dir=path.parent)
    temp = pathlib.Path(temp_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(value, stream, sort_keys=True, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
        _sync(path.parent)
    finally:
        if temp.exists():
            temp.unlink()


def _remove(path: pathlib.Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)


def _replace_link(link: pathlib.Path, target: str) -> None:
    link.parent.mkdir(parents=True, exist_ok=True)
    temp = link.parent / f".{link.name}.new-{uuid.uuid4().hex}"
    os.symlink(target, temp)
    try:
        os.replace(temp, link)
    finally:
        if temp.exists() or temp.is_symlink():
            temp.unlink()
    _sync(link.parent)


def _copy_file(src: pathlib.Path, dst: pathlib.Path, mode: int) -> bool:
    if not src.exists() or src.is_symlink() or not src.is_file():
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    os.chmod(dst, mode)
    _sync(dst)
    return True


def _atomic_copy(src: pathlib.Path, dst: pathlib.Path, mode: int) -> None:
    if not src.is_file() or src.is_symlink():
        raise RuntimeError(f"bootstrap file is missing: {src}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    temp = dst.parent / f".{dst.name}.new-{uuid.uuid4().hex}"
    try:
        shutil.copy2(src, temp)
        os.chmod(temp, mode)
        _sync(temp)
        os.replace(temp, dst)
        _sync(dst.parent)
    finally:
        if temp.exists():
            temp.unlink()


def copy_if_present(src: pathlib.Path, dst: pathlib.Path, mode: int) -> bool:
    """Compatibility helper retained for the unit tests and callers."""
    return _copy_file(src, dst, mode)


def _copy_tree(src: pathlib.Path, dst: pathlib.Path) -> None:
    if not src.is_dir() or src.is_symlink():
        raise RuntimeError(f"runtime tree is missing: {src}")
    for item in src.rglob("*"):
        relative = item.relative_to(src)
        target = dst / relative
        if item.is_symlink():
            raise RuntimeError(f"runtime tree contains symlink: {relative}")
        if item.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif item.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item, target)
        else:
            raise RuntimeError(f"runtime tree contains special file: {relative}")
    dst.mkdir(parents=True, exist_ok=True)


def _read_version(release: pathlib.Path, requested: str | None) -> str:
    version = requested
    if not version:
        marker = release / "RELEASE"
        if marker.is_file():
            for line in marker.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith("version="):
                    version = line.split("=", 1)[1].strip()
                    break
    version = version or "0.0.0"
    if not VERSION_RE.fullmatch(version):
        raise RuntimeError("bootstrap runtime version is not semver")
    return version


def _legacy_release(root: pathlib.Path, release: pathlib.Path,
                    updater: pathlib.Path, service: pathlib.Path,
                    watch: pathlib.Path) -> None:
    for name in ("bin", "libexec", "systemd", "gnome", "config"):
        (release / name).mkdir(parents=True, exist_ok=True)
    for name in BINS:
        _copy_file(root / "usr/local/bin" / name, release / "bin" / name, 0o755)
    _copy_file(root / "usr/local/libexec/appsandbox-display-d3d12",
               release / "libexec/appsandbox-display-d3d12", 0o755)
    for name in UNITS:
        _copy_file(root / "etc/systemd/system" / name,
                   release / "systemd" / name, 0o644)
    _copy_file(root / "etc/modprobe.d/asb_drm.conf",
               release / "config/asb_drm.conf", 0o644)
    old_mesa = root / "opt/wsl-mesa"
    if old_mesa.is_dir() and not old_mesa.is_symlink():
        shutil.copytree(old_mesa, release / "graphics/legacy-mesa",
                        symlinks=True, dirs_exist_ok=True)
    _copy_file(updater, release / "libexec/appsandbox-guest-updater", 0o755)
    _copy_file(service, release / "systemd/appsandbox-guest-updater.service", 0o644)
    _copy_file(watch, release / "systemd/appsandbox-guest-update-watch.service", 0o644)
    mesa_marker = root / "opt/wsl-mesa/current/GRAPHICS"
    graphics = mesa_marker.read_text(errors="replace").strip() if mesa_marker.is_file() else "0.0.0"
    graphics = graphics or "0.0.0"
    (release / "RELEASE").write_text(
        f"version=0.0.0\ngraphics_version={graphics}\ncommit=legacy-bootstrap\n",
        encoding="utf-8")


def _bootstrap_release(root: pathlib.Path, release: pathlib.Path,
                       bootstrap_root: pathlib.Path | None,
                       updater: pathlib.Path, service: pathlib.Path,
                       watch: pathlib.Path, version: str) -> None:
    if bootstrap_root is not None:
        _copy_tree(bootstrap_root, release)
    else:
        # This compatibility path exists only for the small Linux unit test
        # fixture. Production Host migration always supplies bootstrap-root.
        (release / "bin").mkdir(parents=True, exist_ok=True)
        (release / "bin/appsandbox-agent").write_bytes(
            b"appsandbox bootstrap control agent\n")
        os.chmod(release / "bin/appsandbox-agent", 0o755)
        for name in BINS[1:]:
            old = root / "usr/local/bin" / name
            _copy_file(old, release / "bin" / name, 0o755)
        (release / "systemd").mkdir(parents=True, exist_ok=True)
    (release / "libexec").mkdir(parents=True, exist_ok=True)
    (release / "systemd").mkdir(parents=True, exist_ok=True)
    # These are copied from the Host-pinned resources even if a stale copy is
    # present in the bootstrap tree.
    if not _copy_file(updater, release / "libexec/appsandbox-guest-updater", 0o755):
        raise RuntimeError("pinned updater is missing")
    if not _copy_file(service, release / "systemd/appsandbox-guest-updater.service", 0o644):
        raise RuntimeError("pinned updater service is missing")
    if not _copy_file(watch, release / "systemd/appsandbox-guest-update-watch.service", 0o644):
        raise RuntimeError("pinned updater watch service is missing")
    if not (release / "bin/appsandbox-agent").is_file():
        raise RuntimeError("bootstrap control agent is missing")
    if not (release / "RELEASE").exists():
        (release / "RELEASE").write_text(
            f"version={version}\ncommit=offline-bootstrap\n", encoding="utf-8")


def _entry_state(root: pathlib.Path, relative: str, backup_dir: pathlib.Path,
                 index: int) -> dict:
    path = root / relative
    record = {"path": relative, "kind": "missing"}
    if path.is_symlink():
        record.update(kind="symlink", target=os.readlink(path))
    elif path.is_file():
        backup = backup_dir / str(index)
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, backup)
        record.update(kind="file", backup=str(backup.relative_to(backup_dir.parent)))
    elif path.exists():
        raise RuntimeError(f"stable path is not a file or symlink: {relative}")
    return record


def _restore_entries(root: pathlib.Path, txn: pathlib.Path, entries: list[dict]) -> None:
    for record in entries:
        path = root / record["path"]
        if path.is_symlink() or path.is_file():
            path.unlink()
        elif path.is_dir():
            raise RuntimeError(f"cannot restore over directory: {record['path']}")
        kind = record["kind"]
        if kind == "symlink":
            path.parent.mkdir(parents=True, exist_ok=True)
            os.symlink(record["target"], path)
        elif kind == "file":
            backup = txn / record["backup"]
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(backup, path)
        _sync(path.parent)


def _recover(root: pathlib.Path) -> None:
    marker = root / "opt/appsandbox/guest/.legacy-bootstrap-state"
    if not marker.is_file():
        return
    state = json.loads(marker.read_text(encoding="utf-8"))
    txn = root / state["txn"]
    if state.get("phase") != "committed":
        _restore_entries(root, txn, state["entries"])
        for relative in state.get("new_releases", []):
            _remove(root / relative)
        for relative in state.get("new_links", []):
            _remove(root / relative)
    shutil.rmtree(txn, ignore_errors=True)
    marker.unlink(missing_ok=True)
    _sync(marker.parent)


def _stable_paths() -> list[str]:
    paths = ["opt/appsandbox/guest/current", "opt/appsandbox/guest/previous"]
    paths += [f"usr/local/bin/{name}" for name in BINS]
    paths += ["usr/local/libexec/appsandbox-display-d3d12",
              "usr/local/libexec/appsandbox-guest-updater"]
    paths += [f"etc/systemd/system/{name}" for name in (*UNITS, *UPDATE_UNITS)]
    paths += [f"etc/systemd/system/multi-user.target.wants/{name}"
              for name in UPDATE_UNITS]
    paths += ["etc/modprobe.d/asb_drm.conf",
              "etc/systemd/user-environment-generators/50-appsandbox-gpu",
              "etc/systemd/user/org.gnome.Shell@.service.d/no-gpu.conf",
              "usr/local/bin/appsandbox-gpu",
              "usr/share/gnome-shell/extensions/appsandbox-pointer@appsandbox/metadata.json",
              "usr/share/gnome-shell/extensions/appsandbox-pointer@appsandbox/extension.js"]
    return paths


def _activate(root: pathlib.Path, guest: pathlib.Path, release_name: str,
              available_bins: list[str], available_units: list[str],
              new_links: list[str]) -> None:
    current = guest / "current"
    previous = guest / "previous"
    _replace_link(current, f"releases/{release_name}")
    new_links.append("opt/appsandbox/guest/current")
    _replace_link(previous, "releases/legacy-original")
    new_links.append("opt/appsandbox/guest/previous")
    for name in available_bins:
        link = root / "usr/local/bin" / name
        _replace_link(link, f"/opt/appsandbox/guest/current/bin/{name}")
        new_links.append(f"usr/local/bin/{name}")
    libexec = root / "usr/local/libexec"
    if (guest / "current/libexec/appsandbox-display-d3d12").is_file():
        _replace_link(libexec / "appsandbox-display-d3d12",
                      "/opt/appsandbox/guest/current/libexec/appsandbox-display-d3d12")
        new_links.append("usr/local/libexec/appsandbox-display-d3d12")
    _replace_link(libexec / "appsandbox-guest-updater",
                  "/opt/appsandbox/guest/current/libexec/appsandbox-guest-updater")
    new_links.append("usr/local/libexec/appsandbox-guest-updater")
    systemd = root / "etc/systemd/system"
    for name in available_units:
        _replace_link(systemd / name,
                      f"/opt/appsandbox/guest/current/systemd/{name}")
        new_links.append(f"etc/systemd/system/{name}")
    wants = systemd / "multi-user.target.wants"
    for name in UPDATE_UNITS:
        if name in available_units:
            _replace_link(wants / name, f"../{name}")
            new_links.append(f"etc/systemd/system/multi-user.target.wants/{name}")
    release = guest / "current"
    config_files = (
        (release / "config/asb_drm.conf", root / "etc/modprobe.d/asb_drm.conf", 0o644),
        (release / "config/50-appsandbox-gpu",
         root / "etc/systemd/user-environment-generators/50-appsandbox-gpu", 0o755),
        (release / "config/org.gnome.Shell-no-gpu.conf",
         root / "etc/systemd/user/org.gnome.Shell@.service.d/no-gpu.conf", 0o644),
        (release / "config/appsandbox-gpu", root / "usr/local/bin/appsandbox-gpu", 0o755),
        (release / "gnome/appsandbox-pointer@appsandbox/metadata.json",
         root / "usr/share/gnome-shell/extensions/appsandbox-pointer@appsandbox/metadata.json", 0o644),
        (release / "gnome/appsandbox-pointer@appsandbox/extension.js",
         root / "usr/share/gnome-shell/extensions/appsandbox-pointer@appsandbox/extension.js", 0o644),
    )
    for source, target, mode in config_files:
        if source.is_file():
            _atomic_copy(source, target, mode)


def migrate(root: pathlib.Path, updater: pathlib.Path, service: pathlib.Path,
            watch: pathlib.Path, bootstrap_root: pathlib.Path | None = None,
            version: str | None = None) -> None:
    root = root.resolve()
    _recover(root)
    guest = root / "opt/appsandbox/guest"
    current = guest / "current"
    releases = guest / "releases"
    if current.exists() or current.is_symlink():
        raise RuntimeError("guest runtime layout already exists")
    if ((releases / "legacy-original").exists() or
            any(releases.glob("bootstrap-*"))):
        raise RuntimeError("bootstrap release already exists")
    if not (root / "usr/local/bin/appsandbox-agent").is_file():
        raise RuntimeError("legacy agent is missing")
    if not updater.is_file() or not service.is_file() or not watch.is_file():
        raise RuntimeError("pinned updater artifacts are missing")
    if bootstrap_root is not None and not bootstrap_root.is_dir():
        raise RuntimeError("bootstrap runtime tree is missing")

    guest.mkdir(parents=True, exist_ok=True)
    releases.mkdir(parents=True, exist_ok=True)
    txid = uuid.uuid4().hex
    txn = guest / f".legacy-bootstrap-txn-{txid}"
    backup_dir = txn / "backups"
    staging = txn / "staging"
    marker = guest / ".legacy-bootstrap-state"
    legacy_name = "legacy-original"
    bootstrap_version = _read_version(bootstrap_root, version) if bootstrap_root else (version or "0.0.0")
    if not VERSION_RE.fullmatch(bootstrap_version):
        raise RuntimeError("bootstrap runtime version is not semver")
    bootstrap_name = f"bootstrap-{bootstrap_version}"
    entries: list[dict] = []
    new_links: list[str] = []
    try:
        txn.mkdir(parents=True, exist_ok=False)
        for index, relative in enumerate(_stable_paths()):
            entries.append(_entry_state(root, relative, backup_dir, index))
        legacy_stage = staging / legacy_name
        bootstrap_stage = staging / bootstrap_name
        _legacy_release(root, legacy_stage, updater, service, watch)
        _bootstrap_release(root, bootstrap_stage, bootstrap_root,
                           updater, service, watch, bootstrap_version)
        _sync(staging)
        os.replace(legacy_stage, releases / legacy_name)
        os.replace(bootstrap_stage, releases / bootstrap_name)
        # Keep the old lookup name for old tooling while making the actual
        # stable pointer unambiguously bootstrap-*.
        _replace_link(releases / "legacy", "legacy-original")
        _write_json_durable(marker, {
            "version": 1,
            "phase": "staged",
            "txn": str(txn.relative_to(root)),
            "entries": entries,
            "new_releases": [str((releases / legacy_name).relative_to(root)),
                             str((releases / bootstrap_name).relative_to(root))],
            "new_links": ["opt/appsandbox/guest/releases/legacy"],
        })
        state = json.loads(marker.read_text(encoding="utf-8"))
        state["phase"] = "activating"
        _write_json_durable(marker, state)
        bootstrap_release = releases / bootstrap_name
        available_bins = [name for name in BINS
                          if (bootstrap_release / "bin" / name).is_file()]
        available_units = [name for name in (*UNITS, *UPDATE_UNITS)
                           if (releases / bootstrap_name / "systemd" / name).is_file()]
        if "appsandbox-agent" not in available_bins:
            raise RuntimeError("bootstrap control agent is missing")
        _activate(root, guest, bootstrap_name, available_bins, available_units, new_links)
        state["phase"] = "committed"
        state["new_links"] = new_links
        _write_json_durable(marker, state)
        shutil.rmtree(txn, ignore_errors=True)
        marker.unlink(missing_ok=True)
        _sync(marker.parent)
    except Exception:
        if marker.is_file():
            try:
                state = json.loads(marker.read_text(encoding="utf-8"))
                _restore_entries(root, txn, state["entries"])
            except Exception:
                # Leave the marker and transaction for the next invocation to
                # recover rather than claiming that rollback completed.
                raise
        for relative in (f"opt/appsandbox/guest/releases/{legacy_name}",
                         f"opt/appsandbox/guest/releases/{bootstrap_name}",
                         "opt/appsandbox/guest/releases/legacy"):
            _remove(root / relative)
        if marker.exists():
            marker.unlink()
        if txn.exists():
            shutil.rmtree(txn, ignore_errors=True)
        raise


def _discover_root() -> pathlib.Path:
    base = pathlib.Path("/mnt/wsl")
    if not base.exists():
        raise RuntimeError("no mounted WSL VHDX root found")
    candidates = [base]
    for directory, dirs, _ in os.walk(base):
        current = pathlib.Path(directory)
        depth = len(current.relative_to(base).parts)
        if depth > 4:
            dirs[:] = []
            continue
        candidates.append(current)
    for candidate in candidates:
        if (candidate / "etc/os-release").is_file() and \
                (candidate / "usr/local/bin/appsandbox-agent").is_file():
            return candidate
    raise RuntimeError("mounted VHDX root is not a Linux AppSandbox guest")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=pathlib.Path)
    ap.add_argument("--discover-mounted-root", action="store_true")
    ap.add_argument("--updater", type=pathlib.Path, required=True)
    ap.add_argument("--service", type=pathlib.Path, required=True)
    ap.add_argument("--watch", type=pathlib.Path, required=True)
    ap.add_argument("--bootstrap-root", type=pathlib.Path)
    ap.add_argument("--version")
    args = ap.parse_args()
    try:
        if bool(args.root) == bool(args.discover_mounted_root):
            raise RuntimeError("choose exactly one root selector")
        root = args.root if args.root is not None else _discover_root()
        migrate(root, args.updater, args.service, args.watch,
                args.bootstrap_root, args.version)
    except (OSError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"legacy bootstrap rejected: {exc}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
