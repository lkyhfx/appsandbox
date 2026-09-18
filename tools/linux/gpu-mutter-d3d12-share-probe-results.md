# Isolated Mutter → D3D12 shared ring → HEVC probe

## Result

**BLOCKED - real Mutter target reached; format boundary prevents the requested
BGRA shared-ring proof.**

The Linux guest `yunsen@192.168.42.2` is reachable. The isolated run now
proves all of the following before stopping:

```text
CLIENT_READY width=3840 height=2160 opaque=1 dynamic=1
libmutter-Message: Created gbm renderer for '/dev/dri/renderD128'
MUTTER_D3D12 hook-target has_commands=1 cbuf_texture=0x...
MUTTER_D3D12 ensure-ring source width=3840 height=2160 format=28 flags=0x00000005
BLOCKED stage=mutter-consumer reason=no-producer-frame-within-timeout
```

`format=28` is `DXGI_FORMAT_R8G8B8A8_UNORM` (RGBA8), while the validated
consumer and current producer contract require `DXGI_FORMAT_B8G8R8A8_UNORM`
(BGRA8, value 87). The hook therefore rejects the actual Mutter render target
before creating or sending the shared-resource ring. This is a real format
compatibility blocker, not a missing client, missing renderer, or hung SSH
session.

The isolated direct GBM/GLES diagnostic separately reaches
`renderer=D3D12 (NVIDIA GeForce RTX 4070)` with `GBM_ALWAYS_SOFTWARE=1`, but
DMA-BUF export remains unavailable. The full 4K60/3600-frame HEVC chain is
therefore still not a PASS. No synthetic frame or fake PASS is substituted.

## Experiment artifacts

- `agent/d3d12-share-probe-protocol.h` — shared wire ABI.
- `agent/d3d12-mutter-consumer.cpp` — socket listener using the validated
  D3D12 BGRA8 → GPU NV12 → HEVC consumer.
- `agent/mutter-d3d12-share-probe.patch` — opt-in Mutter post-paint hook.
- `wsl-mesa/patches/0002-d3d12-mutter-native-share-probe.patch` — opt-in Mesa
  d3d12 hook for the real Gallium framebuffer.
- `agent/gpu-mutter-d3d12-share-probe.sh` — isolated session runner.

## Required raw output

When run in the Linux GPU-PV guest, the runner must write these files under
the selected output directory:

```text
mutter-consumer.log
mutter-session.log
runner.log
decode.log
mutter.hevc
```

The run is accepted only if the logs contain all of the following evidence:

```text
PASS stage=mutter-d3d12-renderer
PASS stage=mutter-real-render-target synthetic_source=0
PASS stage=mutter-shared-resource native_d3d12_shared=1 gpu_copy=1 cpu_copy=0
PASS stage=consumer-done-eventfd ... set_event_on_completion=1 scm_rights=1
PASS stage=producer-eventfd-sync busy_poll=0 set_event_on_completion=1 eventfd=1 poll=1
PASS stage=consumer-gpu-operation bgra_to_nv12=GPU-only d3d12_encode=GPU-only cpu_framebuffer_copy=0
PASS stage=consumer-real-desktop-frame diagnostic_points=3 stale_frames=0 mismatches=0
PASS stage=4k60-sustained frames=3600 fps=... timeouts=0 mismatches=0 encode_failures=0
PASS stage=d3d12-hardware-encode frames=3600
PASS stage=decoded-hevc frames=3600 resolution=3840x2160
PASS mutter-d3d12-zero-copy-encode
```

The performance lines must also be present with p50/p95/p99 values for
`mutter_publish_to_consumer_wake_us`, `gpu_copy_us`,
`bgra_to_nv12_us`, `encode_submit_us`, and `slot_recycle_us`.

The runner also validates the real-rendered frame marker at three GPU
readback diagnostic points every 120 frames. Those readbacks are diagnostics
only; the video path remains GPU-only.

## Production-path isolation

No production `appsandbox-display` source, unit, or runtime configuration was
changed. The experiment is activated only by the new isolated runner and the
two opt-in environment variables consumed by the patched Mutter/Mesa builds.
