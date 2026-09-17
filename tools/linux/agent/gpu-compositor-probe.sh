#!/usr/bin/env bash
# Isolated headless startup only; does not prove output export or GPU residency.
set -uo pipefail
out=${1:-compositor.log}
runtime=$(mktemp -d /tmp/asb-compositor-XXXXXX) || exit 1
chmod 700 "$runtime"
printf 'Temporary runtime: %s\n' "$runtime"
# Keep the temporary directory for diagnosis; no existing session is replaced.
timeout -k 5s 20s dbus-run-session -- env XDG_RUNTIME_DIR="$runtime" \
    appsandbox-gpu gnome-shell --headless --no-x11 \
    --virtual-monitor=1920x1080 --wayland-display=asb-gpu-probe >"$out" 2>&1
result=$?
printf 'EXIT_CODE=%s\n' "$result" >>"$out"
cat "$out"
# 124 means timeout, not a successful compositor-output acceptance test.
exit "$result"
