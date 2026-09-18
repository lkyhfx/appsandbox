#!/usr/bin/env bash
# Run beside d3d12-cross-process-share-probe in the target Linux Guest.
# The probe itself enforces 3840x2160 BGRA8, three slots, 3600 frames, and
# 60-Hz pacing.  GPU_RUNNER may be set to an empty string for direct launch;
# the default appsandbox-gpu wrapper is preferred on GPU-PV guests.
set -uo pipefail

out=${1:-gpu-d3d12-cross-process-share-results}
mkdir -p "$out"
log="$out/cross-process-share.log"
runner=${GPU_RUNNER:-appsandbox-gpu}
d3d12_libdir=${D3D12_LIBDIR:-/opt/appsandbox/wsl-deps}

# The Guest image keeps Microsoft's libd3d12/libdxcore runtime outside the
# system linker cache. Preserve any caller-provided path and let
# appsandbox-gpu append its Mesa/WSL runtime paths below it.
if [[ -d "$d3d12_libdir" ]]; then
    export LD_LIBRARY_PATH="$d3d12_libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

set +e
if [[ -n "$runner" ]]; then
    timeout 120s "$runner" ./d3d12-cross-process-share-probe >"$log" 2>&1
else
    timeout 120s ./d3d12-cross-process-share-probe >"$log" 2>&1
fi
rc=$?
set -e

cat "$log"
printf 'EXIT_CODE=%d\n' "$rc"
exit "$rc"
