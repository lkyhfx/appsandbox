#!/usr/bin/env bash
# One-frame isolation probe for the real Mutter framebuffer path.
#
# Each mode starts a fresh headless Mutter session and asks the patched Mesa
# producer to do exactly one post-flush diagnostic operation.  The producer
# submits an independent capture list and fence on the same D3D12 queue, waits
# for it, and logs
# GetDeviceRemovedReason() immediately afterwards.
set -uo pipefail

out=${1:-gpu-mutter-d3d12-isolation-results}
mkdir -p "$out"
socket_path="/run/appsandbox/display-d3d12.sock"
consumer_bin=${MUTTER_CONSUMER_BIN:-./d3d12-mutter-consumer}
runner=${GPU_RUNNER:-appsandbox-gpu}
d3d12_libdir=${D3D12_LIBDIR:-/opt/appsandbox/wsl-deps}
mesa_probe_prefix=${MESA_PROBE_PREFIX:-}
wayland_display_base=${MUTTER_WAYLAND_DISPLAY:-asb-gpu-isolation}

if [[ -z "${MUTTER_TEST_CLIENT_CMD:-}" ]]; then
    printf 'BLOCKED stage=copy-isolation reason=MUTTER_TEST_CLIENT_CMD-not-set\n'
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

mkdir -p "$(dirname "$socket_path")" || {
    printf 'BLOCKED stage=copy-isolation reason=unwritable-socket-directory\n'
    exit 2
}

needs_consumer() {
    [[ "$1" == postflush-shared-copy-consumer ]]
}

valid_mode() {
    case "$1" in
        postflush-local-copy|postflush-shared-copy-no-consumer|postflush-shared-copy-consumer)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

status=0
if [[ -n "${ASB_ISOLATION_MODES:-}" ]]; then
    read -r -a modes <<<"$ASB_ISOLATION_MODES"
else
    modes=(postflush-local-copy postflush-shared-copy-no-consumer postflush-shared-copy-consumer)
fi
if [[ "${#modes[@]}" -eq 0 ]]; then
    printf 'BLOCKED stage=copy-isolation reason=no-modes\n'
    exit 2
fi
for mode in "${modes[@]}"; do
    if ! valid_mode "$mode"; then
        printf 'BLOCKED stage=copy-isolation reason=invalid-mode:%s\n' "$mode"
        exit 2
    fi
done
requires_consumer=0
for mode in "${modes[@]}"; do
    if needs_consumer "$mode"; then
        requires_consumer=1
        break
    fi
done
if [[ "$requires_consumer" -eq 1 ]] &&
   [[ ! -x "$consumer_bin" ]]; then
    printf 'BLOCKED stage=copy-isolation reason=missing-consumer:%s\n' "$consumer_bin"
    exit 2
