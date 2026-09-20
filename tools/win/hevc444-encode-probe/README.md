# Windows native D3D12 HEVC444 probe

This probe reports the Host-native D3D12 codec, Main444 profile, AYUV input,
HEVC1 codec configuration, resource requirement, and 3840x2160@60 capability
fields. The profile-level query supplies valid min/max HEVC level records and
the configuration query uses the HEVC1 support masks, including the extended
picture defaults required by Main444.
The public Windows SDK does not name the runtime's Main444 enum, so the probe
uses the runtime value `5` and reports it explicitly.

`host_d3d12_hevc444_actual_encode` is kept separate from capability queries.
When all capability gates pass, the probe creates the encoder and heap, uploads
an AYUV 3840x2160 frame, submits `EncodeFrame` with HEVC1 picture control,
resolves output metadata, and uses explicit copy-to-encode and encode-to-copy
fences. Success requires `EncodeErrorFlags == 0`, a bounded
`EncodedBitstreamWrittenBytesCount`, and generated VPS/SPS/PPS/IRAP NAL units;
it never scans uninitialized bytes for a non-zero value. The production gate
remains `production-4k60: false` until the complete runtime path is validated.
