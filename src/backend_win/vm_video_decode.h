/* SPDX-License-Identifier: MIT */
#ifndef ASB_VM_VIDEO_DECODE_H
#define ASB_VM_VIDEO_DECODE_H

#include <windows.h>
#include <d3d11.h>

typedef struct VmVideoDecoder VmVideoDecoder;

typedef enum VmVideoDecodeProfile {
    VM_VIDEO_HEVC420 = 0,
    VM_VIDEO_HEVC444 = 1
} VmVideoDecodeProfile;

typedef struct VmVideoDecodeCapability {
    BOOL available;
    UINT decoder_index;
    GUID decoder_clsid;
    WCHAR decoder_name[128];
    BOOL decoder_identity_valid;
    BOOL actual_decode;
    BOOL gpu_surface;
    DXGI_FORMAT decoded_format;
    BOOL ayuv_video_processor;
} VmVideoDecodeCapability;

BOOL vm_video_decode_supported(ID3D11Device *device);
BOOL vm_video_decode_supported_profile(ID3D11Device *device,
                                       VmVideoDecodeProfile profile);
/* Probe every hardware MFT with a real HEVC access unit. A missing access unit
 * intentionally returns unavailable: registration/activation alone is not
 * production evidence and must never enable HEVC444 HostHello negotiation. */
BOOL vm_video_decode_probe_profile(ID3D11Device *device,
                                   UINT width, UINT height,
                                   UINT fps_num, UINT fps_den,
                                   const BYTE *extradata, UINT extradata_size,
                                   const BYTE *access_unit, UINT access_unit_size,
                                   VmVideoDecodeProfile profile,
                                   VmVideoDecodeCapability *out);
BOOL vm_video_decode_probe_builtin_hevc444(ID3D11Device *device,
                                           VmVideoDecodeCapability *out);
VmVideoDecoder *vm_video_decoder_create(ID3D11Device *device,
                                        UINT width, UINT height,
                                        UINT fps_num, UINT fps_den,
                                        const BYTE *extradata,
                                        UINT extradata_size,
                                        VmVideoDecodeProfile profile);
VmVideoDecoder *vm_video_decoder_create_with_capability(
    ID3D11Device *device, UINT width, UINT height,
    UINT fps_num, UINT fps_den, const BYTE *extradata, UINT extradata_size,
    VmVideoDecodeProfile profile, const VmVideoDecodeCapability *capability);
/* S_OK returns a GPU NV12/AYUV texture with one caller-owned reference. S_FALSE
 * means the decoder accepted input but needs more data before output. */
HRESULT vm_video_decoder_decode(VmVideoDecoder *decoder,
                                const BYTE *data, UINT size,
                                LONGLONG capture_time_100ns,
                                ID3D11Texture2D **texture,
                                UINT *subresource);
void vm_video_decoder_destroy(VmVideoDecoder *decoder);

#endif
