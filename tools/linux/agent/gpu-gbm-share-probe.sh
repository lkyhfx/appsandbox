#!/usr/bin/env bash
# Run beside gbm-shared-export-probe in the target Guest.
set -uo pipefail
out=${1:-gpu-gbm-share-results}
mkdir -p "$out"
failed=0
for node in /dev/dri/card1 /dev/dri/renderD128; do
    name=$(basename "$node")
    timeout 30s appsandbox-gpu ./gbm-shared-export-probe "$node" >"$out/$name.log" 2>&1
    result=$?
    printf 'EXIT_CODE=%s\n' "$result" >>"$out/$name.log"
    cat "$out/$name.log"
    if [ "$result" -ne 0 ]; then failed=1; fi
done
exit "$failed"
