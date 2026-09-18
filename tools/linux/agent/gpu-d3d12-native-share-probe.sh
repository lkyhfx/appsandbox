#!/usr/bin/env bash
# Run beside d3d12-native-share-probe in the target Linux Guest.
set -uo pipefail

out=${1:-gpu-d3d12-native-share-results}
mode=${2:-}

case "$mode" in
    ""|--cpu-sync-fallback|--cross-adapter-fence) ;;
    *)
        printf 'Usage: %s [output-directory] [--cpu-sync-fallback|--cross-adapter-fence]\n' "$0" >&2
        exit 2
        ;;
esac

mkdir -p "$out"
log="$out/native-share.log"

args=()
if [[ -n "$mode" ]]; then
    args+=("$mode")
fi

set +e
timeout 30s appsandbox-gpu ./d3d12-native-share-probe "${args[@]}" >"$log" 2>&1
rc=$?
set -e

cat "$log"
printf 'EXIT_CODE=%d\n' "$rc"
exit "$rc"
