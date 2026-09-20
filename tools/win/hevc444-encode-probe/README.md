# Windows native D3D12 HEVC444 probe

This probe reports the Host-native D3D12 codec, Main444 profile, AYUV input,
codec configuration, resource requirement, and 3840x2160@60 capability fields.
The public Windows SDK does not name the runtime's Main444 enum, so the probe
uses the runtime value `5` and reports it explicitly.

`host_d3d12_hevc444_actual_encode` is kept separate from capability queries.
When all capability gates pass, the probe creates the encoder and heap, uploads
an AYUV 3840x2160 frame, submits `EncodeFrame`, resolves output metadata, and
checks that the generated bitstream is non-empty. The production gate remains
`production-4k60: false` until the generated VPS/SPS/PPS/IRAP stream is also
validated end-to-end.
