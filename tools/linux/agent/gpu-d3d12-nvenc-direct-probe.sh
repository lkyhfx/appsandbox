#!/usr/bin/env bash
# Gate B': probe the last D3D12 -> NVENC DirectX route.  The probe itself
# fails closed and never adds CPU framebuffer staging or upload.
set -uo pipefail

out=${1:-gpu-d3d12-nvenc-direct-results}
probe=${D3D12_NVENC_DIRECT_PROBE:-./d3d12-nvenc-direct-probe}
d3d12_libdir=${D3D12_LIBDIR:-/opt/appsandbox/wsl-deps}
mkdir -p "$out"
export LD_LIBRARY_PATH="$d3d12_libdir:/usr/lib/wsl/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

set +e
"$probe" >"$out/direct.log" 2>&1
probe_rc=$?
set -e
cat "$out/direct.log"

session=$(sed -n 's/^nvenc_directx_session=//p' "$out/direct.log" | tail -n 1)
register=$(sed -n 's/^nvenc_register_d3d12_resource=//p' "$out/direct.log" | tail -n 1)
map=$(sed -n 's/^nvenc_directx_map=//p' "$out/direct.log" | tail -n 1)
encode=$(sed -n 's/^nvenc_directx_actual_encode=//p' "$out/direct.log" | tail -n 1)
pix_fmt=$(sed -n 's/^nvenc_directx_decoded_pix_fmt=//p' "$out/direct.log" | tail -n 1)

printf 'nvenc_directx_session=%s\n' "${session:-BLOCKED}"
printf 'nvenc_register_d3d12_resource=%s\n' "${register:-BLOCKED}"
printf 'nvenc_directx_map=%s\n' "${map:-BLOCKED}"
printf 'nvenc_directx_actual_encode=%s\n' "${encode:-BLOCKED}"
printf 'nvenc_directx_decoded_pix_fmt=%s\n' "${pix_fmt:-unknown}"
printf 'cpu_framebuffer_copy=0\n'
printf 'cpu_upload=0\n'

if [[ "${session:-BLOCKED}" == PASS &&
      "${register:-BLOCKED}" == PASS &&
      "${map:-BLOCKED}" == PASS &&
      "${encode:-BLOCKED}" == PASS &&
      "${pix_fmt:-unknown}" == yuv444p ]]; then
    printf 'gate_b_prime_verdict=PASS\n'
    printf 'production-4k60: false\n'
    exit 0
fi

printf 'gate_b_prime_verdict=BLOCKED\n'
printf 'production-4k60: false\n'
exit "${probe_rc:-3}"
