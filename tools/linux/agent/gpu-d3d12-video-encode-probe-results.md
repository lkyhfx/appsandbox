# D3D12 shared texture to hardware encode probe

## Target and execution

- Guest: `192.168.42.2`
- GPU: `NVIDIA GeForce RTX 4070` GPU-PV
- D3D12 runtime: `/opt/appsandbox/wsl-deps/libd3d12.so`
- Input: `3840x2160 BGRA8`, three cross-process shared textures
- Output: HEVC Annex-B stream with host-owned VPS/SPS/PPS headers
- Run: 3600 frames at a 60 Hz producer cadence
- Processes: independent producer/consumer `fork + exec`; no inherited D3D12 device

## Gate 1: capability — PASS

The Guest reports:

```text
PASS stage=d3d12-video-device-interface version=3
PASS stage=d3d12-video-codec codec=H264 supported=1
PASS stage=d3d12-video-codec codec=HEVC supported=1
PASS stage=d3d12-video-nv12-input codec=HEVC format=NV12 supported=1
PASS stage=d3d12-video-4k-resolution codec=HEVC supported=1
PASS stage=d3d12-video-60fps-configuration codec=HEVC supported=1 validation=0x00000000
PASS stage=d3d12-video-objects-constructed codec=HEVC encoder=1 heap=1 resolution=3840x2160 input=NV12 fps=60/1
PASS d3d12-video-encode-capability codec=HEVC resolution=3840x2160 input=NV12 fps=60/1
```

## Gate 2: GPU data path — PASS

The final run passed the shared-resource and encode path:

```text
PASS stage=cross-process-resource-fd-transfer slots=3 scm_rights=1 resource_fds=3
PASS stage=cross-process-open-shared-resource slots=3 consumer_device=independent
PASS stage=resource-transport-fd-close side=consumer count=3
PASS stage=bgra-to-nv12-gpu-only support=1 cpu_conversion=0
PASS stage=consumer-gpu-operation bgra_to_nv12=GPU-only d3d12_encode=GPU-only cpu_framebuffer_copy=0
PASS stage=hevc-sequence-headers vps=1 sps=1 pps=1 bytes=82 host_generated=1 framebuffer_bytes=0
PASS stage=consumer-done-eventfd slots=3 set_event_on_completion=1 scm_rights=1
PASS stage=producer-ready-eventfd slots=3 set_event_on_completion=1
PASS stage=triple-buffer-reuse slots=3
PASS stage=diagnostic-frame-sequence mismatches=0 checks=30
```

The final asynchronous three-slot run produced:

```text
frames=3600 fps=60.00 timeouts=0 mismatches=0 resource_reopen_failures=0
encoded_frames=3600 encoded_bytes=6822102 encode_failures=0
bgra_to_nv12_us p50=193.040 p95=234.464 p99=287.389 samples=3600
encode_submit_us p50=283.129 p95=393.997 p99=493.578 samples=3600
encode_complete_us p50=47971.267 p95=48416.334 p99=48556.940 samples=3600
total_consumer_pipeline_us p50=49906.700 p95=50062.553 p99=50269.536 samples=3600
wake_latency_us p50=229.678 p95=299.384 p99=434.841 samples=3600
PASS stage=4k60-sustained
PASS stage=throughput-zero-copy mmap_framebuffer=0 cpu_memcpy_framebuffer=0 gpu_cpu_gpu=0
PASS d3d12-shared-texture-hardware-encode-payload
```

`encode_complete_us` and `total_consumer_pipeline_us` are observed when a
three-buffer slot is recycled, so they include ring depth and scheduling. The
conversion and submit measurements are per-frame measurements. The only
readback in this probe is encoded bitstream/metadata output; the BGRA input
does not go through CPU memory.

## Decode gate — PASS

The D3D12 encoder returns picture payload NAL units; sequence headers are
host-owned. The probe now prepends a fixed 82-byte VPS/SPS/PPS header matching
its fixed configuration: HEVC Main, 3840x2160, level 5.1, 32x32 CTU, 8x8
minimum CU, and 8-bit POC. The header contains no framebuffer pixels.

The final stream was pulled from the Guest and checked with FFmpeg/FFprobe:

```text
ffprobe: 3840,2160,3600
ffmpeg: exit=0
```

The [D3D12 H.264/HEVC encoding specification](https://microsoft.github.io/DirectX-Specs/d3d/D3D12_Video_Encoding_H264_HEVC.html)
also makes this host-header responsibility explicit. The Guest runner itself
reported `BLOCKED stage=decoded-hevc reason=ffmpeg-or-ffprobe-not-found`
because FFmpeg is not installed there; the same final bitstream was decoded on
the development host with zero errors.

## Final result

```text
PASS d3d12-shared-texture-hardware-encode
decoded_frames=3600
resolution=3840x2160
bitstream_decode_errors=0
```

The D3D12 shared texture + GPU-only conversion + D3D12 hardware encode gate is
now passed. The next step may be an isolated Mutter render-target adapter;
`appsandbox-display`'s existing `mmap + send_all()` path should remain
unchanged until that integration is separately validated.
