#!/usr/bin/env bash
# Isolated nested Mutter diagnostic.  This uses a private headless Weston
# parent, so it never opens the production asb_drm display or appsandbox-
# display service.  It is a renderer/client smoke test, not the 3600-frame
# acceptance runner.
set -u

root=${1:-/tmp/asb-nested-mutter-d3d12}
client=${MUTTER_TEST_CLIENT_BIN:-./mutter-wayland-pattern-client}
mesa=${MESA_PROBE_PREFIX:-}
mkdir -p "$root"
runtime=$(mktemp -d /tmp/asb-nested-runtime-XXXXXX)
chmod 700 "$runtime"
parent_pid=''
shell_pid=''

cleanup() {
    set +e
    [[ -n "$shell_pid" ]] && kill "$shell_pid" 2>/dev/null || true
    [[ -n "$parent_pid" ]] && kill "$parent_pid" 2>/dev/null || true
    [[ -n "$shell_pid" ]] && wait "$shell_pid" 2>/dev/null || true
    [[ -n "$parent_pid" ]] && wait "$parent_pid" 2>/dev/null || true
    rm -rf "$runtime"
}
trap cleanup EXIT INT TERM

if [[ -n "$mesa" ]]; then
    export LD_LIBRARY_PATH="$mesa/lib/x86_64-linux-gnu:/opt/appsandbox/wsl-deps${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LIBGL_DRIVERS_PATH="$mesa/lib/x86_64-linux-gnu/dri"
    export GBM_BACKENDS_PATH="$mesa/lib/x86_64-linux-gnu/gbm"
    export __EGL_VENDOR_LIBRARY_FILENAMES="$mesa/share/glvnd/egl_vendor.d/50_mesa.json"
    export MESA_LOADER_DRIVER_OVERRIDE=d3d12
    export GALLIUM_DRIVER=d3d12
fi

export XDG_RUNTIME_DIR="$runtime"
export WAYLAND_DISPLAY=asb-parent
weston --backend=headless-backend.so --socket="$WAYLAND_DISPLAY" \
    --width=3840 --height=2160 --idle-time=0 \
    >"$root/weston.log" 2>&1 &
parent_pid=$!
for _ in $(seq 1 200); do
    [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] && break
    sleep 0.1
done
if [[ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]]; then
    printf 'FAIL stage=nested-parent-wayland\n'
    exit 1
fi

export WAYLAND_DISPLAY=asb-nested
gnome-shell --wayland --no-x11 --wayland-display="$WAYLAND_DISPLAY" \
    >"$root/mutter.log" 2>&1 &
shell_pid=$!
for _ in $(seq 1 300); do
    [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] && break
    sleep 0.1
done
if [[ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]]; then
    printf 'FAIL stage=nested-mutter-wayland\n'
    exit 1
fi

ASB_PATTERN_CLIENT_FRAMES=${ASB_PATTERN_CLIENT_FRAMES:-120} \
    "$client" >"$root/client.log" 2>&1
client_status=$?
cat "$root/weston.log" "$root/mutter.log" "$root/client.log"
printf 'nested_client_exit=%d\n' "$client_status"
exit "$client_status"
