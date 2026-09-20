/* SPDX-License-Identifier: MIT */
#pragma once
#include "vm_video_decode.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct VmD3D12Decoder VmD3D12Decoder;
BOOL vm_d3d12_probe(ID3D11Device *device, VmVideoDecodeCapability *out);
BOOL vm_video_decode_probe_ayuv_surface(ID3D11Device *device, ID3D11Texture2D *texture);
VmD3D12Decoder *vm_d3d12_create(ID3D11Device *device, UINT width, UINT height,
    UINT fps_num, UINT fps_den, const BYTE *headers, UINT size);
HRESULT vm_d3d12_decode(VmD3D12Decoder *decoder, const BYTE *data, UINT size,
    ID3D11Texture2D **texture);
BOOL vm_d3d12_render_begin(VmD3D12Decoder *decoder,
                           ID3D11Texture2D *texture);
BOOL vm_d3d12_render_submitted(VmD3D12Decoder *decoder,
                               ID3D11Texture2D *texture);
void vm_d3d12_render_cancel(VmD3D12Decoder *decoder,
                            ID3D11Texture2D *texture);
void vm_d3d12_drop_frame(VmD3D12Decoder *decoder,
                         ID3D11Texture2D *texture);
UINT64 vm_d3d12_slot_reuse_hazards(VmD3D12Decoder *decoder);
void vm_d3d12_destroy(VmD3D12Decoder *decoder);
#ifdef __cplusplus
}
#endif
