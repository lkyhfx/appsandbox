# AppSandbox Linux GPU display: final validation and engineering guidance

This directory contains the Linux guest agents and the completed GPU-PV display
validation work for AppSandbox.

This README is the **single source of truth for validated facts and engineering
guidance**. Historical probe-result Markdown files have been consolidated here.
For the production implementation plan, see
[IMPLEMENTATION_PLAN.md](./IMPLEMENTATION_PLAN.md).

## Final conclusion

The target architecture is validated end to end on the GPU-PV Linux Guest with
an NVIDIA GeForce RTX 4070:

```text
Mutter real compositor output (RGBA8)
        |
        | GPU CopyResource
        v
3-slot D3D12_HEAP_FLAG_SHARED ring
        |
        | native dxg shared handle + SCM_RIGHTS
        v
independent encoder process
        |
        | ID3D12Device::OpenSharedHandle()
        | eventfd + SetEventOnCompletion() + poll()
        v
D3D12 Video Processor
        |
        | GPU-only RGBA8 -> NV12
        v
D3D12 Video Encode
        |
        | HEVC
        v
valid 3840x2160 stream
```

The final real-Mutter run validated:

```text
RGBA8 dxgi_format=28
native_d3d12_shared=1
gpu_copy=1
cpu_copy=0
consumer_device=independent
rgba-to-nv12-gpu-only cpu_conversion=0

frames=3600
fps=63.73
stale_frames=0
mismatches=0
encode_failures=0

framebuffer_mmap=0
cpu_memcpy_framebuffer=0
gpu_cpu_gpu=0

decoded=3840x2160,3600 frames
mutter_session_exit=0
consumer_exit=0
```

This closes the architecture feasibility phase. Production work should build on
this path instead of reopening DMA-BUF, Vulkan/CUDA, or CPU framebuffer capture
as the primary design.

## What "zero-copy" means here

The validated claim is **zero CPU framebuffer copy / GPU-resident video path**.

It means:

- compositor pixels do not enter CPU memory for transport;
- no framebuffer `mmap` is used by the accelerated path;
- no CPU `memcpy` moves framebuffer pixels;
- no GPU -> CPU -> GPU round-trip is used;
- RGBA/BGRA -> NV12 conversion stays on the GPU;
- producer and encoder are independent processes sharing native D3D12
  resources.

It does **not** mean that the pipeline performs zero GPU copies. The validated
Mutter integration currently performs one explicit GPU `CopyResource` from
the compositor-owned render target into the shared three-slot ring:

```text
Mutter private GPU target -> shared D3D12 ring
                      gpu_copy=1
```

That copy is acceptable for the production baseline. A future direct-shared
render target can remove it, but that optimization must not block production
integration.

## Validation summary

### 1. Legacy KMS/CPU capture is functional but not the target path

The existing `appsandbox-display.c` captures the primary KMS framebuffer using:

```text
drmModeGetFB2
 -> drmPrimeHandleToFD
 -> mmap
 -> send(vsock)
```

This is a CPU-visible framebuffer path and therefore is **not** the production
zero-copy path. Keep it as a compatibility/fallback path until the accelerated
path is fully deployed.

### 2. DMA-BUF export from Mesa D3D12 is not a viable primary route

The EGL/GBM experiments reached the D3D12 renderer but did not yield a usable
DMA-BUF export:

```text
renderer=D3D12 (NVIDIA GeForce RTX 4070)
export_fd=-1
modifier=DRM_FORMAT_MOD_INVALID
```

Treating a dxg/NT shared object descriptor as a DMA-BUF was explicitly rejected.
Do not build the production design around `EGL_MESA_image_dma_buf_export` or
DRM PRIME export from Mesa D3D12.

### 3. Vulkan opaque-FD -> CUDA interop is not a viable primary route

A Vulkan D3D12 texture could produce an opaque external FD, but the tested CUDA
external-memory import returned `CUDA_ERROR_NOT_SUPPORTED`.

CUDA -> NVENC itself worked, but the missing GPU-resident bridge from the actual
D3D12 compositor resource made this route unsuitable as the main architecture.

Do not reintroduce CUDA/NVENC unless a future platform requirement specifically
demands it.

### 4. Native D3D12 shared resources are viable

The native D3D12 probe proved that a shared texture created with
`D3D12_HEAP_FLAG_SHARED` can be opened by another D3D12 device through the
Linux dxg/NT shared-object FD.

