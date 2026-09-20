# Optional Windows NVENC HEVC444 probe

The repository does not vendor the NVIDIA Video Codec SDK. The probe therefore
never infers support from `nvEncodeAPI64.dll` being loadable and reports an
explicit `BLOCKED` reason until an SDK-enabled build performs both the
YUV444/4K60 capability query and one actual encode.

This result is independent from Host native D3D12 and Guest GPU-PV D3D12
results; a blocked optional NVENC probe must not be treated as hardware
unsupported.
