# AppSandbox Linux GPU display: probe status and engineering guidance

This directory contains the Linux guest agents and the GPU-PV display probes for
AppSandbox. The synthetic feasibility probes and the production Mutter/Mesa
integration are tracked separately.

This README is the **single source of truth for validated facts and engineering
guidance**. Historical probe-result Markdown files have been consolidated here.
For the production implementation plan, see
[IMPLEMENTATION_PLAN.md](./IMPLEMENTATION_PLAN.md).

## Current conclusion

The architecture feasibility probes passed on the GPU-PV Linux Guest with an
NVIDIA GeForce RTX 4070, but the current production Mutter/Mesa integration is
not validated. Its first real-target `CopyResource` currently ends in
`DXGI_ERROR_DEVICE_HUNG`; isolate that producer operation before making any
4K60 production claim.

```text
Mutter real compositor output (RGBA8) [production path under investigation]
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
historical feasibility 3840x2160 stream
```

## Secure Guest Runtime Update

The agent is not self-updating. New VMs install the independent
`appsandbox-guest-updater` and its transport/watchdog units. The updater owns
the signed bundle, staging, atomic activation, reboot recovery, and local
rollback; `appsandbox-agent` exposes only bounded fixed update commands.

Bundle layout:

```text
manifest.json
manifest.sig
payload/bin/...
payload/libexec/...
payload/systemd/...
payload/gnome/...
payload/graphics/wsl-mesa.tar.zst
payload/config/asb_drm.conf
```

Schema 1 manifests contain `version`, `commit`, `arch`, `os`, protocol range,
minimum updater, `graphics_version`, `reboot_required`, and one
`path/sha256/size/mode/component` record per payload file. Ed25519 verification,
path/type checks, hash validation, and size limits happen in the guest. The
line-oriented control channel never carries bundle bytes.

`current` and `previous` select versioned runtime directories. Mesa uses the
equivalent `/opt/wsl-mesa/releases` layout. The watchdog checks agent/display
health and can restore both pointers if the target agent or display service
fails, even when the host cannot reconnect. The intended 4K60 path is
Mutter final target -> GPU-only CopyResource -> 3-slot shared D3D12 ring ->
independent consumer -> GPU-only RGBA8/NV12 -> D3D12 HEVC, but the current
production producer is blocked at its first real-target copy. `dxgkrnl.ko` and
`asb_drm.ko` are intentionally outside normal Guest Runtime updates.

The checked-in Mesa tarball is explicitly marked `production-4k60: false` in
its `BUILDINFO`; it is a legacy provisioning artifact and must be rebuilt from
the production patchset before a signed 4K60 bundle is shipped.

Historical feasibility-probe result (not current production evidence):

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

This closes the synthetic/architecture feasibility phase only. The current
production Mutter integration remains blocked by the real-target first-copy
`DXGI_ERROR_DEVICE_HUNG`; the three-mode post-flush producer isolation probe
is the next acceptance gate. Do not use the historical 3600-frame result as
production validation.

## What "zero-copy" means here

The historical feasibility claim is **zero CPU framebuffer copy / GPU-resident
video path**. It is not yet a production acceptance result.

It means:

- compositor pixels do not enter CPU memory for transport;
- no framebuffer `mmap` is used by the accelerated path;
- no CPU `memcpy` moves framebuffer pixels;
- no GPU -> CPU -> GPU round-trip is used;
- RGBA/BGRA -> NV12 conversion stays on the GPU;
- producer and encoder are independent processes sharing native D3D12
  resources.

It does **not** mean that the pipeline performs zero GPU copies. The prototype
Mutter integration attempts one explicit GPU `CopyResource` from
the compositor-owned render target into the shared three-slot ring:

```text
Mutter private GPU target -> shared D3D12 ring
                      gpu_copy=1
```

That copy is currently the production blocker: the real-target first-copy probe
reports `DXGI_ERROR_DEVICE_HUNG`. A future direct-shared render target can be
considered only after the producer isolation result is understood.

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
Issue #4 therefore keeps CUDA/NVENC as an isolated Gate A/B diagnostic only;
the probe is not wired into the production daemon and must never add a CPU
staging fallback.

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

