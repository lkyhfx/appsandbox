#!/usr/bin/env python3
"""Probe Mutter's user-session DisplayConfig API.

This is intentionally a standalone verification tool. It does not change the
AppSandbox capture daemon, ASDC protocol, or host resize state machine.

Run it from the live GNOME user session, for example:

    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
        ./mutter-displayconfig-probe.py 1920x1080 2560x1440 3840x2160

The target mode must already be present in Mutter's current mode list. For the
AppSandbox DRM driver, update /sys/devices/platform/asb_drm.0/mode first; the
probe then actively asks Mutter to apply the enumerated mode through
org.gnome.Mutter.DisplayConfig.ApplyMonitorsConfig.
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import time

from gi.repository import Gio, GLib


BUS_NAME = "org.gnome.Mutter.DisplayConfig"
OBJECT_PATH = "/org/gnome/Mutter/DisplayConfig"
INTERFACE = "org.gnome.Mutter.DisplayConfig"
APPLY_TEMPORARY = 1


def make_proxy():
    return Gio.DBusProxy.new_for_bus_sync(
        Gio.BusType.SESSION,
        Gio.DBusProxyFlags.NONE,
        None,
        BUS_NAME,
        OBJECT_PATH,
        INTERFACE,
        None,
    )


def current_state(proxy):
    result = proxy.call_sync(
        "GetCurrentState", GLib.Variant("()", ()), Gio.DBusCallFlags.NONE, 5000, None
    )
    serial, monitors, logical_monitors, properties = result.unpack()
    return {
        "serial": serial,
        "monitors": monitors,
        "logical_monitors": logical_monitors,
        "properties": properties,
    }


def current_mode(modes):
    for mode in modes:
        if mode[6].get("is-current"):
            return mode
    return None


def mode_record(mode):
    mode_id, width, height, refresh, scale, scales, properties = mode
    return {
        "id": mode_id,
        "width": width,
        "height": height,
        "refresh": refresh,
        "is_current": bool(properties.get("is-current", False)),
        "is_preferred": bool(properties.get("is-preferred", False)),
    }


def print_state(state, label):
    print("mutter_state label=%s serial=%u" % (label, state["serial"]))
    for identity, modes, properties in state["monitors"]:
        selected = current_mode(modes)
        selected_text = "none"
        if selected:
            selected_text = "%sx%s@%.3f id=%s" % (
                selected[1],
                selected[2],
                selected[3],
                selected[0],
            )
        print(
            "mutter_monitor connector=%s vendor=%s product=%s serial=%s "
            "current=%s modes=%s"
            % (
                identity[0],
                identity[1],
                identity[2],
                identity[3],
                selected_text,
                ",".join(
                    "%sx%s@%.3f" % (mode[1], mode[2], mode[3]) for mode in modes
                ),
            )
        )
    for x, y, scale, transform, primary, monitors, properties in state[
        "logical_monitors"
    ]:
        print(
            "mutter_logical x=%s y=%s scale=%.3f transform=%s primary=%s "
            "connectors=%s"
            % (
                x,
                y,
                scale,
                transform,
                str(bool(primary)).lower(),
                ",".join(identity[0] for identity in monitors),
            )
        )


def parse_target(text):
    match = re.fullmatch(r"([0-9]+)x([0-9]+)", text)
    if not match:
        raise ValueError("target must use WIDTHxHEIGHT, got %r" % text)
    return int(match.group(1)), int(match.group(2))


def find_mode(state, width, height):
    matches = []
    for identity, modes, properties in state["monitors"]:
        for mode in modes:
            if mode[1] == width and mode[2] == height:
                matches.append((identity, mode))
    return matches


def build_apply_configuration(state, width, height):
    logical_config = []
    for x, y, scale, transform, primary, logical_monitors, properties in state[
        "logical_monitors"
    ]:
        physical_config = []
        for identity in logical_monitors:
            matches = [
                mode
                for mode in physical_identity_modes(state, identity)
                if mode[1] == width and mode[2] == height
            ]
            if not matches:
                raise RuntimeError(
                    "Mutter did not enumerate %sx%s for connector %s"
                    % (width, height, identity[0])
                )
            # ApplyMonitorsConfig takes (connector, mode-id, properties), and
            # the mode-id is selected from GetCurrentState rather than guessed.
            physical_config.append((identity[0], matches[0][0], {}))
        logical_config.append(
            (x, y, scale, transform, bool(primary), physical_config)
        )
    return GLib.Variant(
        "(uua(iiduba(ssa{sv}))a{sv})",
        (state["serial"], APPLY_TEMPORARY, logical_config, {}),
    )


def physical_identity_modes(state, identity):
    for physical_identity, modes, properties in state["monitors"]:
        if physical_identity == identity:
            return modes
    return []


def active_target(state, width, height):
    if not state["monitors"]:
        return False
    return all(
        mode is not None and mode[1] == width and mode[2] == height
        for identity, modes, properties in state["monitors"]
        for mode in [current_mode(modes)]
    )


def drm_state_paths():
    paths = sorted(glob.glob("/sys/kernel/debug/dri/*/state"))
    if not paths:
        result = subprocess.run(
            ["sudo", "-n", "find", "/sys/kernel/debug/dri", "-maxdepth", "2", "-name", "state", "-print"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode == 0:
            paths = [line for line in result.stdout.splitlines() if line]
    return paths


def read_drm_state():
    paths = drm_state_paths()
    snapshots = []
    if not paths:
        print("drm_state unavailable reason=no_debugfs_state_files")
        return snapshots
    for path in paths:
        try:
            with open(path, "r", encoding="utf-8") as stream:
                content = stream.read()
        except PermissionError:
            result = subprocess.run(
                ["sudo", "-n", "cat", path],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            if result.returncode != 0:
                print("drm_state path=%s unavailable reason=permission" % path)
                continue
            content = result.stdout
        primary = False
        in_crtc = False
        primary_size = None
        crtc_mode = None
        for line in content.splitlines():
            stripped = line.strip()
            if stripped.startswith("plane["):
                primary = stripped.endswith(": primary")
                in_crtc = False
            elif stripped.startswith("crtc["):
                primary = False
                in_crtc = True
            elif stripped.startswith("connector["):
                primary = False
                in_crtc = False
            if primary and primary_size is None:
                match = re.match(r"size=([0-9]+)x([0-9]+)$", stripped)
                if match:
                    primary_size = (int(match.group(1)), int(match.group(2)))
            if in_crtc and crtc_mode is None:
                match = re.match(r'mode: "([0-9]+)x([0-9]+)"', stripped)
                if match:
                    crtc_mode = (int(match.group(1)), int(match.group(2)))
        snapshots.append(
            {"path": path, "primary_size": primary_size, "crtc_mode": crtc_mode}
        )
        print("drm_state path=%s" % path)
        for line in content.splitlines():
            stripped = line.strip()
            if re.match(
                r"^(plane\[|crtc\[|connector\[|fb=|size=|crtc-pos=|mode:)",
                stripped,
            ):
                print("drm_state_detail %s" % stripped)
    return snapshots


def drm_active_target(snapshots, width, height):
    return any(
        snapshot["primary_size"] == (width, height)
        and snapshot["crtc_mode"] == (width, height)
        for snapshot in snapshots
    )


def print_display_log():
    command = [
        "journalctl",
        "-u",
        "appsandbox-display.service",
        "--no-pager",
        "-n",
        "120",
        "-o",
        "cat",
    ]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0 or not result.stdout.strip():
        result = subprocess.run(
            ["sudo", "-n"] + command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    if result.returncode != 0:
        print("display_log unavailable reason=journalctl")
        return
    for line in result.stdout.splitlines():
        if re.search(r"capturing |layout changed|selected /dev/dri|using /dev/dri", line):
            print("display_log %s" % line)


def wait_for_target(proxy, width, height, timeout):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        latest = current_state(proxy)
        if active_target(latest, width, height):
            return latest, True
        time.sleep(0.25)
    return latest, False


def apply_target(proxy, state, width, height, timeout):
    print("mutter_apply requested=%sx%s serial=%u" % (width, height, state["serial"]))
    matches = find_mode(state, width, height)
    if not matches:
        print(
            "mutter_apply result=FAIL requested=%sx%s reason=mode_not_enumerated"
            % (width, height)
        )
        return state, False
    try:
        params = build_apply_configuration(state, width, height)
        proxy.call_sync(
            "ApplyMonitorsConfig", params, Gio.DBusCallFlags.NONE, 10000, None
        )
    except GLib.Error as error:
        print(
            "mutter_apply result=FAIL requested=%sx%s reason=dbus_apply_error "
            "error=%s" % (width, height, error)
        )
        return state, False
    except RuntimeError as error:
        print(
            "mutter_apply result=FAIL requested=%sx%s reason=%s"
            % (width, height, error)
        )
        return state, False

    latest, mutter_passed = wait_for_target(proxy, width, height, timeout)
    print(
        "mutter_apply active=%s requested=%sx%s"
        % (str(mutter_passed).lower(), width, height)
    )
    print_state(latest, "after-%sx%s" % (width, height))
    drm_snapshots = read_drm_state()
    drm_passed = drm_active_target(drm_snapshots, width, height)
    print(
        "mutter_apply result=%s requested=%sx%s mutter_active=%s drm_active=%s"
        % (
            "PASS" if mutter_passed and drm_passed else "FAIL",
            width,
            height,
            str(mutter_passed).lower(),
            str(drm_passed).lower(),
        )
    )
    print_display_log()
    return latest, mutter_passed and drm_passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("targets", nargs="*", help="one or more WIDTHxHEIGHT modes to apply")
    parser.add_argument(
        "--wait-seconds", type=float, default=8.0, help="wait for Mutter active state"
    )
    args = parser.parse_args()

    try:
        proxy = make_proxy()
        state = current_state(proxy)
    except GLib.Error as error:
        print("probe result=FAIL reason=session_or_displayconfig error=%s" % error)
        return 2

    print("probe interface=%s" % INTERFACE)
    print("probe session_bus=%s" % os.environ.get("DBUS_SESSION_BUS_ADDRESS", "default"))
    print_state(state, "initial")
    read_drm_state()

    overall_pass = True
    for target in args.targets:
        try:
            width, height = parse_target(target)
        except ValueError as error:
            print("probe result=FAIL reason=%s" % error)
            overall_pass = False
            continue
        state, passed = apply_target(proxy, state, width, height, args.wait_seconds)
        overall_pass = overall_pass and passed

    print("probe result=%s" % ("PASS" if overall_pass else "FAIL"))
    return 0 if overall_pass else 1


if __name__ == "__main__":
    sys.exit(main())
