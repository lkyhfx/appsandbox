# HEVC444 capability matrix

The probes keep the three capability layers separate. A `0` in a D3D12
profile query means that the selected D3D12 path did not expose that profile;
it does not prove that the physical NVIDIA hardware cannot encode YUV444.

## 2026-09-20 validation snapshot (after HEVC1 probe fix)

The Host probe now supplies the `PROFILE_LEVEL` min/max backing records,
queries codec configuration through `HEVC1/pHEVCSupport1`, uses
`HEVC1/pHEVCPicData1` for the actual frame, and only reports encode success
after metadata and the generated Annex-B stream pass validation.

| Layer | Profile query | Input format | Codec config | Actual encode/decode | GPU surface | 4K60 | Evidence |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Windows Host native D3D12 encode | 0 | 0 (AYUV) | 0 | BLOCKED (capability gate) | n/a | 0 | RTX 4070 run of `appsandbox-hevc444-encode-probe.exe`; HEVC codec=1, resource requirements=1, profile/config/AYUV=0 |
| Windows Host MF decode + D3D11 presentation | BLOCKED | BLOCKED | n/a | BLOCKED | BLOCKED | BLOCKED | bundled sample reached real MFT probe; hardware HEVC MFT count=0 |
| Windows Host native D3D12 decode | 1 | AYUV=1 | n/a | actual `DecodeFrame` probe | AYUV GPU surface + readback checksum | 3840x2160@60 decode query | bundled IRAP submitted through `ID3D12VideoDecodeCommandList`; result is reported as PASS/BLOCKED at runtime |
| Linux Guest GPU-PV D3D12 encode | 0 (Main444) | 0 (AYUV) | BLOCKED | BLOCKED | n/a | 0 (Main444) | latest re-run unavailable: SSH to `yunsen@192.168.42.2` returned `Permission denied`; prior probe recorded standard Main/NV12 4K60 PASS |
| Windows native NVENC | BLOCKED | BLOCKED | BLOCKED | BLOCKED | n/a | BLOCKED | NVIDIA Video Codec SDK is not installed; DLL loading is not treated as support |

The Host `actual_encode`, sequence-header, and IRAP stages are deliberately
not run when the capability gate is zero. This is a reliable negative result
for the exposed D3D12 path, not a hardware-level conclusion that an RTX 4070
cannot encode HEVC444. `production-4k60` remains `false`.