### 5. Synthetic cross-process shared-resource + eventfd 4K60 gate passed

The cross-process probe uses independent `fork+exec` producer and consumer
roles, so the consumer does not inherit the producer D3D12 device. This is a
synthetic producer gate; it does not exercise the real Mutter framebuffer or
the current Mesa Gallium command list.

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

### HEVC 4:4:4 capability probe

The same opt-in executable also contains a diagnostic-only HEVC Main 4:4:4
gate. It does not alter the default no-argument workload or the production
NV12 path. Build and run it in the guest with:

```sh
make d3d12-video-encode-probe
./d3d12-video-encode-probe --capability-444
./d3d12-video-encode-probe --workload-444
```

`--capability-444` checks the real D3D12 Video HEVC Main 4:4:4 profile,
driver-supported HEVC1 codec/picture configuration, AYUV input, GPU-only
RGBA/BGRA -> AYUV Video Processor conversion, and encoder/heap creation. Its
`hevc444_4k60_config=1` result is an object/configuration gate only;
`hevc444_4k60_sustained` is `not-run`. `--workload-444` explicitly selects the
probe-only `Hevc444` mode, keeps the production wrapper on `Hevc420`, runs the
existing independent producer/consumer workload for 3600 frames at 60/1, and
writes the diagnostic stream to:

```text
/tmp/appsandbox-hevc444-probe.hevc

For the 4:4:4 workload, VPS/SPS/PPS are built from the runtime HEVC1
configuration and HEVC1 picture-control values. The probe reparses those
headers and compares profile/level, SPS/PPS flags, CU/TU limits, and picture
control values before reporting `sequence_header_runtime_config_match=1`; the
capability-only mode deliberately reports the sequence-header fields as
`not-run`.
```

The workload reports `cpu_conversion=0`, `framebuffer_mmap=0`, and
`cpu_memcpy_framebuffer=0`; it intentionally does not read source framebuffer
pixels back to the CPU. `guest_sequence_header_444=1` only proves that the
guest-owned VPS/SPS/PPS has `chroma_format_idc=3` and the expected dimensions;
`guest_bitstream_generated=1` proves that the requested encode workload wrote
frames without encode failures; `guest_encode_failures=0` reports that count
explicitly. Neither is a decoder result. The Windows host probe below owns
actual decodability.

There are four distinct evidence levels:

- capability: support queries such as HEVC Main 4:4:4 and AYUV;
- object creation: encoder/heap, decoder/heap, and processor objects;
- actual encode/decode: completed GPU work or real `ProcessInput`/
  `ProcessOutput` on the guest stream;
- end-to-end path: decoded GPU-resident AYUV passed through a real
  `VideoProcessorBlt` to RGB.

The standalone Windows host probe consumes the guest stream:

```text
appsandbox-hevc444-probe.exe [--adapter <index>] <path-to-hevc444-stream>
```

It tests every enumerated hardware HEVC MFT independently. `HEVC444_PATH=MF`
is allowed only after real hardware decode succeeds, an `IMFDXGIBuffer` yields
an `ID3D11Texture2D` whose format is `DXGI_FORMAT_AYUV`, and the decoded
surface passes the actual AYUV -> RGB `VideoProcessorBlt`. D3D12 capability or
decoder/heap creation alone never produces `HEVC444_PATH=D3D12`; the D3D12
backend must complete an actual decode, GPU AYUV -> BGRA process, and shared
surface open. A `BLOCKED` result is a runtime capability fact, not a request
to enable a fallback or change the production display protocol.

The Windows probe parses Annex-B NAL units into the first complete IRAP access
unit (including prefix SEI and all slices up to the next picture). It supplies
only that AU as the first MF sample, with time `0`, duration `1/60`, and a clean
point marker; the full 3600-frame guest file is never submitted as one sample.

The probe-only CPU regression tests run with:

```sh
python3 tools/linux/agent/test_hevc_headers.py
```

The matching Windows CI job builds only
`tools/win/hevc444-probe/hevc444-probe.vcxproj` in Release/x64. It does not run
the hardware probe.

### Issue #4 gates: A/B/C evidence and decision

