#!/usr/bin/env python3
"""User-session display mode helper for AppSandbox.

The KMS capture daemon runs as root, but Mutter's DisplayConfig API belongs to
the desktop user's D-Bus session. This small user service is the only process
that talks to that API. It accepts one fixed WxH@60 request over a private
runtime-directory Unix socket and returns only after Mutter reports the mode
active.
"""

import os
import re
import socket
import time

from gi.repository import Gio, GLib


BUS_NAME = "org.gnome.Mutter.DisplayConfig"
OBJECT_PATH = "/org/gnome/Mutter/DisplayConfig"
INTERFACE = "org.gnome.Mutter.DisplayConfig"
APPLY_TEMPORARY = 1
SOCKET_NAME = "appsandbox/display-control.sock"
MODES = {(1280, 720), (1920, 1080), (2560, 1440), (3840, 2160)}


def proxy():
    return Gio.DBusProxy.new_for_bus_sync(
        Gio.BusType.SESSION, Gio.DBusProxyFlags.NONE, None,
        BUS_NAME, OBJECT_PATH, INTERFACE, None
    )


def state(p):
    serial, monitors, logical, properties = p.call_sync(
        "GetCurrentState", GLib.Variant("()", ()),
        Gio.DBusCallFlags.NONE, 5000, None
    ).unpack()
    return serial, monitors, logical, properties


def current_mode(modes):
    return next((m for m in modes if m[6].get("is-current")), None)


def mode_for(monitors, identity, width, height):
    for monitor_id, modes, properties in monitors:
        if monitor_id == identity:
            for mode in modes:
                if mode[1] == width and mode[2] == height and round(mode[3]) == 60:
                    return mode
    return None


def apply(p, width, height):
    serial, monitors, logical, _ = state(p)
    if not monitors:
        raise RuntimeError("Mutter has no connected monitor")
    logical_config = []
    for x, y, scale, transform, primary, physical, properties in logical:
        selected = []
        for identity in physical:
            mode = mode_for(monitors, identity, width, height)
            if mode is None:
                raise RuntimeError("mode is not enumerated by Mutter")
            selected.append((identity[0], mode[0], {}))
        logical_config.append((x, y, scale, transform, bool(primary), selected))
    params = GLib.Variant(
        "(uua(iiduba(ssa{sv}))a{sv})",
        (serial, APPLY_TEMPORARY, logical_config, {})
    )
    p.call_sync("ApplyMonitorsConfig", params, Gio.DBusCallFlags.NONE, 10000, None)
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        _, latest, _, _ = state(p)
        if all(
            current_mode(modes) is not None and
            current_mode(modes)[1] == width and
            current_mode(modes)[2] == height
            for identity, modes, properties in latest
        ):
            return
        time.sleep(0.2)
    raise RuntimeError("Mutter did not report the requested mode active")


def handle(p, line):
    match = re.fullmatch(r"([0-9]+)x([0-9]+)@([0-9]+)", line.strip())
    if not match:
        return "ERR invalid request\n"
    width, height, refresh = map(int, match.groups())
    if refresh != 60 or (width, height) not in MODES:
        return "ERR unsupported preset\n"
    try:
        apply(p, width, height)
    except (GLib.Error, RuntimeError) as error:
        return "ERR %s\n" % str(error).replace("\n", " ")[:180]
    return "OK %dx%d@60\n" % (width, height)


def main():
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if not runtime:
        raise SystemExit("XDG_RUNTIME_DIR is required")
    path = os.path.join(runtime, SOCKET_NAME)
    os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    os.umask(0o177)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(path)
    os.chmod(path, 0o600)
    server.listen(2)
    p = proxy()
    try:
        while True:
            client, _ = server.accept()
            with client:
                data = client.recv(128)
                if data:
                    client.sendall(handle(p, data.decode("ascii", "replace")))
    finally:
        server.close()
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    main()
