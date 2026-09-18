#!/usr/bin/env bash
# Run the isolated Mutter/Mesa native-D3D12 experiment.
#
# This script intentionally refuses to claim real-desktop success unless a
# visible dynamic Wayland test client is supplied.  The test client must paint
# an opaque full-screen frame whose semantic RGBA pixel at the three diagnostic
# points is the frame pattern advertised by the patched Mesa publisher:
#   Base R=31, G=127, B=223, A=255; a moving opaque square changes per frame
#   away from the three diagnostic points.
#
# The compositor side is expected to be patched with
# mutter-d3d12-share-probe.patch and the Mesa d3d12 hook described by that
# patch.  appsandbox-display is never stopped or reconfigured.
set -uo pipefail

out=${1:-gpu-mutter-d3d12-share-probe-results}
mkdir -p "$out"
consumer_log="$out/mutter-consumer.log"
mutter_log="$out/mutter-session.log"
client_log="$out/mutter-client.log"
runner_log="$out/runner.log"
decode_log="$out/decode.log"
stream="$out/mutter.hevc"
socket_path="${ASB_MUTTER_SOCKET:-$out/mutter-d3d12.sock}"
consumer_bin=${MUTTER_CONSUMER_BIN:-./d3d12-mutter-consumer}
runner=${GPU_RUNNER:-appsandbox-gpu}
d3d12_libdir=${D3D12_LIBDIR:-/opt/appsandbox/wsl-deps}
mesa_probe_prefix=${MESA_PROBE_PREFIX:-}
wayland_display=${MUTTER_WAYLAND_DISPLAY:-asb-gpu-mutter}

if [[ -z "${MUTTER_TEST_CLIENT_CMD:-}" ]]; then
    printf 'BLOCKED stage=consumer-real-desktop-frame reason=MUTTER_TEST_CLIENT_CMD-not-set\n' \
        | tee "$runner_log"
    printf 'The runner requires an opaque, dynamic Wayland test client so diagnostic pixels are evidence of real compositor content.\n' \
        >>"$runner_log"
    exit 2
fi

if [[ ! -x "$consumer_bin" ]]; then
    printf 'BLOCKED stage=consumer-binary reason=missing:%s\n' "$consumer_bin" \
        | tee "$runner_log"
    exit 2
fi

if [[ -n "$mesa_probe_prefix" ]]; then
    export LD_LIBRARY_PATH="$mesa_probe_prefix/lib/x86_64-linux-gnu:$d3d12_libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LIBGL_DRIVERS_PATH="$mesa_probe_prefix/lib/x86_64-linux-gnu/dri"
    export EGL_DRIVERS_PATH="$mesa_probe_prefix/lib/x86_64-linux-gnu"
    export GBM_BACKENDS_PATH="$mesa_probe_prefix/lib/x86_64-linux-gnu/gbm"
    export __EGL_VENDOR_LIBRARY_FILENAMES="$mesa_probe_prefix/share/glvnd/egl_vendor.d/50_mesa.json"
    export MESA_LOADER_DRIVER_OVERRIDE=d3d12
    export GALLIUM_DRIVER=d3d12
elif [[ -d "$d3d12_libdir" ]]; then
    export LD_LIBRARY_PATH="$d3d12_libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
rm -f "$socket_path" "$stream"
: >"$client_log"

consumer_status=1
mutter_status=1
consumer_pid=''
mutter_pid=''
runtime=''

cleanup() {
    set +e
    [[ -n "$mutter_pid" ]] && kill "$mutter_pid" 2>/dev/null || true
    [[ -n "$consumer_pid" ]] && kill "$consumer_pid" 2>/dev/null || true
    [[ -n "$mutter_pid" ]] && wait "$mutter_pid" 2>/dev/null || true
    [[ -n "$consumer_pid" ]] && wait "$consumer_pid" 2>/dev/null || true
    [[ -n "$runtime" ]] && rm -rf "$runtime"
    rm -f "$socket_path"
}
trap cleanup EXIT INT TERM