Issue #4 makes the production decision explicit. A capability query or a
synthetic workload is not sufficient evidence for `production-4k60`.

Gate A is the independent Guest-native CUDA/NVENC test:

```sh
make nvenc-hevc444-probe \
  NV_CODEC_HEADERS=/path/to/nv-codec-headers/include
bash gpu-nvenc-hevc444-probe.sh 600 gate-a-600
bash gpu-nvenc-hevc444-probe.sh 3600 gate-a-3600
```

`nvenc-hevc444-probe` dynamically loads `libcuda.so.1` and
`libnvidia-encode.so.1`, registers CUDA device memory as
`NV_ENC_BUFFER_FORMAT_YUV444`, requests `NV_ENC_HEVC_PROFILE_FREXT_GUID`, and
writes a HEVC elementary stream. The runner uses ffmpeg/ffprobe to require
3840x2160, 4:4:4 output, zero decode errors, and the requested frame count.
This gate does not test Mutter or D3D12 interop.

Gate B is deliberately fail-closed:

```sh
make d3d12-cuda-nvenc-interop-probe \
  NV_CODEC_HEADERS=/path/to/nv-codec-headers/include
bash gpu-d3d12-cuda-nvenc-interop-probe.sh gate-b
```

It reports `d3d12_cuda_import`, `gpu_rgb_to_yuv444`,
`cuda_nvenc_register`, `cuda_nvenc_map`, `real_mutter`,
`cpu_framebuffer_copy`, `cpu_conversion`, and `cpu_upload`. The current
implementation may report `BLOCKED` at CUDA texture import or the CUDA
RGB-to-YUV444 boundary; that is an honest Gate B failure, not permission to
add CPU readback or upload.
The real Mutter B4 workload remains separate and cannot be replaced by the
synthetic texture in this probe.

Gate C is the Host D3D12 production decoder path. It now owns a fixed three-slot
ring with persistent decode/process queues, processor, fences, bitstream
uploads, AYUV outputs, RGB shared surfaces, command allocators/lists, and
opened D3D11 presentation textures. The production harness prints the required
throughput and failure fields:

```powershell
.\tools\win\hevc444-probe\build-production-probe.ps1
.\build\issue3\production-backend-probe.exe 600
.\build\issue3\production-backend-probe.exe 3600
```

The decoder path never creates a queue, video processor, fence, shared handle,
or opened D3D11 resource per frame. If any Gate A/B/C requirement is missing,
the decision remains `HIGH_PERFORMANCE_HEVC420` or `NOT_RUN`; this repository
continues to publish:

```text
production-4k60: false
```

Validation snapshot for this implementation: Gate A passed both 600 and 3600
frame runs, with `3840x2160 yuv444p`, zero encode/decode failures, and full
frame counts. Gate B is `BLOCKED` at
`d3d12_cuda_import: opaque-fd-texture-import`; B2/B3/B4 remain blocked and no
CPU fallback is used. Gate C passed both 600 and 3600 frame runs with zero
decode/process/present failures, `device_removed=0`, three persistent slots,
and all per-frame creation/copy metrics at zero. The resulting decision is
`HIGH_PERFORMANCE_HEVC420`; `production-4k60` remains false.

### 7. Current real Mutter output gate is blocked

The isolated Mutter/Mesa integration reaches the real compositor render target:

```text
DXGI_FORMAT_R8G8B8A8_UNORM = 28
synthetic_source=0
```

The Mesa D3D12 hook then attempts a GPU-only `CopyResource` into the shared
ring. The current production result is:

```text
FAIL: real Mutter target first CopyResource
DXGI_ERROR_DEVICE_HUNG
production-4k60: false
```

The historical feasibility result is retained only as historical evidence:

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

### 8. Historical producer `CopyResource` isolation probe

The original five-mode result is retained as historical evidence:

```text
ring-only                 PASS: producer fence; consumer OpenSharedHandle
barrier-only              PASS: producer fence; removed_reason=0x00000000
local-copy                FAIL: completion timeout; Mutter session timed out
shared-copy-no-consumer   FAIL: completion timeout; Mutter session timed out
shared-copy-consumer      FAIL: completion timeout; consumer ready timed out
```

