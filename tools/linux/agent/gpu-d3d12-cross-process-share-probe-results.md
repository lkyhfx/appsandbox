# D3D12 cross-process shared-resource probe

Date: 2026-09-18

## Scope

This probe implements the first architecture gate requested for AppSandbox:

```text
producer process
  3 x 3840x2160 BGRA8 D3D12 shared textures
  producer fence + ready eventfd[3]
       | SCM_RIGHTS over Unix SOCK_SEQPACKET
consumer process
  OpenSharedHandle() on an independent D3D12 device
  GPU-only CopyTextureRegion()
  local fence + done eventfd[3]
```

The default binary invocation is only a launcher. It creates the two roles
with `fork+exec`; D3D12 initialization happens after `exec` in each role, so
the consumer does not inherit the producer's D3D12 device. Resource file
descriptors are closed on both sides after the consumer's successful open
acknowledgement.

## Implemented acceptance checks

| Check | Implementation |
| --- | --- |
| Independent processes | `--producer` and `--consumer` are separate `exec` children |
| Resource FD transfer | `SOCK_SEQPACKET` + `SCM_RIGHTS`, three resource FDs |
| Consumer open | Consumer-owned device calls `OpenSharedHandle()` for all slots |
| Bidirectional synchronization | Producer ready fence/eventfd and consumer done fence/eventfd |
| Wait mechanism | `SetEventOnCompletion()` followed by Linux `poll()`; no completion-value polling |
| Ring buffer | Three slots, one allocator/list and one done eventfd per slot |
| Throughput operation | Shared texture to private default-heap sink with `CopyTextureRegion()` |
| Diagnostic readback | Every 120th frame only; three pixels checked against encoded frame sequence |
| Sustained run | 3840x2160 BGRA8, 3600 frames, 60-Hz pacing |
| Final gates | timeout=0, mismatch=0, resource_reopen_failures=0, 59-61 FPS |
| Metrics | producer-to-consumer wake p50/p95/p99; GPU work min/mean/max/p50/p95/p99; FPS |

The throughput path does not call `mmap`, does not memcpy a framebuffer, and
does not perform GPU-to-CPU-to-GPU transfer. The readback resource is diagnostic
only and is not used by the steady-state operation.

## Build and run

Run these commands inside the target GPU-PV Linux guest:

```sh
cd tools/linux/agent
make d3d12-cross-process-share-probe
bash gpu-d3d12-cross-process-share-probe.sh results-d3d12-cross-process
```

For a direct launch without the guest wrapper:

```sh
GPU_RUNNER= bash gpu-d3d12-cross-process-share-probe.sh results-direct
```

The wrapper allows 120 seconds for startup plus the 60-second paced run. A
successful log must contain the following terminal result and zero counters:

```text
PASS stage=cross-process-resource-fd-transfer slots=3 scm_rights=1 resource_fds=3
PASS stage=cross-process-open-shared-resource slots=3 consumer_device=independent
PASS stage=producer-ready-eventfd slots=3 set_event_on_completion=1
PASS stage=consumer-done-eventfd slots=3 set_event_on_completion=1 scm_rights=1
PASS stage=consumer-gpu-operation copy=GPU-only cpu_framebuffer_copy=0
PASS stage=triple-buffer-reuse slots=3
PASS stage=diagnostic-frame-sequence mismatches=0 checks=30
PASS stage=4k60-sustained
PASS d3d12-cross-process-zero-copy
```

## Execution result

Status: **PASS**

The probe was built and run on the target Guest:

```text
Guest: yunsen@192.168.42.2
Adapter: NVIDIA GeForce RTX 4070
Runtime: DirectX-Headers 1.619.1, /opt/appsandbox/wsl-deps/libd3d12.so,
         /opt/appsandbox/wsl-deps/libdxcore.so
Frames: 3600
FPS: 60.01
Timeouts: 0
Mismatches: 0
Resource reopen failures: 0
Exit code: 0
```

Measured statistics:

```text
wake_latency_us p50=233.484 p95=311.391 p99=441.021 samples=3600
gpu_work_us min=348.615 mean=429.007 max=2973.328 p50=406.666 p95=509.553 p99=783.827 samples=3600
```

The complete captured output is preserved at
`build/d3d12-cross-process-share/cross-process-share.log`.

The Guest image stores the Microsoft D3D12 runtime in
`/opt/appsandbox/wsl-deps`, outside the default linker cache. The Make target
now exposes `D3D12_LIBDIR` (defaulting to that path), and the runner adds the
same directory to `LD_LIBRARY_PATH`; this was required for the real Guest
build and is part of the committed implementation.

This PASS validates the synthetic cross-process resource/synchronization gate
only. It does not yet validate D3D12 Video Encode, Mutter integration, or the
production `appsandbox-display` path. The next gate is therefore the planned
D3D12 shared texture to hardware-encoder capability probe.