fi
mode_count=${#modes[@]}
declare -A mode_status=()
for mode in "${modes[@]}"; do
    mode_dir="$out/$mode"
    mkdir -p "$mode_dir"
    consumer_log="$mode_dir/consumer.log"
    mutter_log="$mode_dir/mutter.log"
    runner_log="$mode_dir/runner.log"
    client_log="$mode_dir/client.log"
    runtime=''
    consumer_pid=''
    shell_pid=''

    cleanup_mode() {
        set +e
        [[ -n "$shell_pid" ]] && kill "$shell_pid" 2>/dev/null || true
        [[ -n "$consumer_pid" ]] && kill "$consumer_pid" 2>/dev/null || true
        [[ -n "$shell_pid" ]] && wait "$shell_pid" 2>/dev/null || true
        [[ -n "$consumer_pid" ]] && wait "$consumer_pid" 2>/dev/null || true
        [[ -n "$runtime" ]] && rm -rf "$runtime"
        rm -f "$socket_path"
    }
    trap cleanup_mode EXIT INT TERM
    rm -f "$socket_path"
    : >"$consumer_log"
    : >"$client_log"

    if needs_consumer "$mode"; then
        set +e
        D3D12_VIDEO_BITSTREAM_PATH="$mode_dir/probe.hevc" \
            "$consumer_bin" "$socket_path" >"$consumer_log" 2>&1 &
        consumer_pid=$!
        set -e
        for _ in $(seq 1 100); do
            [[ -S "$socket_path" ]] && break
            sleep 0.1
        done
        if [[ ! -S "$socket_path" ]]; then
            printf 'FAIL mode=%s stage=consumer-listener\n' "$mode" | tee "$runner_log"
            mode_status["$mode"]='FAIL'
            status=1
            cleanup_mode
            trap - EXIT INT TERM
            continue
        fi
    fi

    runtime=$(mktemp -d "/tmp/asb-mutter-copy-probe-${mode}-XXXXXX")
    chmod 700 "$runtime"
    set +e
    timeout -k 5s "${ASB_ISOLATION_TIMEOUT_SECONDS:-60}s" \
        dbus-run-session -- env \
        XDG_RUNTIME_DIR="$runtime" \
        WAYLAND_DISPLAY="${wayland_display_base}-${mode}" \
        ASB_D3D12_DISPLAY=1 \
        ASB_D3D12_COPY_PROBE="$mode" \
        ASB_SESSION_LOG="$mutter_log" \
        ASB_CLIENT_LOG="$client_log" \
        ASB_PATTERN_CLIENT_FRAMES=1 \
        MUTTER_TEST_CLIENT_CMD="$MUTTER_TEST_CLIENT_CMD" \
        bash -c '
            set -u
            gnome_shell=${GNOME_SHELL_BIN:-gnome-shell}
            "$gnome_shell" --headless --no-x11 \
                --virtual-monitor=3840x2160 --wayland-display="$WAYLAND_DISPLAY" \
                >"$ASB_SESSION_LOG" 2>&1 &
            shell_pid=$!
            for _ in $(seq 1 200); do
                [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] && break
                sleep 0.1
            done
            if [[ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]]; then
                printf "FAIL mode=%s stage=wayland-display\\n" "$ASB_D3D12_COPY_PROBE" >>"$ASB_SESSION_LOG"
                kill "$shell_pid" 2>/dev/null || true
                wait "$shell_pid" 2>/dev/null || true
                exit 1
            fi
            bash -c "$MUTTER_TEST_CLIENT_CMD" >"$ASB_CLIENT_LOG" 2>&1
            client_status=$?
            printf "client_exit=%d\\n" "$client_status" >>"$ASB_CLIENT_LOG"
            kill "$shell_pid" 2>/dev/null || true
            wait "$shell_pid" 2>/dev/null || true
            exit "$client_status"
        ' >"$runner_log" 2>&1
    mutter_status=$?
    set -e

    # The consumer waits for the normal multi-frame stream.  The probe only
    # needs its OpenSharedHandle/ready acknowledgement, so stop it after the
    # one-frame Mutter session has ended.
    if [[ -n "$consumer_pid" ]]; then
        for _ in $(seq 1 20); do
            kill -0 "$consumer_pid" 2>/dev/null || break
            sleep 0.1
        done
        kill "$consumer_pid" 2>/dev/null || true
        wait "$consumer_pid" 2>/dev/null || true
    fi

    cat "$runner_log"
    cat "$mutter_log" 2>/dev/null || true
    cat "$consumer_log" 2>/dev/null || true
    if grep -Eq "ASB_D3D12 postflush_probe mode=${mode} mesa_flush=pass" \
           "$runner_log" "$mutter_log" "$consumer_log" 2>/dev/null &&
       grep -Eq "ASB_D3D12 postflush_probe mode=${mode} capture_submit=pass" \
           "$runner_log" "$mutter_log" "$consumer_log" 2>/dev/null &&
       grep -Eq "ASB_D3D12 postflush_probe mode=${mode} capture_completion=pass .*removed_reason=0x00000000" \
           "$runner_log" "$mutter_log" "$consumer_log" 2>/dev/null; then
        if needs_consumer "$mode" &&
           ! grep -Eq "ASB_D3D12 postflush_probe mode=${mode} .*consumer_ready=pass" \
               "$runner_log" "$mutter_log" "$consumer_log" 2>/dev/null; then
            printf 'FAIL mode=%s stage=consumer-ready\n' "$mode"
            mode_status["$mode"]='FAIL'
            status=1
        elif needs_consumer "$mode" &&
           ! grep -Eq 'stage=cross-process-open-shared-resource .*slots=3' "$consumer_log"; then
            printf 'FAIL mode=%s stage=consumer-open-shared-resource\n' "$mode"
            mode_status["$mode"]='FAIL'
            status=1
        else
            printf 'PASS mode=%s one_frame=1\n' "$mode"
            mode_status["$mode"]='PASS'
        fi
    else
        printf 'FAIL mode=%s stage=device-removed-reason mutter_exit=%d\n' \
            "$mode" "$mutter_status"
        status=1
        mode_status["$mode"]='FAIL'
    fi

    cleanup_mode
    trap - EXIT INT TERM
done

for mode in "${modes[@]}"; do
    printf '%s %s\n' "$mode" "${mode_status[$mode]:-FAIL}"
done

if [[ "$status" -eq 0 ]]; then
    printf 'PASS stage=mutter-copy-isolation modes=%d one_frame_each=1\n' \
        "$mode_count"
else
    printf 'FAIL stage=mutter-copy-isolation\n'
fi
exit "$status"
