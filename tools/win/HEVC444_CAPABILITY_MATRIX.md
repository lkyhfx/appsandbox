# HEVC444 capability matrix

The probes keep the three capability layers separate. A `0` in a D3D12
profile query means that the selected D3D12 path did not expose that profile;
it does not prove that the physical NVIDIA hardware cannot encode YUV444.

## 2026-09-20: production D3D12 decoder integration

Baseline: `de7f0bd83a7a9f956023d4cef825f8278f4545df`.
Fresh RTX 4070 measurements:

| Stage | State | HRESULT / validation | Evidence |
| --- | --- | --- | --- |
| MF hardware HEVC decoder | UNAVAILABLE | count=0 | native enumeration |
| D3D12 Main444/AYUV decode capability | PASS | supported=1 | 3840x2160 at 60/1 |
| Actual DecodeFrame | PASS | decode status=0 | AYUV checksum `8d9d27c278740383` |
| D3D12 AYUV to BGRA | PASS | GPU Video Process | BGRA checksum `a3e0e636ed338383` |
| Production D3D12 / D3D11 interop | PASS | frame HRESULT=0 | production API harness: probe, 3 frames, real D3D11 Blt |
| Invalid selector / capability-only create | REJECTED | no decoder returned | device and LUID checks; re-probe/create PASS |
| Host encode Main444 profile | UNSUPPORTED | S_OK, IsSupported=0 | actual feature query |
| Host encode AYUV input | UNSUPPORTED | S_OK, IsSupported=0 | independent input query |
| Host encode resource requirements | PASS | S_OK, IsSupported=1 | does not imply encode support |
| Host encode configuration / combined 4K60 | BLOCKED_BY_PROFILE | validation NOT_RUN | 4k60=0; actual encode not run |
| Host encode min/max level | UNKNOWN | profile failed | seeded output storage is not evidence |
| Guest Main/NV12 4K60 | PASS | validation=0 | fresh SSH query; encoder and heap created |
| Guest Main444 profile | UNSUPPORTED | IsSupported=0 | fresh SSH query |
| Guest dependent Main444 stages | BLOCKED_BY_PROFILE | NOT_RUN | no actual encode workload |
| Native NVENC | BLOCKED | SDK unavailable | no hardware conclusion |
| Real Mutter HEVC444 / sustained 4K60 | BLOCKED_BY_DEPENDENCY | NOT_RUN | Guest encoding unavailable |

### Production path

`vm_video_decode_probe_builtin_hevc444` tries actual-tested MF first, then
D3D12 on the same D3D11 device's adapter. It decodes the bundled sample before
HostHello, breaking the negotiation dependency. Results are cached for the
display device lifetime and invalidated on D3D cleanup. Production creation
checks the tested backend, device identity, adapter LUID and decode evidence;
initialization failure causes IDD to re-probe before trying another create.

D3D12 creates a decoder/heap, uploads compressed Annex-B data, supplies DXVA
Range Extensions picture/slice arguments, submits DecodeFrame, resolves
decode statistics and waits for its fence. Reference-only allocations are
used when required. A video-process queue converts AYUV to BGRA; only after
its fence completes does D3D11 open the BGRA shared handle. Direct AYUV
sharing returned E_INVALIDARG on this driver.

The probe reads back the known BGRA checksum and executes D3D11
VideoProcessorBlt on the actual shared surface. The standalone diagnostic
also verifies the AYUV checksum. Production frame decoding reads only
statistics, never uncompressed image data. `decoded_format=AYUV` records
internal decode output; `presentation_format=BGRA` records the returned
D3D11 texture. A fresh surface per frame prevents the receive thread from
overwriting a frame still retained by the window renderer.

The initial backend deliberately supports the Guest's canonical single-slice,
all-IDR Main444 8-bit 3840x2160@60 stream. It parses, rebuilds and byte-compares
sequence headers, rejecting unsupported syntax rather than using incorrect
DXVA parameters. Inter pictures, multiple VCL slices, malformed headers,
wrong PPS and in-band parameter changes fail closed. ASVC recreates the
decoder. Guest HEVC444 now requests IDR with POC 0; HEVC420 is unchanged.
This is not a general HEVC decoder or a sustained-throughput claim.

Runtime backend value 4 identifies HEVC444/D3D12. READY requires a successful
window Present. Decode/conversion/Blt/Present failure preserves the existing
reconnect to RAW degraded path. The standalone harness does not claim real
Mutter or window-session validation.

### Validation

- Release x64 Core and AppSandbox: full compile/link PASS in
  `build/issue3/app/`; not a signed installer build.
- Standalone Host decode/encode probes: build PASS; runtime results above.
- CPU backend/config tests: 2 executable harnesses PASS, including failed and
  unchanged decode evidence, capability-only selection, canonical headers,
  malformed PPS and multiple-picture rejection.
- CPU encode validator: 1 executable harness PASS, including IDR versus CRA,
  payload without parameter sets, malformed/duplicate VCL and combined 60 FPS
  gating. Existing HEVC suite 10/10; display suite 14/14. Source contracts in
  those older suites are not end-to-end runtime evidence.
- Production GPU harness: probe and 3 frames PASS; invalid device/LUID and
  capability-only selectors rejected; re-probe/create PASS.
- Guest's existing source with the IDR/POC edit compiled with -Werror and
  standard capability passed in an isolated temporary directory. Exact local
  source/header transfer was rejected by automatic approval review as a
  private-source export; that build is **not** exact-worktree verification.

From an MSVC x64 developer shell:

```powershell
./tools/win/hevc444-probe/build-production-probe.ps1 -Run
python tools/win/hevc444-probe/test_backend_policy.py
python tools/win/hevc444-encode-probe/test_encode_validation.py
```

Raw logs: `build/issue3/{production-backend,host-decode,host-encode,app-build,
core-build,guest-capability,guest-probe-rebuild}.log`.

`production-4k60: false` remains unchanged. No 3600-frame acceptance run.
**CONTROL PLANE IMPLEMENTED / HIGH PERFORMANCE RUNTIME BLOCKED**.
