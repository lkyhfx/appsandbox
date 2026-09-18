# AppSandbox Linux 4K60 D3D12 display: production implementation plan

This document defines the final production implementation after completion of
the GPU feasibility work.

Validated facts, rejected alternatives, zero-copy terminology, and regression
rules live in [README.md](./README.md). This plan intentionally references
those facts instead of duplicating the experiment history.

## 1. Goal

Replace the Linux guest's normal 4K60 frame path:

```text
KMS framebuffer -> PRIME -> mmap -> raw BGRA -> vsock
```

with the validated GPU-resident path:

```text
Mutter/Mesa D3D12 render target
        |
        | GPU CopyResource
        v
shared D3D12 ring
        |
        | native dxg handle / SCM_RIGHTS / eventfd
        v
D3D12 encoder helper
        |
        | D3D12 Video Processor
        | RGBA8/BGRA8 -> NV12
        v
D3D12 HEVC Video Encode
        |
        | compressed Annex-B HEVC
        v
appsandbox-display
        |
        | vsock :2
        v
Windows host
        |
        | hardware HEVC decode
        v
D3D11 GPU texture -> presentation
```

The production baseline keeps the validated compositor-to-shared-ring
`gpu_copy=1`. Removing that GPU copy is an optimization, not a launch
requirement. See
[README: Keep the compositor-to-ring GPU copy for the baseline](./README.md#keep-the-compositor-to-ring-gpu-copy-for-the-baseline).

## 2. Non-negotiable constraints

The production implementation must preserve the validated invariants in
[README: Acceptance rules for future changes](./README.md#acceptance-rules-for-future-changes).

In particular:

- accelerated steady state must not mmap the desktop framebuffer;
- no CPU framebuffer memcpy;
- no GPU -> CPU -> GPU framebuffer round-trip;
- RGB -> NV12 remains GPU-only;
- producer and encoder remain separate D3D12 processes;
- slot ownership is explicit and bounded;
- accelerated failure has a working raw-frame fallback;
- host/guest compatibility must not require lock-step upgrades.

## 3. Production component split

Do not turn the current probe binary into one monolithic production daemon.

Use three responsibilities.

### 3.1 Mutter/Mesa publisher

Runs in the compositor process through the downstream Mesa D3D12 integration.

Responsibilities:

- observe the real final compositor color target;
- maintain a three-slot `D3D12_HEAP_FLAG_SHARED` ring;
- record GPU-only `CopyResource` into a free slot;
- signal per-frame readiness through local D3D12 fence + eventfd;
- send resource descriptors/handles and frame metadata to the encoder helper;
- recreate the ring when width, height, format, device, or generation changes;
- never perform CPU pixel readback in normal operation.

The publisher must not encode and must not own the host vsock connection.

### 3.2 D3D12 encoder helper

New production helper, proposed binary:

```text
/usr/local/libexec/appsandbox-display-d3d12
```

Responsibilities:

- connect to the Mutter/Mesa publisher Unix socket;
- open shared textures on its own `ID3D12Device`;
- negotiate RGBA8/BGRA8 input format;
- perform D3D12 Video Processor RGB -> NV12;
- perform D3D12 HEVC hardware encode;
- generate codec configuration/VPS/SPS/PPS for the active mode;
- return slot-done events as soon as the shared RGB texture is no longer used;
- send encoded access units and stream configuration to `appsandbox-display`
  over a local Unix `SOCK_SEQPACKET` channel;
- restart cleanly after D3D12/device/publisher failures.

The helper does **not** listen on vsock. Keeping vsock ownership in
`appsandbox-display` makes fallback and cursor serialization simple.

### 3.3 appsandbox-display

Keep `appsandbox-display` as the single owner of guest vsock port 2.

Responsibilities:

- accept the Windows host connection;
- negotiate encoded-video capability;
- forward encoded video from the D3D12 helper when both sides are healthy;
- continue emitting the existing `ASCR` cursor protocol;
- retain the current KMS/PRIME/mmap `ASFR` implementation as fallback;
- switch to fallback on accelerated startup failure;
- expose clear logs/metrics showing which backend is active.

This separation keeps D3D12 runtime failures from removing the working display
service and avoids two processes writing concurrently to one stream socket.

## 4. Guest-local publisher protocol

The current probe ABI is fixed-size and 4K/3600-specific. Replace it with a
versioned production ABI.

Create:

```text
tools/linux/agent/display_d3d12_protocol.h
```

The local protocol should use `AF_UNIX + SOCK_SEQPACKET` and
`SCM_RIGHTS`.

### 4.1 Publisher hello / resource generation

A generation message should carry at least:

```c
struct AsbD3D12ResourceSet {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;

    uint64_t generation;

    uint32_t width;
    uint32_t height;
    uint32_t dxgi_format;
    uint32_t slot_count;

    uint32_t flags;
    uint32_t reserved;
};
```

The packet carries, using `SCM_RIGHTS`:

- one native D3D12 resource FD per slot;
- one ready eventfd per slot.

The helper responds with:

- generation;
- accepted/rejected status;
- one done eventfd per slot.

Close transport resource FDs after successful
`ID3D12Device::OpenSharedHandle()`. Object lifetime is then owned by the
D3D12 resources, as proven by the validation path.

### 4.2 Frame notification

Each captured compositor frame should send:

```c
struct AsbD3D12Frame {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;

    uint64_t generation;
    uint64_t frame_seq;
    uint64_t capture_time_ns;

    uint32_t slot;
    uint32_t flags;
};
```

No pixel data belongs in this message.

### 4.3 Slot state

Production slot state is:

```text
FREE -> GPU_COPY_PENDING -> READY -> CONSUMING -> FREE
```

The publisher may write only to `FREE`.

The encoder may read only `READY`.

Generation teardown must wait for or abandon all old-generation work before
closing its resources.

## 5. Never stall Mutter on encoder backpressure

The experiment used bounded waits to make failures obvious. Production must
not block the compositor for seconds waiting for an encoder slot.

Use a real-time policy:

1. poll/reap completed done eventfds without blocking;
2. if a slot is free, publish the new frame;
3. if all three slots are busy, **drop this capture opportunity**;
4. continue normal compositor rendering.

Do not block GNOME Shell waiting for the encoder.

Record:

- capture opportunities;
- published frames;
- capture drops because ring full;
- maximum consecutive drops.

At healthy 4K60 the validated GPU path should normally keep the ring flowing,
but compositor responsiveness has priority over capture completeness.

## 6. Return a slot after RGB consumption, not after bitstream readback

The probe's full slot lifetime included later encode/readback work. Production
can safely release the shared RGB slot earlier.

Recommended queue ordering:

```text
shared RGBA
   |
D3D12 VIDEO_PROCESS
   |
   +--> signal process_fence
   |       |
   |       +--> SetEventOnCompletion(done_eventfd)
   |             shared RGBA slot can now be reused
   v
private NV12
   |
D3D12 VIDEO_ENCODE
   |
encoded GPU buffer
   |
copy encoded bytes/metadata to CPU-visible output buffer
```

Once the Video Processor has completed reading the shared RGB resource, later
encode work depends only on the helper-owned NV12 resource. This reduces ring
occupancy and protects the compositor from encoder latency.

## 7. Dynamic mode and format handling

Remove all probe constants for:

- 3840x2160;
- 3600 frames;
- fixed lifetime;
- fixed diagnostic pattern.

Supported production source formats initially:

```text
DXGI_FORMAT_R8G8B8A8_UNORM = 28
DXGI_FORMAT_B8G8R8A8_UNORM = 87
```

Use the actual source format in
`D3D12_FEATURE_VIDEO_PROCESS_SUPPORT` and
`D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC`.

On any change to:

- width;
- height;
- DXGI format;
- D3D12 device;
- compositor resource generation;

perform:

```text
stop publishing old generation
 -> create new shared ring
 -> encoder opens new generation
 -> recreate Video Processor/NV12/encoder state as required
 -> emit new host codec configuration
 -> force IDR
 -> resume
```

Do not reinterpret RGBA as BGRA.

## 8. Production HEVC configuration

Initial production codec:

```text
HEVC Main, 8-bit
NV12
low-latency IP structure
no B frames
```

Use a configurable target FPS, default 60.

Use a low-latency GOP, for example one IDR every 1-2 seconds, and force IDR on:

- new host connection;
- decoder reset;
- resolution change;
- encoder restart;
- transport discontinuity.

Bitrate should be configurable rather than compiled into the binary. Start
with a 4K60 profile appropriate for the local Hyper-V/vsock transport and tune
from measured quality/latency.

### Sequence headers

The probe used fixed VPS/SPS/PPS for one known configuration. Production must
generate parameter sets from the active:

- codec profile;
- level/tier;
- width/height;
- coding-unit configuration;
- GOP/reference configuration.

Do not keep a hard-coded 3840x2160 header blob as the production solution.

## 9. Host/guest display protocol v2

The current host `src/backend_win/vm_display_idd.c` expects raw `ASFR`
BGRA frames and keeps a CPU-side `frame_buf`. Sending HEVC without a protocol
extension would simply move the bottleneck.

Keep port 2 and make the connection backward-compatible.

### 9.1 Host capability hello

On a new frame-channel connection, a new host sends a small client hello:

```c
#define ASDH_MAGIC 0x48445341u /* "ASDH" */

struct DisplayHostHello {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t capabilities;
    uint32_t max_width;
    uint32_t max_height;
};
```

Initial capability bits:

```text
RAW_ASFR
HEVC_D3D11_HW_DECODE
```

Compatibility behavior:

- new guest + new host: negotiate HEVC when accelerated backend is healthy;
- new guest + old host: no hello arrives -> use legacy ASFR;
- old guest + new host: the hello remains unread by the old one-way sender and
  the host still accepts ASFR.

Use a short hello deadline. Lack of hello must not hang connection startup.

### 9.2 Encoded stream configuration

Use a separate message, e.g. `ASVC`:

```c
struct EncodedVideoConfig {
    uint32_t magic;       /* ASVC */
    uint16_t version;
    uint16_t header_size;

    uint64_t generation;

    uint32_t codec;       /* HEVC initially */
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;

    uint32_t flags;
    uint32_t extradata_size;
};
/* followed by Annex-B VPS/SPS/PPS */
```

A new `generation` invalidates decoder state from the previous generation.

### 9.3 Encoded frame

Use a message such as `ASVE`:

```c
struct EncodedVideoFrame {
    uint32_t magic;       /* ASVE */
    uint16_t version;
    uint16_t header_size;

    uint64_t generation;
    uint64_t frame_seq;
    uint64_t capture_time_ns;

    uint32_t flags;       /* IDR, DISCONTINUITY */
    uint32_t payload_size;
};
/* followed by exactly one encoded access-unit payload */
```

Keep `ASCR` unchanged for cursor updates and `ASFR` unchanged for raw
fallback.

The host receive loop must dispatch by magic and maintain strict message
length limits.

## 10. Windows host hardware decode

Add an HEVC hardware-decoder path to `src/backend_win/vm_display_idd.c` or,
preferably, a new module:

```text
src/backend_win/vm_video_decode.c
src/backend_win/vm_video_decode.h
```

Recommended baseline:

- Media Foundation hardware HEVC decoder;
- `IMFDXGIDeviceManager` backed by the existing D3D11 device;
- decoder output as GPU-resident NV12 DXGI surfaces;
- D3D11 Video Processor or an NV12 shader to the existing render target.

The accelerated host path must not copy every decoded 4K frame through a CPU
`frame_buf`.

Only advertise `HEVC_D3D11_HW_DECODE` after decoder creation/capability
validation succeeds. If hardware decode is unavailable, advertise only raw
ASFR and let the guest select fallback.

### Host decoder lifecycle

On `ASVC` generation change:

```text
flush old decoder
 -> apply new VPS/SPS/PPS
 -> recreate output surfaces if size changed
 -> wait for IDR
 -> resume presentation
```

On decode failure:

1. request/reconnect for a new IDR if recovery is cheap;
2. if accelerated decode cannot recover, reconnect without the HEVC capability
   so the guest falls back to ASFR.

## 11. Latency and queue policy

This is an interactive desktop/game path, not an archival encoder.

Guest:

- never allow an unbounded raw-frame or encoded-frame queue;
- skip future capture opportunities before encoding when downstream is full;
- never intentionally drop an already encoded reference frame from the stream.

Host:

- decode in order;
- keep the presentation queue shallow;
- if rendering falls behind, present the newest decoded surface rather than
  accumulating seconds of latency.

Optional phase-two feedback can add host-present acknowledgements containing
`generation + frame_seq + present_time` for end-to-end latency telemetry.

## 12. Cursor path

Keep existing `ASCR` cursor messages for the first production version.

Cursor shape extraction is tiny and independent of the full desktop video path;
it does not justify delaying the zero-CPU-framebuffer-copy rollout.

The video and cursor messages must be serialized by the single
`appsandbox-display` vsock writer.

A later cleanup may move cursor metadata directly out of Mutter, but that is
not part of the 4K60 video critical path.

## 13. Fallback design

The current raw path in `appsandbox-display.c` remains the fallback described
in [README: Preserve the CPU capture path as fallback](./README.md#preserve-the-cpu-capture-path-as-fallback).

Backend state:

```text
STARTING
  |
  +-- host lacks HEVC capability ------------> RAW
  |
  +-- D3D12 helper unavailable/fails --------> RAW
  |
  +-- publisher unavailable -----------------> RAW
  |
  +-- accelerated init succeeds -------------> HEVC
                                                |
                                                +-- fatal failure -> reconnect/recover
                                                                     |
                                                                     +-> RAW
```

Do not silently oscillate every frame. Use a circuit breaker:

- record the accelerated failure reason;
- fall back for the current host session after repeated fatal failures;
- retry acceleration on a new session or after an explicit cooldown/restart.

Logs must print one clear line such as:

```text
display_backend=hevc-d3d12
```

or:

```text
display_backend=raw-asfr fallback_reason=<reason>
```

## 14. Production code changes

### Phase A: stabilize the publisher ABI

Refactor probe-only code into production files:

```text
tools/linux/agent/display_d3d12_protocol.h
tools/linux/agent/display_d3d12_encoder.cpp
tools/linux/agent/display_d3d12_encoder.h
```

Convert the Mesa experiment patch into a production downstream patch:

```text
tools/linux/wsl-mesa/patches/
  0002-d3d12-appsandbox-share-publisher.patch
```

Remove from the production code path:

- fixed 3600 frame count;
- diagnostic pixel expectations;
- test-only logging;
- 4K-only assumptions;
- long blocking waits inside Mutter.

Keep the probe sources available as tests until production validation is
complete.

### Phase B: add local encoder helper

Build/install:

```text
appsandbox-display-d3d12
```

Add a private Unix socket, for example under:

```text
/run/appsandbox/display-d3d12.sock
```

The helper should use systemd/runtime permissions appropriate for the GNOME
session and D3D12 device access. Avoid world-writable sockets.

### Phase C: extend appsandbox-display transport

Refactor the current display daemon so that:

- one thread owns the vsock write stream;
- encoded frames arrive from the helper;
- raw frames arrive from the existing DRM fallback;
- cursor messages use the same ordered writer;
- host hello selects one video backend per session.

Move duplicated wire structs into a shared protocol header used by guest and
Windows host where practical.

### Phase D: add Windows HEVC decoder

Implement host capability detection, `ASVC`/`ASVE` parsing, hardware
decode, and GPU presentation.

Preserve the existing `ASFR` receive/render path unchanged until encoded
mode passes its full acceptance suite.

### Phase E: make accelerated mode default

After all acceptance gates below pass:

- advertise HEVC by default on capable Windows hosts;
- select accelerated mode by default on capable GPU-PV Linux guests;
- keep an explicit diagnostic switch to force raw fallback.

## 15. systemd and packaging

The current service runs:

```text
/usr/local/bin/appsandbox-display
```

Keep that stable.

Add the D3D12 helper as either:

- a separate systemd service ordered with `appsandbox-display`; or
- a child process supervised by `appsandbox-display`.

A separate service is preferred for crash/restart isolation, but
`appsandbox-display` remains the owner of port 2.

Package the required:

- DirectX-Headers-built helper;
- WSL/GPU-PV `libd3d12.so` / `libdxcore.so` runtime;
- patched Mesa D3D12 driver.

Do not make unrelated input/audio/clipboard agents depend on D3D12 libraries.

## 16. Observability

Expose at least:

```text
backend
mode_generation
width
height
source_dxgi_format
capture_fps
encoded_fps
capture_drops
ring_full_drops
publisher_reconnects
encoder_restarts
resource_reopen_failures
eventfd_timeouts
encode_failures
encoded_bitrate
vsock_bytes_per_sec
host_decoder_resets
host_present_fps
fallback_count
fallback_reason
```

Latency histograms:

```text
mutter_publish_to_encoder_wake_us
rgba_to_nv12_us / bgra_to_nv12_us
encode_submit_us
encode_complete_us
guest_transport_queue_us
host_decode_us
host_present_latency_us
```

Never infer health only from average FPS. Track p95/p99 and failure counters.

## 17. Final acceptance suite

Production accelerated mode is ready only when all of the following pass on
the target GPU-PV Guest and Windows host.

### Functional

- GNOME/Mutter real desktop, not synthetic content;
- 3840x2160 @ 60 Hz;
- 3600-frame minimum smoke run;
- 30-minute dynamic desktop/video run;
- fullscreen video playback;
- fullscreen 3D/game workload;
- cursor movement and cursor shape changes;
- host window resize/minimize/restore;
- guest resolution change;
- guest compositor restart;
- encoder helper restart;
- host disconnect/reconnect.

### Correctness

```text
stale_frames=0
mismatches=0
resource_reopen_failures=0
encode_failures=0
unexpected_decoder_errors=0
```

No persistent green/purple channel swap, tearing, old-generation frame, or
resolution mismatch is allowed.

### Zero-CPU-framebuffer-copy

Accelerated mode must preserve:

```text
framebuffer_mmap=0
cpu_memcpy_framebuffer=0
gpu_cpu_gpu=0
cpu_rgb_to_nv12=0
```

Bitstream and metadata CPU reads are allowed.

### Performance

Required:

- sustained presentation >= 59 FPS for a 60 Hz target;
- no unbounded guest or host queue growth;
- no compositor stalls caused by a full encoder ring;
- p99 capture/convert/submit metrics remain within the 16.67 ms frame budget
  with substantial headroom;
- CPU usage materially below the raw 4K BGRA transport path.

### Recovery

Force each of these failures and confirm visible recovery/fallback:

- kill encoder helper;
- close publisher socket;
- restart GNOME Shell;
- remove/recreate host display connection;
- force D3D12 initialization failure;
- force host HEVC decoder creation failure.

No failure may leave the display permanently black while the raw fallback is
available.

## 18. Rollout sequence

Implement and merge in this order:

1. production local publisher protocol;
2. non-blocking dynamic Mesa publisher;
3. production D3D12 encoder helper;
4. host/guest display capability hello;
5. `ASVC` + `ASVE` transport;
6. Windows hardware HEVC decoder;
7. automatic accelerated/raw backend selection;
8. lifecycle/recovery tests;
9. 4K60 game/video soak tests;
10. make D3D12 HEVC the default capable-Linux path.

Do not optimize away the compositor GPU copy before step 10.

## 19. Deferred optimizations

After production stability:

- direct shared Mutter render target (`gpu_copy=0`);
- larger/smarter ring only if measurements require it;
- dirty-region-aware encode decisions;
- H.264 compatibility mode;
- AV1;
- HDR/P010;
- adaptive bitrate;
- end-to-end present acknowledgements;
- direct Mutter cursor metadata.

These are not prerequisites for the validated 4K60 baseline.

## 20. Definition of done

The implementation is complete when a normal AppSandbox Linux VM boots and,
without test probes or test clients:

1. Mutter publishes real GPU compositor frames;
2. the D3D12 helper consumes shared resources cross-process;
3. RGB -> NV12 and HEVC encode remain GPU-only;
4. encoded frames cross vsock using the negotiated v2 protocol;
5. the Windows host hardware-decodes and presents them without a CPU 4K frame
   copy;
6. real fullscreen video/game workloads hold stable 4K60;
7. failures automatically recover or fall back to legacy ASFR.

At that point the probe architecture described in
[README.md](./README.md) has become the production display path.