Native shared-fence opening was not supported by the tested
`libd3d12/dxg` combination. The architecture therefore uses event-driven CPU
coordination without moving framebuffer pixels through the CPU:

```text
GPU producer work
 -> D3D12 fence
 -> SetEventOnCompletion(eventfd)
 -> poll(eventfd)
 -> submit consumer GPU work
```

CPU synchronization is not a CPU framebuffer copy.

### 5. Cross-process shared-resource + eventfd 4K60 gate passed

The cross-process probe uses independent `fork+exec` producer and consumer
roles, so the consumer does not inherit the producer D3D12 device.

Validated configuration:

- 3 x 3840x2160 shared textures;
- native resource FDs passed with `SCM_RIGHTS`;
- consumer-owned `ID3D12Device::OpenSharedHandle()`;
- resource transport FDs closed after open acknowledgement;
- producer-ready and consumer-done eventfds;
- triple-buffer reuse;
- 3600 frames at 60 Hz;
- no steady-state framebuffer CPU copy.

Measured target-Guest result:

```text
frames=3600
fps=60.01
timeouts=0
mismatches=0
resource_reopen_failures=0

wake_latency_us p50=233.484 p95=311.391 p99=441.021
gpu_work_us     p50=406.666 p95=509.553 p99=783.827
```

### 6. D3D12 hardware encode gate passed

The target Guest exposes the required D3D12 Video capabilities:

- H.264 and HEVC video encode;
- NV12 encoder input;
- 3840x2160 output;
- 60/1 configuration;
- D3D12 Video Processor RGB -> NV12 conversion.

The shared-texture hardware-encode probe passed 3600 frames with no encode
failures and produced a decodable 3840x2160 HEVC stream.

Representative synthetic-BGRA measurements:

```text
bgra_to_nv12_us p50=193.040 p95=234.464 p99=287.389
encode_submit_us p50=283.129 p95=393.997 p99=493.578
```

The larger `encode_complete_us` / total-pipeline numbers observed by the probe
include three-slot recycling and scheduling depth; they are not a direct
single-frame encoder execution time.

### 7. Real Mutter output gate passed

The isolated Mutter/Mesa integration reached the real compositor render target:

```text
DXGI_FORMAT_R8G8B8A8_UNORM = 28
synthetic_source=0
```

The Mesa D3D12 hook creates a shared ring using the real source format and
records a GPU-only `CopyResource` into that ring. The independent encoder
process opens those resources, converts RGBA8 -> NV12 with D3D12 Video
Processor, and encodes HEVC with D3D12 Video Encode.

The final real-desktop result is:

```text
PASS: real Mutter target
PASS: native D3D12 shared ring
PASS: independent consumer device
PASS: eventfd synchronization
PASS: GPU-only RGBA8 -> NV12
PASS: D3D12 HEVC hardware encode
PASS: 3600 real frames
PASS: no stale/mismatched frames
PASS: zero CPU framebuffer-copy conditions
PASS: 3840x2160 / 3600-frame decode
```

## Production engineering guidance

### Use native D3D12 sharing as the primary data plane

The production accelerated path should be:

```text
Mutter/Mesa D3D12 publisher
 -> shared D3D12 ring
 -> encoder daemon
 -> D3D12 Video Processor
 -> D3D12 Video Encode
 -> host transport
```

Do not route production video through KMS framebuffer mmap, DMA-BUF export,
Vulkan/CUDA external memory, or CPU colorspace conversion when the validated
D3D12 path is available.

### Keep a three-slot ring

Three slots have already been validated and provide enough decoupling for
producer rendering and encoder work. Production code should preserve per-slot
ownership explicitly:

```text
FREE -> PRODUCING -> READY -> CONSUMING -> FREE
```

A producer must never reuse a slot until its consumer-done event has arrived.

Do not serialize all frames through one resource.

### Keep eventfd synchronization

Use:

- D3D12 local fences for queue completion;
- `ID3D12Fence::SetEventOnCompletion()`;
- Linux `eventfd`;
- bounded `poll()`.

Do not busy-poll `GetCompletedValue()` in the steady-state path.

A timeout is a fault and should trigger controlled pipeline restart/fallback,
not indefinite blocking.

### Carry the real resource format in the protocol

The real Mutter source is currently
`DXGI_FORMAT_R8G8B8A8_UNORM (28)`. The protocol already supports both:

