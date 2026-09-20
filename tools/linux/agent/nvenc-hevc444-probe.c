/* SPDX-License-Identifier: MIT
 *
 * Gate A: synthetic, GPU-resident NVIDIA NVENC HEVC Main 4:4:4 probe.
 *
 * This is deliberately independent of Mutter and D3D12.  CUDA device memory
 * is registered directly with NVENC, so a PASS here says only that the native
 * Guest encoder path works.  It does not establish D3D12/CUDA interop.
 *
 * Build with the NVIDIA Video Codec SDK (or nv-codec-headers) include path:
 *   cc -O2 -Wall -Wextra -D_GNU_SOURCE -I.../nv-codec-headers/include \
 *      nvenc-hevc444-probe.c -ldl -o nvenc-hevc444-probe
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvEncodeAPI.h"

typedef int CUresult;
typedef void *CUcontext;
typedef int CUdevice;
typedef uint64_t CUdeviceptr;

typedef CUresult (*cu_init_fn)(unsigned);
typedef CUresult (*cu_driver_version_fn)(int *);
typedef CUresult (*cu_device_get_fn)(CUdevice *, int);
typedef CUresult (*cu_device_name_fn)(char *, int, CUdevice);
typedef CUresult (*cu_ctx_create_fn)(CUcontext *, unsigned, CUdevice);
typedef CUresult (*cu_ctx_destroy_fn)(CUcontext);
typedef CUresult (*cu_mem_alloc_fn)(CUdeviceptr *, size_t);
typedef CUresult (*cu_mem_free_fn)(CUdeviceptr);
typedef CUresult (*cu_memset_d8_fn)(CUdeviceptr, unsigned char, size_t);
typedef CUresult (*cu_ctx_sync_fn)(void);
typedef NVENCSTATUS (*nv_create_instance_fn)(NV_ENCODE_API_FUNCTION_LIST *);
typedef NVENCSTATUS (*nv_get_max_version_fn)(uint32_t *);

static double monotonic_seconds(void)
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

/* Keep the capability query in one place. The result is returned through the
 * explicit capsVal output used by current and recent Video Codec SDK headers. */
static int query_cap_value(NV_ENCODE_API_FUNCTION_LIST *api, void *encoder,
                           NV_ENC_CAPS cap)
{
    NV_ENC_CAPS_PARAM params = {0};
    params.version = NV_ENC_CAPS_PARAM_VER;
    params.capsToQuery = cap;
    int value = 0;
    return api->nvEncGetEncodeCaps(encoder, NV_ENC_CODEC_HEVC_GUID, &params,
                                   &value) == NV_ENC_SUCCESS ? value : 0;
}

int main(int argc, char **argv)
{
    const int frames = argc > 1 ? atoi(argv[1]) : 600;
    const char *stream_path = argc > 2 ? argv[2] : "/tmp/appsandbox-nvenc-hevc444.hevc";
    void *cuda = NULL;
    void *nv = NULL;
    CUcontext context = NULL;
    CUdeviceptr pixels = 0;
    void *encoder = NULL;
    FILE *stream = NULL;
    NV_ENC_REGISTER_RESOURCE registered = {0};
    NV_ENC_MAP_INPUT_RESOURCE mapped = {0};
    NV_ENC_CREATE_BITSTREAM_BUFFER output = {0};
    int resource_registered = 0;
    int resource_mapped = 0;
    int bitstream_created = 0;
    int result = 3;
    int encoded_frames = 0;
    int encode_failures = 0;
    double total_seconds = 0.0;
    double max_ms = 0.0;
    int max_width = 0;
    int max_height = 0;
    int hevc = 0;
    int yuv444 = 0;
    int hevc444 = 0;
    int four_k = 0;
    int encode_error = 0;
    const char *encode_error_stage = "none";
    char blocked_reason[128] = "unknown";
    NV_ENCODE_API_FUNCTION_LIST api = {0};
    cu_init_fn cu_init = NULL;
    cu_driver_version_fn cu_driver_get_version = NULL;
    cu_device_get_fn cu_device_get = NULL;
    cu_device_name_fn cu_device_get_name = NULL;
    cu_ctx_create_fn cu_ctx_create = NULL;
    cu_ctx_destroy_fn cu_ctx_destroy = NULL;
    cu_mem_alloc_fn cu_mem_alloc = NULL;
    cu_mem_free_fn cu_mem_free = NULL;
    cu_memset_d8_fn cu_memset_d8 = NULL;
    cu_ctx_sync_fn cu_ctx_sync = NULL;
    nv_create_instance_fn nv_create_instance = NULL;
    nv_get_max_version_fn nv_get_max_version = NULL;

    if (frames < 1 || frames > 3600) {
        puts("guest_nvenc_blocked_reason=frames-must-be-1..3600");
        return 2;
    }

    cuda = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    nv = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!cuda || !nv) {
        snprintf(blocked_reason, sizeof(blocked_reason), "runtime-not-found");
        goto finish;
    }

