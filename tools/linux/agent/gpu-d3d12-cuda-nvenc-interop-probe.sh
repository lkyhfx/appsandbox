#!/usr/bin/env bash
# Gate B runner. It reports the first unsupported boundary and never inserts
# CPU readback, conversion, or upload as a fallback.
set -uo pipefail

out=${1:-gpu-d3d12-cuda-nvenc-results}
probe=${D3D12_CUDA_NVENC_PROBE:-./d3d12-cuda-nvenc-interop-probe}
mkdir -p "$out"

export LD_LIBRARY_PATH="/usr/lib/wsl/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
set +e
"$probe" >"$out/interop.log" 2>&1
probe_rc=$?
set -e
cat "$out/interop.log"

if [[ $probe_rc -eq 0 ]]; then
    printf 'gate_b_verdict=PASS\n'
else
    printf 'gate_b_verdict=BLOCKED\n'
fi
printf 'production-4k60: false\n'
exit "$probe_rc"