- RGBA8: `DXGI_FORMAT_R8G8B8A8_UNORM (28)`;
- BGRA8: `DXGI_FORMAT_B8G8R8A8_UNORM (87)`.

Never reinterpret one byte layout as the other. Configure the D3D12 Video
Processor from the negotiated source format.

### Keep the compositor-to-ring GPU copy for the baseline

The validated baseline is:

```text
private compositor target
 -> GPU CopyResource
 -> shared ring
```

Do not first redesign Mutter allocation to make its render target directly
shared. Direct sharing is a later optimization after the production pipeline is
stable and measured.

### Keep bitstream/metadata CPU visibility separate from framebuffer copying

The encoder output bitstream and encode metadata are expected to become
CPU-visible so they can be packetized/transmitted. That does not violate the
zero-CPU-framebuffer-copy guarantee.

The guarantee applies to source framebuffer pixels and colorspace conversion.

### Treat diagnostic readback as test-only

Pixel readback used by probes to detect stale frames and format errors is
diagnostic only. Production steady state must not read framebuffer pixels back
to the CPU.

### Preserve the CPU capture path as fallback

Do not remove `appsandbox-display.c` immediately.

Recommended runtime policy:

```text
try accelerated D3D12 publisher/encoder
  -> healthy: use accelerated path
  -> unsupported/init failure/fatal runtime failure:
       fall back to legacy CPU capture
```

The fallback should be visible in logs/metrics so performance regressions are
not hidden.

### Handle dynamic lifecycle explicitly

Production code must handle:

- encoder process restart;
- publisher reconnect;
- compositor restart;
- resolution/mode change;
- shared-resource recreation;
- stale SCM_RIGHTS descriptors;
- eventfd closure;
- D3D12 device removal;
- encode queue failure;
- consumer backpressure;
- host disconnect/reconnect.

Do not assume a single 3600-frame lifetime.

## Current important sources

The validated behavior is represented by these implementation/probe sources:

- `appsandbox-display.c` — current legacy KMS/CPU fallback.
- `d3d12-share-probe-protocol.h` — shared-resource probe wire ABI.
- `d3d12-cross-process-share-probe.cpp` — cross-process sharing/eventfd gate.
- `d3d12-video-encode-probe.cpp` — D3D12 Video Processor + HEVC encode gate.
- `d3d12-mutter-consumer.cpp` — isolated Mutter encoder consumer.
- `gpu-mutter-d3d12-share-probe.sh` — real compositor end-to-end gate.
- `../wsl-mesa/patches/0002-d3d12-mutter-native-share-probe.patch` — Mesa
  D3D12 publisher prototype.
- `mutter-d3d12-share-probe.patch` — isolated Mutter post-paint probe hook.

These are validation artifacts, not all of them should be promoted unchanged
into production. The productionization steps are defined in
[IMPLEMENTATION_PLAN.md](./IMPLEMENTATION_PLAN.md).

## Build and existing guest agents

The non-GPU guest agents remain in this directory. Basic build:

```sh
sudo apt-get install -y build-essential
make
```

Install/enable the standard guest service set as supported by the current
Makefile/systemd definitions:

```sh
sudo make enable
```

For troubleshooting:

```sh
systemctl status appsandbox-agent
journalctl -u appsandbox-agent -f
```

GPU probes remain opt-in build targets and should not become mandatory
dependencies of unrelated guest agents.

## Acceptance rules for future changes

A change to the accelerated display pipeline must not regress these invariants:

1. source is the real compositor output, not a synthetic substitute;
2. independent producer/consumer D3D12 devices remain supported;
3. shared-resource transport works cross-process;
4. no framebuffer mmap or CPU framebuffer memcpy appears in accelerated steady state;
5. no GPU -> CPU -> GPU framebuffer round-trip is introduced;
6. RGB -> NV12 remains GPU-only;
7. slot reuse remains synchronized and bounded;
8. a 4K60 sustained run has no stale frames, mismatches, or encode failures;
9. the produced stream decodes to the expected frame count and resolution;
10. accelerated-path failure remains recoverable through explicit restart/fallback.

The baseline architectural result is therefore:

```text
PASS: Mutter -> D3D12 shared ring -> GPU NV12 -> D3D12 HEVC
      at real 4K60 with zero CPU framebuffer copy.
```
