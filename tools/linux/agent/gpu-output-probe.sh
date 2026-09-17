#!/usr/bin/env bash
# Run in the target Guest. Requires gpu-output-probe in the current directory.
# Does not change the installed compositor, KMS state, or service configuration.
set -uo pipefail
out=${1:-gpu-output-results}
mkdir -p "$out"
failed=0
for mode in surfaceless /dev/dri/card1 /dev/dri/renderD128; do
    name=$(basename "$mode")
    timeout 30s appsandbox-gpu ./gpu-output-probe "$mode" >"$out/$name.log" 2>&1
    result=$?
    printf 'EXIT_CODE=%s\n' "$result" >>"$out/$name.log"
    cat "$out/$name.log"
    if [ "$result" -ne 0 ]; then failed=1; fi
done
exit "$failed"