set +e
export D3D12_VIDEO_BITSTREAM_PATH="$stream"
"$consumer_bin" "$socket_path" >"$consumer_log" 2>&1 &
consumer_pid=$!
set -e

for _ in $(seq 1 100); do
    [[ -S "$socket_path" ]] && break
    sleep 0.1
done
if [[ ! -S "$socket_path" ]]; then
    printf 'BLOCKED stage=mutter-consumer-listener reason=socket-not-ready\n' |
        tee "$runner_log"
    exit 2
fi

runtime=$(mktemp -d /tmp/asb-mutter-d3d12-XXXXXX)
chmod 700 "$runtime"

set +e
timeout -k 5s 180s dbus-run-session -- env \
    XDG_RUNTIME_DIR="$runtime" \
    WAYLAND_DISPLAY="$wayland_display" \
    ASB_MUTTER_D3D12_SHARE_SOCKET="$socket_path" \
    ASB_MUTTER_D3D12_SHARE_WIDTH=3840 \
    ASB_MUTTER_D3D12_SHARE_HEIGHT=2160 \
    ASB_MUTTER_D3D12_SHARE_FRAMES=3600 \
    ASB_MUTTER_D3D12_SHARE_EXPECTED_PATTERN=frame-rgba \
    ASB_MUTTER_SESSION_LOG="$mutter_log" \
    ASB_MUTTER_CLIENT_LOG="$client_log" \
    ASB_PATTERN_CLIENT_FRAMES="${ASB_PATTERN_CLIENT_FRAMES:-3600}" \
    MUTTER_TEST_CLIENT_CMD="$MUTTER_TEST_CLIENT_CMD" \
    bash -c '
        set -u
        gnome_shell=${GNOME_SHELL_BIN:-gnome-shell}
        "$gnome_shell" --headless --no-x11 \
            --virtual-monitor=3840x2160 --wayland-display="$WAYLAND_DISPLAY" \
            >"$ASB_MUTTER_SESSION_LOG" 2>&1 &
        shell_pid=$!
        for _ in $(seq 1 200); do
            [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] && break
            sleep 0.1
        done
        if [[ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]]; then
            printf "FAIL stage=mutter-wayland-display\n" >>"$ASB_MUTTER_SESSION_LOG"
            kill "$shell_pid" 2>/dev/null || true
            wait "$shell_pid" 2>/dev/null || true
            exit 1
        fi
        printf "client_cmd=%s\\n" "$MUTTER_TEST_CLIENT_CMD" >>"$ASB_MUTTER_SESSION_LOG"
        # The caller supplies the client command; it must stay visible for the
        # duration of the 3600-frame capture and must not use screenshots or
        # framebuffer readback as its source.
        bash -c "$MUTTER_TEST_CLIENT_CMD" >"$ASB_MUTTER_CLIENT_LOG" 2>&1 &
        client_pid=$!
        wait "$client_pid"
        client_status=$?
        printf "client_exit=%d\\n" "$client_status" >>"$ASB_MUTTER_CLIENT_LOG"
        kill "$shell_pid" 2>/dev/null || true
        wait "$shell_pid" 2>/dev/null || true
        if [[ "$client_status" -ne 0 ]]; then
            exit "$client_status"
        fi
        exit 0
    ' >"$runner_log" 2>&1
mutter_status=$?
set -e

set +e
# A missing producer leaves the consumer blocked in accept()/recv().  Do not
# let that turn a diagnostic failure into an apparently hung SSH session.
consumer_grace_seconds=${ASB_CONSUMER_GRACE_SECONDS:-30}
consumer_grace_ticks=$((consumer_grace_seconds * 10))
for _ in $(seq 1 "$consumer_grace_ticks"); do
    if ! kill -0 "$consumer_pid" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
if kill -0 "$consumer_pid" 2>/dev/null; then
    printf 'BLOCKED stage=mutter-consumer reason=no-producer-frame-within-timeout seconds=%d\n' \
        "$consumer_grace_seconds" | tee -a "$runner_log"
    kill "$consumer_pid" 2>/dev/null || true
fi
wait "$consumer_pid"
consumer_status=$?
set -e