#define LOAD_INTO(handle, symbol, target)                                      \
    do {                                                                       \
        (target) = (typeof(target))dlsym((handle), (symbol));                  \
        if (!(target)) {                                                       \
            snprintf(blocked_reason, sizeof(blocked_reason), "missing-%s",   \
                     (symbol));                                                \
            goto finish;                                                       \
        }                                                                      \
    } while (0)
    LOAD_INTO(cuda, "cuInit", cu_init);
    LOAD_INTO(cuda, "cuDriverGetVersion", cu_driver_get_version);
    LOAD_INTO(cuda, "cuDeviceGet", cu_device_get);
    LOAD_INTO(cuda, "cuDeviceGetName", cu_device_get_name);
    LOAD_INTO(cuda, "cuCtxCreate_v2", cu_ctx_create);
    LOAD_INTO(cuda, "cuCtxDestroy_v2", cu_ctx_destroy);
    LOAD_INTO(cuda, "cuMemAlloc_v2", cu_mem_alloc);
    LOAD_INTO(cuda, "cuMemFree_v2", cu_mem_free);
    LOAD_INTO(cuda, "cuMemsetD8_v2", cu_memset_d8);
    LOAD_INTO(cuda, "cuCtxSynchronize", cu_ctx_sync);
    LOAD_INTO(nv, "NvEncodeAPICreateInstance", nv_create_instance);
    LOAD_INTO(nv, "NvEncodeAPIGetMaxSupportedVersion", nv_get_max_version);