The historical modes are not the post-flush acceptance matrix. The updated
runner accepts only the following three one-frame modes, each in a fresh Mutter
session:

```sh
make d3d12-mutter-consumer
MUTTER_TEST_CLIENT_CMD='...' \
  ./gpu-mutter-d3d12-isolation-probe.sh gpu-mutter-d3d12-isolation-results
```

```text
postflush-local-copy                 PASS|FAIL
postflush-shared-copy-no-consumer    PASS|FAIL
postflush-shared-copy-consumer      PASS|FAIL
```

The Mesa hook prepares the real framebuffer before `d3d12_flush_cmdlist()` and
submits an independent `D3D12_COMMAND_LIST_TYPE_DIRECT` capture list on the
same device and `screen->cmdqueue` after Mesa's compositor list is submitted.
The capture list uses direct D3D12 barriers, keeps an explicit COM reference to
the source, and signals a bounded diagnostic capture fence. It does not call
Mesa's resource-state tracker or write capture work to `ctx->cmdlist`.

`postflush-local-copy` uses a non-shared DEFAULT destination and does not
require a consumer socket. `postflush-shared-copy-no-consumer` uses a shared
destination but does not open it in another process. Only
`postflush-shared-copy-consumer` starts the existing consumer and exercises the
ready/consumer-done slot ownership protocol.

The post-flush logs include source resource description and heap properties,
allocator/list reset and close status, queue signal HRESULT, capture completion,
and `GetDeviceRemovedReason()`. Unknown `ASB_D3D12_COPY_PROBE` values and
unknown `ASB_ISOLATION_MODES` values fail closed.

The historical five-mode output remains:

```text
ring-only                 shared ring + consumer OpenSharedHandle; no source operation
barrier-only              real source state transition; no CopyResource
local-copy                real source -> same-device non-shared destination
shared-copy-no-consumer   real source -> shared destination; no consumer use
shared-copy-consumer      current complete producer/consumer path
```

The first mode reporting a failed diagnostic fence, timeout, or
`DXGI_ERROR_DEVICE_HUNG` identifies the failing boundary. The diagnostic
environment variable is opt-in and does not alter the normal production path.

The first real-Mutter run on the GPU Guest (Mutter 50.1, Mesa commit `7f1ccad`,
one frame per mode) produced this boundary result:

```text
ring-only                 PASS: producer fence; consumer OpenSharedHandle
barrier-only              PASS: producer fence; removed_reason=0x00000000
local-copy                FAIL: completion timeout; Mutter session timed out
shared-copy-no-consumer   FAIL: completion timeout; Mutter session timed out
shared-copy-consumer      FAIL: completion timeout; consumer ready timed out
```

The copy-failure modes returned `removed_reason=0x00000000` at the immediate
diagnostic read, so the hang is observable as queue completion failure before
the device-removal reason becomes `DXGI_ERROR_DEVICE_HUNG`. This isolates the
first failure to the real-target `CopyResource` path in the current Gallium
command list; shared-destination and consumer ownership are downstream.

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

### Keep the compositor-to-ring GPU copy as the current probe target

The intended baseline is:

```text
private compositor target
 -> GPU CopyResource
 -> shared ring
```

Do not first redesign Mutter allocation to make its render target directly
shared. First isolate the current real-target copy failure; direct sharing is a
later optimization only if the producer path is stable and measured.

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
- `gpu-mutter-d3d12-isolation-probe.sh` — three one-frame post-flush
  real-target producer isolation modes with an independent capture fence; the
  README retains the original five-mode result as historical evidence.
- `../wsl-mesa/patches/0002-d3d12-mutter-appsandbox-share.patch` — Mesa
  D3D12 publisher prototype.
- `mutter-appsandbox-display.patch` — deterministic AppSandbox Mutter
  post-paint hook (`ASB_D3D12_DISPLAY=1`).

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

The current repository status is therefore:

```text
BLOCKED: Mutter real-target CopyResource -> DXGI_ERROR_DEVICE_HUNG
         production-4k60: false; synthetic feasibility probes remain PASS.
```