cat "$runner_log"
cat "$mutter_log" 2>/dev/null || true
cat "$consumer_log"
printf 'mutter_session_exit=%d\nconsumer_exit=%d\n' \
       "$mutter_status" "$consumer_status" | tee -a "$runner_log"

if [[ "$consumer_status" -ne 0 ]]; then
    printf 'FAIL stage=mutter-consumer exit_code=%d\n' "$consumer_status"
    exit "$consumer_status"
fi
if ! grep -Eq 'PASS stage=mutter-d3d12-renderer' "$consumer_log" "$runner_log" "$mutter_log"; then
    printf 'FAIL stage=mutter-d3d12-renderer reason=missing-Mesa-renderer-evidence\n'
    exit 1
fi
if ! grep -Eq 'PASS stage=mutter-real-render-target synthetic_source=0' "$consumer_log"; then
    printf 'FAIL stage=mutter-real-render-target reason=missing-real-target-evidence\n'
    exit 1
fi
require_gate() {
    if ! grep -Eq "$1" "$consumer_log" "$runner_log"; then
        printf 'FAIL stage=%s reason=missing-gate-evidence\n' "$2"
        exit 1
    fi
}
require_gate 'PASS stage=mutter-real-render-target synthetic_source=0 .*format=RGBA8 dxgi_format=28' \
    mutter-real-render-target
require_gate 'PASS stage=mutter-shared-resource format=RGBA8 .*native_d3d12_shared=1 .*gpu_copy=1 .*cpu_copy=0' \
    mutter-shared-resource
require_gate 'PASS stage=cross-process-open-shared-resource format=RGBA8' \
    cross-process-open-shared-resource
require_gate 'PASS stage=d3d12-video-rgba-nv12-support input=RGBA8 output=NV12 resolution=3840x2160 fps=60/1' \
    d3d12-video-rgba-nv12-support
require_gate 'PASS stage=rgba-to-nv12-gpu-only .*cpu_conversion=0' \
    rgba-to-nv12-gpu-only
require_gate 'PASS stage=producer-eventfd-sync busy_poll=0' producer-eventfd-sync
require_gate 'PASS stage=consumer-real-desktop-frame .*stale_frames=0 mismatches=0' \
    consumer-real-desktop-frame
require_gate 'PASS stage=d3d12-hardware-encode' d3d12-hardware-encode
require_gate 'PASS stage=4k60-sustained .*frames=3600 .*timeouts=0 .*mismatches=0 .*encode_failures=0' \
    4k60-sustained
require_gate 'PASS stage=throughput-zero-copy .*framebuffer_mmap=0 .*cpu_memcpy_framebuffer=0 .*gpu_cpu_gpu=0' \
    throughput-zero-copy

ffmpeg_bin=${FFMPEG_BIN:-ffmpeg}
ffprobe_bin=${FFPROBE_BIN:-ffprobe}
if ! command -v "$ffmpeg_bin" >/dev/null 2>&1 ||
   ! command -v "$ffprobe_bin" >/dev/null 2>&1; then
    printf 'BLOCKED stage=decoded-hevc reason=ffmpeg-or-ffprobe-not-found\n' |
        tee "$decode_log"
    exit 2
fi

set +e
"$ffmpeg_bin" -v error -f hevc -i "$stream" -f null - >"$decode_log" 2>&1
decode_status=$?
geometry=$("$ffprobe_bin" -v error -f hevc -count_frames -select_streams v:0 \
    -show_entries stream=width,height,nb_read_frames -of csv=p=0 "$stream" \
    2>>"$decode_log")
set -e
cat "$decode_log"
printf 'decoded_probe=%s\n' "$geometry"
if [[ "$decode_status" -ne 0 || "$geometry" != "3840,2160,3600" ]]; then
    printf 'FAIL stage=decoded-hevc frames_or_resolution_invalid\n'
    exit 1
fi

printf 'decoded_frames=3600 resolution=3840x2160 bitstream_decode_errors=0\n'
printf 'PASS stage=decoded-hevc frames=3600 resolution=3840x2160\n'
printf 'PASS mutter-d3d12-zero-copy-encode\n'
exit 0
