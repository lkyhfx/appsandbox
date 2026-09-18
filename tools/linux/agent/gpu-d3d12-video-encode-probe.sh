#!/usr/bin/env bash
# Run beside d3d12-video-encode-probe in the target Linux Guest.
set -uo pipefail

out=${1:-gpu-d3d12-video-encode-results}
mkdir -p "$out"
capability_log="$out/video-encode-capability.log"
encode_log="$out/video-encode-4k60.log"
decode_log="$out/video-encode-decode.log"
stream="$out/probe.hevc"
runner=${GPU_RUNNER:-appsandbox-gpu}
d3d12_libdir=${D3D12_LIBDIR:-/opt/appsandbox/wsl-deps}
probe=./d3d12-video-encode-probe

if [[ -d "$d3d12_libdir" ]]; then
    export LD_LIBRARY_PATH="$d3d12_libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

run_probe() {
    local timeout_seconds=$1
    shift
    if [[ -n "$runner" ]]; then
        timeout "${timeout_seconds}s" "$runner" "$probe" "$@"
    else
        timeout "${timeout_seconds}s" "$probe" "$@"
    fi
}

set +e
run_probe 30 --capability >"$capability_log" 2>&1
capability_rc=$?
set -e
cat "$capability_log"
if [[ $capability_rc -ne 0 ]]; then
    printf 'BLOCKED stage=d3d12-video-encode-capability exit_code=%d\n' "$capability_rc"
    printf 'EXIT_CODE=%d\n' "$capability_rc"
    exit "$capability_rc"
fi

rm -f "$stream"
set +e
export D3D12_VIDEO_BITSTREAM_PATH="$stream"
run_probe 90 >"$encode_log" 2>&1
encode_rc=$?
set -e
cat "$encode_log"
if [[ $encode_rc -ne 0 ]]; then
    printf 'FAIL stage=4k60-hardware-encode exit_code=%d\n' "$encode_rc"
    printf 'EXIT_CODE=%d\n' "$encode_rc"
    exit "$encode_rc"
fi

ffmpeg_bin=${FFMPEG_BIN:-ffmpeg}
ffprobe_bin=${FFPROBE_BIN:-ffprobe}
if ! command -v "$ffmpeg_bin" >/dev/null 2>&1 ||
   ! command -v "$ffprobe_bin" >/dev/null 2>&1; then
    printf 'BLOCKED stage=decoded-hevc reason=ffmpeg-or-ffprobe-not-found\n' |
        tee "$decode_log"
    printf 'EXIT_CODE=2\n'
    exit 2
fi

set +e
"$ffmpeg_bin" -v error -f hevc -i "$stream" -f null - >"$decode_log" 2>&1
decode_rc=$?
geometry=$(
    "$ffprobe_bin" -v error -f hevc -count_frames -select_streams v:0 \
        -show_entries stream=width,height,nb_read_frames -of csv=p=0 "$stream" 2>>"$decode_log"
)
set -e
cat "$decode_log"
printf 'decoded_probe=%s\n' "$geometry"
if [[ $decode_rc -eq 0 && "$geometry" == "3840,2160,3600" ]]; then
    printf 'PASS stage=decoded-hevc frames=3600 resolution=3840x2160\n'
    printf 'PASS d3d12-shared-texture-hardware-encode\n'
    printf 'EXIT_CODE=0\n'
    exit 0
fi

printf 'BLOCKED stage=decoded-hevc raw_d3d12_payload=not_container_ready\n'
printf 'EXIT_CODE=2\n'
exit 2
