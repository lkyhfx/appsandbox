#!/usr/bin/env bash
# Gate A runner. This validates the native CUDA/NVENC elementary stream with
# ffmpeg/ffprobe after the probe completes. It never treats missing runtime or
# a non-4:4:4 stream as a PASS.
set -uo pipefail

frames=${1:-600}
out=${2:-gpu-nvenc-hevc444-results}
probe=${NVENC_HEVC444_PROBE:-./nvenc-hevc444-probe}
ffmpeg_bin=${FFMPEG_BIN:-ffmpeg}
ffprobe_bin=${FFPROBE_BIN:-ffprobe}
stream="$out/nvenc-hevc444-${frames}.hevc"
mkdir -p "$out"

if [[ "$frames" != 600 && "$frames" != 3600 ]]; then
    printf 'BLOCKED gate_a reason=frames-must-be-600-or-3600\n'
    exit 2
fi

export LD_LIBRARY_PATH="/usr/lib/wsl/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
set +e
"$probe" "$frames" "$stream" >"$out/nvenc.log" 2>&1
probe_rc=$?
set -e
cat "$out/nvenc.log"

if [[ $probe_rc -ne 0 || ! -s "$stream" ]]; then
    printf 'decoded_width=not-run\n'
    printf 'decoded_height=not-run\n'
    printf 'decoded_pix_fmt=not-run\n'
    printf 'decode_errors=not-run\n'
    printf 'gate_a_verdict=FAIL\n'
    printf 'production-4k60: false\n'
    exit 3
fi

if ! command -v "$ffmpeg_bin" >/dev/null 2>&1 ||
   ! command -v "$ffprobe_bin" >/dev/null 2>&1; then
    printf 'BLOCKED gate_a reason=ffmpeg-or-ffprobe-not-found\n'
    printf 'gate_a_verdict=FAIL\n'
    printf 'production-4k60: false\n'
    exit 3
fi

set +e
"$ffmpeg_bin" -v error -err_detect explode -f hevc -i "$stream" \
    -f null - >"$out/decode.log" 2>&1
decode_rc=$?
geometry=$("$ffprobe_bin" -v error -f hevc -count_frames -select_streams v:0 \
    -show_entries stream=width,height,pix_fmt,nb_read_frames -of csv=p=0 "$stream" \
    2>>"$out/decode.log")
set -e
cat "$out/decode.log"

IFS=',' read -r decoded_width decoded_height decoded_pix_fmt decoded_frames <<<"$geometry"
decoded_width=${decoded_width:-not-run}
decoded_height=${decoded_height:-not-run}
decoded_pix_fmt=${decoded_pix_fmt:-not-run}
decoded_frames=${decoded_frames:-not-run}
printf 'decoded_width=%s\n' "$decoded_width"
printf 'decoded_height=%s\n' "$decoded_height"
printf 'decoded_pix_fmt=%s\n' "$decoded_pix_fmt"
printf 'decoded_frames=%s\n' "$decoded_frames"
printf 'decode_errors=%s\n' "$([[ $decode_rc -eq 0 ]] && echo 0 || echo 1)"

if [[ $decode_rc -eq 0 && "$decoded_width" == 3840 &&
      "$decoded_height" == 2160 && "$decoded_frames" == "$frames" &&
      "$decoded_pix_fmt" == *444* ]]; then
    printf 'gate_a_verdict=PASS\n'
    printf 'production-4k60: false\n'
    exit 0
fi

printf 'gate_a_verdict=FAIL\n'
printf 'production-4k60: false\n'
exit 3
