# HEVC444 capability matrix

The probes keep the three capability layers separate. A `0` in a D3D12
profile query means that the selected D3D12 path did not expose that profile;
it does not prove that the physical NVIDIA hardware cannot encode YUV444.

## 2026-09-20 validation snapshot

| Layer | Main444 / YUV444 | 3840x2160@60 | Evidence |
| --- | ---: | ---: | --- |
| Windows Host native D3D12 encode | 0 / 0 | 0 | `tools/win/hevc444-encode-probe` feature queries; HEVC codec=1, resource requirements=1, profile/config/AYUV=0 |
| Windows Host MF decode + D3D11 presentation | BLOCKED | BLOCKED | bundled sample reached real probe; this RTX 4070 reported hardware HEVC MFT count=0 |
| Windows Host native D3D12 decode | 1 / 1 | capability-only | standalone host probe created the HEVC Main444 decoder/heap and reported AYUV; actual decode not tested |
| Linux Guest GPU-PV D3D12 encode | 0 | 0 for Main444 | existing Guest validation recorded `supported=0`; standard HEVC Main/NV12 4K60 capability is PASS |
| Windows native NVENC | BLOCKED | BLOCKED | NVIDIA Video Codec SDK is not installed; the optional probe does not infer support from DLL loading |

The current matrix is therefore:

```text
Host D3D12 encode = 0
Guest D3D12 encode = 0
NVENC = BLOCKED
```

This is a D3D12/GPU-PV capability gap, not a hardware-level "RTX 4070 does
not support HEVC444" conclusion. `production-4k60` remains `false`.