#undef LOAD_INTO

    if (cu_init(0) != 0) {
        snprintf(blocked_reason, sizeof(blocked_reason), "cuda-init");
        goto finish;
    }
    CUdevice device = 0;
    char gpu_name[128] = {0};
    int driver_version = 0;
    uint32_t max_api_version = 0;
    if (cu_driver_get_version(&driver_version) != 0 ||
        cu_device_get(&device, 0) != 0 ||
        cu_device_get_name(gpu_name, (int)sizeof(gpu_name), device) != 0 ||
        nv_get_max_version(&max_api_version) != NV_ENC_SUCCESS ||
        cu_ctx_create(&context, 0, device) != 0) {
        snprintf(blocked_reason, sizeof(blocked_reason), "device-init");
        goto finish;
    }
    printf("guest_cuda_driver=%d\n", driver_version);
    printf("guest_gpu=%s\n", gpu_name[0] ? gpu_name : "unknown");
    printf("guest_nvenc_api_max=0x%x\n", max_api_version);

    api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (nv_create_instance(&api) != NV_ENC_SUCCESS ||
        api.nvEncOpenEncodeSessionEx == NULL) {
        snprintf(blocked_reason, sizeof(blocked_reason), "nvenc-api-init");
        goto finish;
    }
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open = {0};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    open.device = context;
    open.apiVersion = NVENCAPI_VERSION;
    if (api.nvEncOpenEncodeSessionEx(&open, &encoder) != NV_ENC_SUCCESS) {
        snprintf(blocked_reason, sizeof(blocked_reason), "nvenc-session");
        goto finish;
    }

    NV_ENC_PRESET_CONFIG preset = {0};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    hevc = api.nvEncGetEncodePresetConfigEx(
               encoder, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P1_GUID,
               NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset) == NV_ENC_SUCCESS;
    yuv444 = query_cap_value(&api, encoder, NV_ENC_CAPS_SUPPORT_YUV444_ENCODE) > 0;
    max_width = query_cap_value(&api, encoder, NV_ENC_CAPS_WIDTH_MAX);
    max_height = query_cap_value(&api, encoder, NV_ENC_CAPS_HEIGHT_MAX);
    hevc444 = hevc && yuv444;
    four_k = max_width >= 3840 && max_height >= 2160;
    if (!hevc || !hevc444 || !yuv444 || !four_k) {
        snprintf(blocked_reason, sizeof(blocked_reason), "capability-unsupported");
        goto finish;
    }

    {
        NV_ENC_CONFIG config = preset.presetCfg;
        config.profileGUID = NV_ENC_HEVC_PROFILE_FREXT_GUID;
        config.encodeCodecConfig.hevcConfig.chromaFormatIDC = 3;
        config.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
        config.gopLength = 60;
        config.frameIntervalP = 1;
        config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        config.rcParams.averageBitRate = 120000000;
        config.rcParams.maxBitRate = 120000000;
        config.rcParams.vbvBufferSize = 2000000;
        config.rcParams.vbvInitialDelay = 2000000;
        config.rcParams.enableLookahead = 0;
        config.rcParams.lookaheadDepth = 0;

        NV_ENC_INITIALIZE_PARAMS init = {0};
        init.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
        init.presetGUID = NV_ENC_PRESET_P1_GUID;
        init.encodeWidth = init.darWidth = 3840;
        init.encodeHeight = init.darHeight = 2160;
        init.frameRateNum = 60;
        init.frameRateDen = 1;
        init.enablePTD = 1;
        init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        init.encodeConfig = &config;
        if (api.nvEncInitializeEncoder(encoder, &init) != NV_ENC_SUCCESS) {
            snprintf(blocked_reason, sizeof(blocked_reason), "hevc444-init");
            goto finish;
        }

        const size_t pitch = 3840;
        const size_t bytes = pitch * 2160u * 3u;
        if (cu_mem_alloc(&pixels, bytes) != 0 ||
            cu_memset_d8(pixels, 128, bytes) != 0 ||
            cu_ctx_sync() != 0) {
            snprintf(blocked_reason, sizeof(blocked_reason), "cuda-yuv444-buffer");
            goto finish;
        }
        registered.version = NV_ENC_REGISTER_RESOURCE_VER;
        registered.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
        registered.width = 3840;
        registered.height = 2160;
        registered.pitch = (uint32_t)pitch;
        registered.resourceToRegister = (void *)(uintptr_t)pixels;
        registered.bufferFormat = NV_ENC_BUFFER_FORMAT_YUV444;
        registered.bufferUsage = NV_ENC_INPUT_IMAGE;
        if (api.nvEncRegisterResource(encoder, &registered) != NV_ENC_SUCCESS) {
            snprintf(blocked_reason, sizeof(blocked_reason), "yuv444-register");
            goto finish;
        }
        resource_registered = 1;
        mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mapped.registeredResource = registered.registeredResource;
        if (api.nvEncMapInputResource(encoder, &mapped) != NV_ENC_SUCCESS) {
            snprintf(blocked_reason, sizeof(blocked_reason), "yuv444-map");
            goto finish;
        }
        resource_mapped = 1;
        output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (api.nvEncCreateBitstreamBuffer(encoder, &output) != NV_ENC_SUCCESS) {
            snprintf(blocked_reason, sizeof(blocked_reason), "bitstream-buffer");
            goto finish;
        }
        bitstream_created = 1;
        stream = fopen(stream_path, "wb");
        if (!stream) {
            snprintf(blocked_reason, sizeof(blocked_reason), "stream-open");
            goto finish;
        }

        const double start = monotonic_seconds();
        for (int frame = 0; frame < frames; ++frame) {
            const double frame_start = monotonic_seconds();
            NV_ENC_PIC_PARAMS pic = {0};
            pic.version = NV_ENC_PIC_PARAMS_VER;
            pic.inputBuffer = mapped.mappedResource;
            pic.bufferFmt = mapped.mappedBufferFmt;
            pic.inputWidth = 3840;
            pic.inputHeight = 2160;
            pic.inputPitch = (uint32_t)pitch;
            pic.outputBitstream = output.bitstreamBuffer;
            pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
            pic.inputTimeStamp = (uint64_t)frame;
            if (frame == 0) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR |
                                                  NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
            const CUresult memset_result = cu_memset_d8(
                pixels, (unsigned char)(32 + frame % 192), bytes);
            const CUresult sync_result = memset_result == 0 ? cu_ctx_sync() : memset_result;
            const NVENCSTATUS encode_result = sync_result == 0
                ? api.nvEncEncodePicture(encoder, &pic) : NV_ENC_ERR_GENERIC;
            if (memset_result != 0 || sync_result != 0 ||
                encode_result != NV_ENC_SUCCESS) {
                encode_error = encode_result != NV_ENC_SUCCESS
                    ? (int)encode_result : (int)(memset_result != 0 ? memset_result : sync_result);
                encode_error_stage = memset_result != 0 ? "cuda-memset" :
                    sync_result != 0 ? "cuda-sync" : "nvenc-encode";
                ++encode_failures;
                break;
            }
            NV_ENC_LOCK_BITSTREAM lock = {0};
            lock.version = NV_ENC_LOCK_BITSTREAM_VER;
            lock.outputBitstream = output.bitstreamBuffer;
            const NVENCSTATUS lock_result = api.nvEncLockBitstream(encoder, &lock);
            if (lock_result != NV_ENC_SUCCESS) {
                encode_error = (int)lock_result;
                encode_error_stage = "nvenc-lock";
                ++encode_failures;
                break;
            }
            if (fwrite(lock.bitstreamBufferPtr, 1, lock.bitstreamSizeInBytes,
                       stream) != lock.bitstreamSizeInBytes) {
                api.nvEncUnlockBitstream(encoder, output.bitstreamBuffer);
                encode_error = -1;
                encode_error_stage = "stream-write";
                ++encode_failures;
                break;
            }
            total_seconds += monotonic_seconds() - frame_start;
            {
                const double elapsed_ms =
                    (monotonic_seconds() - frame_start) * 1000.0;
                if (elapsed_ms > max_ms) max_ms = elapsed_ms;
            }
            ++encoded_frames;
            api.nvEncUnlockBitstream(encoder, output.bitstreamBuffer);
        }
        (void)start;
    }
    if (stream && fclose(stream) != 0) stream = NULL;
    stream = NULL;
    if (encode_failures == 0 && encoded_frames == frames) {
        result = 0;
    } else {
        snprintf(blocked_reason, sizeof(blocked_reason), "actual-encode-failed");
    }

