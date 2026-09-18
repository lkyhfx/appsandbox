/* SPDX-License-Identifier: MIT */
#ifndef ASB_VM_VIDEO_DECODE_H
#define ASB_VM_VIDEO_DECODE_H

#include <windows.h>
#include <d3d11.h>

typedef struct VmVideoDecoder VmVideoDecoder;

BOOL vm_video_decode_supported(ID3D11Device *device);
VmVideoDecoder *vm_video_decoder_create(ID3D11Device *device,
                                        UINT width, UINT height,
                                        UINT fps_num, UINT fps_den,
                                        const BYTE *extradata,
                                        UINT extradata_size);
/* S_OK returns a GPU NV12 texture with one caller-owned reference. S_FALSE
 * means the decoder accepted input but needs more data before output. */
HRESULT vm_video_decoder_decode(VmVideoDecoder *decoder,
                                const BYTE *data, UINT size,
                                LONGLONG capture_time_100ns,
                                ID3D11Texture2D **texture,
                                UINT *subresource);
void vm_video_decoder_destroy(VmVideoDecoder *decoder);

#endif
