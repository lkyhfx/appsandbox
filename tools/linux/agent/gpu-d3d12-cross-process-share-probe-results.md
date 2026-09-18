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

## Execution status in this workspace

The source, Make target, runner script, and this report are complete. A real
4K60 PASS cannot be claimed from the current Windows workspace: the Linux
DirectX-Headers/libd3d12/libdxcore guest toolchain and `/dev/dxg` runtime are
not present here, and the local WSL distro enumeration is unavailable with
`E_ACCESSDENIED`. Therefore this report intentionally records the runtime
gate as **NOT RUN in this workspace**, rather than fabricating PASS metrics.

The first guest run should append the captured `cross-process-share.log` and
fill in the measured `frames`, `fps`, timeout/mismatch counters, wake latency,
and GPU-work statistics. Until that run passes, this remains a validation
probe and is not evidence to change Mutter or the production display path.
