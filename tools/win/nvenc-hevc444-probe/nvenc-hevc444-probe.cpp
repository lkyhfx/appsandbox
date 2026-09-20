/* SPDX-License-Identifier: MIT */
/* Optional Windows NVENC probe. It is intentionally not part of the normal
   solution because the NVIDIA Video Codec SDK is not redistributable. */
#include <windows.h>
#include <cstdio>

int wmain()
{
    HMODULE nvenc = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!nvenc) {
        std::puts("nvenc_hevc444_profile=BLOCKED");
        std::puts("nvenc_yuv444=BLOCKED");
        std::puts("nvenc_4k60=BLOCKED");
        std::puts("nvenc_actual_encode=BLOCKED");
        std::puts("nvenc_blocked_reason=NVENC_VIDEO_CODEC_SDK_RUNTIME_NOT_AVAILABLE");
        return 2;
    }
    /* Loading the runtime alone is not capability evidence. The optional SDK
       build supplies the NV_ENCODE_API_FUNCTION_LIST and must perform the
       GUID/configuration query plus one real YUV444 encode before changing
       these fields to PASS. */
    FreeLibrary(nvenc);
    std::puts("nvenc_hevc444_profile=BLOCKED");
    std::puts("nvenc_yuv444=BLOCKED");
    std::puts("nvenc_4k60=BLOCKED");
    std::puts("nvenc_actual_encode=BLOCKED");
    std::puts("nvenc_blocked_reason=NVENC_SDK_OPTIONAL_PROBE_NOT_BUILT");
    return 2;
}
