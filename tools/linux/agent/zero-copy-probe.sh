#!/usr/bin/env bash
# Non-destructive prerequisites and encoder probe. Run inside the target Guest.
# This does not establish zero-copy interop or measure desktop capture latency.
set -u
export PATH="/usr/lib/wsl/lib:$PATH"
export LD_LIBRARY_PATH="/usr/lib/wsl/lib:${LD_LIBRARY_PATH:-}"
printf '\n== Environment ==\n'
uname -a
cat /etc/os-release
ls -l /dev/dxg /dev/dri 2>/dev/null || true
printf '\n== DRM drivers and active framebuffer state ==\n'
for card in /sys/class/drm/card[0-9]*; do
    [ -e "$card/device" ] || continue
    printf '%s: ' "$card"
    readlink -f "$card/device/driver" || true
done
for node in /sys/kernel/debug/dri/*/name /sys/kernel/debug/dri/*/state /sys/kernel/debug/dma_buf/bufinfo; do
    [ -r "$node" ] || continue
    printf '\n%s\n' "$node"
    cat "$node"
done
printf '\n== Display services ==\n'
systemctl is-active appsandbox-display 2>/dev/null || true
printf '\n== Tools ==\n'
command -v ffmpeg gcc pkg-config vainfo nvidia-smi || true
pkg-config --modversion libdrm 2>/dev/null || true
printf '\n== GPU ==\n'
nvidia-smi 2>&1 || true
printf '\n== Encoder execution: synthetic CPU source + upload, NOT zero-copy ==\n'
if command -v ffmpeg >/dev/null; then
    ffmpeg -hide_banner -version | head -3
    timeout 90s ffmpeg -hide_banner -nostdin -benchmark \
        -f lavfi -i testsrc2=size=3840x2160:rate=60 \
        -frames:v 180 -vf format=nv12 \
        -c:v hevc_nvenc -preset p1 -tune ull -bf 0 -rc-lookahead 0 \
        -f null -
    encode_rc=$?
    printf '\nENCODE_EXIT_CODE=%s\n' "$encode_rc"
else
    printf 'SKIP: ffmpeg unavailable\n'
fi
printf '\nExternal image import, fence synchronization, and no-readback tracing still require separate tests.\n'