finish:
    if (stream) fclose(stream);
    if (bitstream_created && encoder)
        api.nvEncDestroyBitstreamBuffer(encoder, output.bitstreamBuffer);
    if (resource_mapped && encoder)
        api.nvEncUnmapInputResource(encoder, mapped.mappedResource);
    if (resource_registered && encoder)
        api.nvEncUnregisterResource(encoder, registered.registeredResource);
    if (encoder && api.nvEncDestroyEncoder) api.nvEncDestroyEncoder(encoder);
    if (pixels && cu_mem_free) cu_mem_free(pixels);
    if (context && cu_ctx_destroy) cu_ctx_destroy(context);
    if (nv) dlclose(nv);
    if (cuda) dlclose(cuda);

    printf("guest_nvenc_hevc=%d\n", hevc);
    printf("guest_nvenc_hevc444=%d\n", hevc444);
    printf("guest_nvenc_yuv444=%d\n", yuv444);
    printf("guest_nvenc_max_width=%d\n", max_width);
    printf("guest_nvenc_max_height=%d\n", max_height);
    printf("guest_nvenc_4k=%d\n", four_k);
    printf("guest_nvenc_actual_encode=%s\n", result == 0 ? "PASS" : "BLOCKED");
    if (result != 0) printf("guest_nvenc_blocked_reason=%s\n", blocked_reason);
    printf("guest_nvenc_encoded_frames=%d\n", encoded_frames);
    printf("guest_nvenc_encode_failures=%d\n", encode_failures);
    printf("guest_nvenc_encode_error=%d\n", encode_error);
    printf("guest_nvenc_encode_error_stage=%s\n", encode_error_stage);
    printf("guest_nvenc_fps=%.3f\n",
           total_seconds > 0.0 ? encoded_frames / total_seconds : 0.0);
    printf("guest_nvenc_mean_ms=%.3f\n",
           encoded_frames ? total_seconds * 1000.0 / encoded_frames : 0.0);
    printf("guest_nvenc_max_ms=%.3f\n", max_ms);
    return result;
}
