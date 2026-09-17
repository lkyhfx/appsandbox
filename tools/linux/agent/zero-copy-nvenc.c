/* Standalone GPU-resident synthetic NVENC probe, not desktop capture.
 * Build with nv-codec-headers n13.0.19.0: gcc -O2 -I. ... -ldl -o nvenc-probe
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>
#include "nvEncodeAPI.h"

#define CHECK(call) do { int r = (call); if (r) { \
    fprintf(stderr, "%s failed: %d\n", #call, r); return 1; } } while (0)
#define LOAD(lib, name, ret, args) ret (*name) args = dlsym(lib, #name); \
    if (!name) { fprintf(stderr, "missing %s\n", #name); return 1; }
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 600;
    if (frames < 1 || frames > 3600) return 2;
    void *cuda = dlopen("libcuda.so.1", RTLD_NOW);
    void *nv = dlopen("libnvidia-encode.so.1", RTLD_NOW);
    if (!cuda || !nv) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    LOAD(cuda, cuInit, int, (unsigned));
    LOAD(cuda, cuDriverGetVersion, int, (int *));
    LOAD(cuda, cuDeviceGet, int, (int *, int));
    LOAD(cuda, cuDeviceGetName, int, (char *, int, int));
    LOAD(cuda, cuCtxCreate_v2, int, (void **, unsigned, int));
    LOAD(cuda, cuCtxDestroy_v2, int, (void *));
    LOAD(cuda, cuMemAlloc_v2, int, (uint64_t *, size_t));
    LOAD(cuda, cuMemFree_v2, int, (uint64_t));
    LOAD(cuda, cuMemsetD8_v2, int, (uint64_t, unsigned char, size_t));
    LOAD(cuda, cuCtxSynchronize, int, (void));
    LOAD(nv, NvEncodeAPICreateInstance, NVENCSTATUS, (NV_ENCODE_API_FUNCTION_LIST *));
    LOAD(nv, NvEncodeAPIGetMaxSupportedVersion, NVENCSTATUS, (uint32_t *));
    CHECK(cuInit(0));
    int dev, version; void *ctx = NULL; char name[128]; uint32_t maxver;
    CHECK(cuDriverGetVersion(&version)); CHECK(cuDeviceGet(&dev, 0));
    CHECK(cuDeviceGetName(name, sizeof(name), dev));
    CHECK(NvEncodeAPIGetMaxSupportedVersion(&maxver));
    printf("CUDA=%d GPU=%s NVENC_MAX=0x%x\n", version, name, maxver);
    CHECK(cuCtxCreate_v2(&ctx, 0, dev));
    NV_ENCODE_API_FUNCTION_LIST api = {0}; api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    CHECK(NvEncodeAPICreateInstance(&api));
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open = {0};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_CUDA; open.device = ctx;
    open.apiVersion = NVENCAPI_VERSION;
    void *enc = NULL; CHECK(api.nvEncOpenEncodeSessionEx(&open, &enc));
    NV_ENC_PRESET_CONFIG preset = {0}; preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    CHECK(api.nvEncGetEncodePresetConfigEx(enc, NV_ENC_CODEC_HEVC_GUID,
          NV_ENC_PRESET_P1_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset));
    NV_ENC_CONFIG config = preset.presetCfg;
    config.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
    config.gopLength = 60; config.frameIntervalP = 1;
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config.rcParams.averageBitRate = 60000000;
    config.rcParams.maxBitRate = 60000000;
    config.rcParams.vbvBufferSize = 1000000;
    config.rcParams.vbvInitialDelay = 1000000;
    config.rcParams.enableLookahead = 0; config.rcParams.lookaheadDepth = 0;
    NV_ENC_INITIALIZE_PARAMS init = {0}; init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_HEVC_GUID; init.presetGUID = NV_ENC_PRESET_P1_GUID;
    init.encodeWidth = init.darWidth = 3840; init.encodeHeight = init.darHeight = 2160;
    init.frameRateNum = 60; init.frameRateDen = 1; init.enablePTD = 1;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY; init.encodeConfig = &config;
    CHECK(api.nvEncInitializeEncoder(enc, &init));
    const unsigned pitch = 3840, height = 2160;
    uint64_t pixels; CHECK(cuMemAlloc_v2(&pixels, (size_t)pitch * height * 3 / 2));
    CHECK(cuMemsetD8_v2(pixels, 128, (size_t)pitch * height * 3 / 2));
    CHECK(cuCtxSynchronize());
    NV_ENC_REGISTER_RESOURCE reg = {0}; reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    reg.width = 3840; reg.height = height; reg.pitch = pitch;
    reg.resourceToRegister = (void *)(uintptr_t)pixels;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12; reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    CHECK(api.nvEncRegisterResource(enc, &reg));
    NV_ENC_MAP_INPUT_RESOURCE map = {0}; map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = reg.registeredResource;
    CHECK(api.nvEncMapInputResource(enc, &map));
    NV_ENC_CREATE_BITSTREAM_BUFFER out = {0}; out.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    CHECK(api.nvEncCreateBitstreamBuffer(enc, &out));
    size_t total = 0; double start = now(), worst = 0;
    FILE *sample = fopen("synthetic.hevc", "wb");
    if (!sample) { perror("synthetic.hevc"); return 1; }
    for (int i = 0; i < frames; i++) {
        double a = now();
        CHECK(cuMemsetD8_v2(pixels, (unsigned char)(32 + i % 192), (size_t)pitch * height));
        CHECK(cuCtxSynchronize());
        NV_ENC_PIC_PARAMS pic = {0}; pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputBuffer = map.mappedResource; pic.bufferFmt = map.mappedBufferFmt;
        pic.inputWidth = 3840; pic.inputHeight = height; pic.inputPitch = pitch;
        pic.outputBitstream = out.bitstreamBuffer;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME; pic.inputTimeStamp = i;
        CHECK(api.nvEncEncodePicture(enc, &pic));
        NV_ENC_LOCK_BITSTREAM lock = {0}; lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = out.bitstreamBuffer;
        CHECK(api.nvEncLockBitstream(enc, &lock));
        total += lock.bitstreamSizeInBytes;
        if (i < 60 && fwrite(lock.bitstreamBufferPtr, 1, lock.bitstreamSizeInBytes, sample)
                != lock.bitstreamSizeInBytes) { perror("write sample"); return 1; }
        CHECK(api.nvEncUnlockBitstream(enc, out.bitstreamBuffer));
        double elapsed = now() - a; if (elapsed > worst) worst = elapsed;
    }
    double seconds = now() - start;
    if (fclose(sample)) return 1;
    printf("frames=%d seconds=%.3f fps=%.2f mean_ms=%.3f max_ms=%.3f bytes=%zu\n",
           frames, seconds, frames / seconds, seconds * 1000 / frames, worst * 1000, total);
    printf("GPU-generated flat frames only; no desktop import or fence interop tested.\n");
    CHECK(api.nvEncUnmapInputResource(enc, map.mappedResource));
    CHECK(api.nvEncUnregisterResource(enc, reg.registeredResource));
    CHECK(api.nvEncDestroyBitstreamBuffer(enc, out.bitstreamBuffer));
    CHECK(api.nvEncDestroyEncoder(enc)); CHECK(cuMemFree_v2(pixels));
    CHECK(cuCtxDestroy_v2(ctx));
    return 0;
}
