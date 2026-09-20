/* SPDX-License-Identifier: MIT */
#ifndef ASB_VIDEO_DECODE_POLICY_H
#define ASB_VIDEO_DECODE_POLICY_H
typedef enum VmVideoDecodeBackend {
    VM_VIDEO_DECODE_BACKEND_NONE = 0,
    VM_VIDEO_DECODE_BACKEND_MF_D3D11,
    VM_VIDEO_DECODE_BACKEND_D3D12
} VmVideoDecodeBackend;
/* Shared by production and executable tests. Capability queries are never
 * enough: the entire decode/presentation chain must have executed. */
static int vm_video_decode_evidence_ready(int actual, int written, int gpu,
                                         int rgb, int cpu_copy)
{
    return actual && written && gpu && rgb && !cpu_copy;
}
static VmVideoDecodeBackend vm_video_decode_select_backend(int mf_actual,
                                                           int d3d12_actual)
{
    return mf_actual ? VM_VIDEO_DECODE_BACKEND_MF_D3D11 :
        d3d12_actual ? VM_VIDEO_DECODE_BACKEND_D3D12 : VM_VIDEO_DECODE_BACKEND_NONE;
}
#endif
