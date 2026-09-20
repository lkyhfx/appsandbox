/* SPDX-License-Identifier: MIT
 *
 * B': last-resort D3D12 -> NVENC DirectX registration probe.
 *
 * This intentionally does not use CUDA external memory.  It creates one
 * GPU-resident AYUV D3D12 resource, opens an NVENC DIRECTX session with the
 * D3D12 device, and registers the ID3D12Resource* directly.  Any failed
 * boundary is reported as BLOCKED and no CPU staging path is introduced.
 */
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <directx/dxcore.h>
#include <dxguids/dxguids.h>
#include <wrl/client.h>
#include <nvEncodeAPI.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>

using Microsoft::WRL::ComPtr;

namespace {

using nv_create_instance_fn = NVENCSTATUS (*)(NV_ENCODE_API_FUNCTION_LIST *);

struct AdapterContext {
    ComPtr<IDXCoreAdapterFactory> factory;
    ComPtr<IDXCoreAdapterList> adapters;
    ComPtr<IDXCoreAdapter> adapter;
    ComPtr<ID3D12Device> device;
};

static void blocked(const char *stage, const char *reason)
{
    std::printf("%s=BLOCKED\n", stage);
    std::printf("%s_reason=%s\n", stage, reason);
}

static bool make_device(AdapterContext *out)
{
    const GUID attributes[] = {DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE};
    return SUCCEEDED(DXCoreCreateAdapterFactory(IID_PPV_ARGS(&out->factory))) &&
        SUCCEEDED(out->factory->CreateAdapterList(
            1, attributes, IID_PPV_ARGS(&out->adapters))) &&
        out->adapters->GetAdapterCount() != 0 &&
        SUCCEEDED(out->adapters->GetAdapter(0, IID_PPV_ARGS(&out->adapter))) &&
        SUCCEEDED(D3D12CreateDevice(out->adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(&out->device)));
}

static D3D12_RESOURCE_DESC texture_desc()
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 3840;
    desc.Height = 2160;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_AYUV;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

static bool decode_verify(const char *path, std::string *pix_fmt)
{
    char command[1024];
    std::snprintf(command, sizeof(command),
                  "ffprobe -v error -select_streams v:0 "
                  "-show_entries stream=width,height,pix_fmt "
                  "-of default=nw=1:nk=1 %s", path);
    FILE *pipe = popen(command, "r");
    if (!pipe) return false;
    char line[128];
    std::string width, height, format;
    if (std::fgets(line, sizeof(line), pipe)) width = line;
    if (std::fgets(line, sizeof(line), pipe)) height = line;
    if (std::fgets(line, sizeof(line), pipe)) format = line;
    const int status = pclose(pipe);
    while (!width.empty() && (width.back() == '\n' || width.back() == '\r'))
        width.pop_back();
    while (!height.empty() && (height.back() == '\n' || height.back() == '\r'))
        height.pop_back();
    while (!format.empty() && (format.back() == '\n' || format.back() == '\r'))
        format.pop_back();
    if (pix_fmt) *pix_fmt = format;
    return status == 0 && width == "3840" && height == "2160" &&
           format == "yuv444p";
}

} // namespace

int main()
{
    std::printf("cpu_framebuffer_copy=0\n");
    std::printf("cpu_upload=0\n");

    void *nv_lib = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!nv_lib) {
        blocked("nvenc_directx_session", "nvenc-runtime-not-found");
        blocked("nvenc_register_d3d12_resource", "not-run-after-session-block");
        blocked("nvenc_directx_map", "not-run-after-register-block");
        blocked("nvenc_directx_actual_encode", "not-run-after-register-block");
        return 3;
    }
    const auto nv_create = reinterpret_cast<nv_create_instance_fn>(
        dlsym(nv_lib, "NvEncodeAPICreateInstance"));
    if (!nv_create) {
        blocked("nvenc_directx_session", "nvenc-api-incomplete");
        blocked("nvenc_register_d3d12_resource", "not-run-after-session-block");
        blocked("nvenc_directx_map", "not-run-after-register-block");
        blocked("nvenc_directx_actual_encode", "not-run-after-register-block");
        dlclose(nv_lib);
        return 3;
    }

    AdapterContext adapter;
    if (!make_device(&adapter)) {
        blocked("nvenc_directx_session", "d3d12-device");
        blocked("nvenc_register_d3d12_resource", "not-run-after-session-block");
        blocked("nvenc_directx_map", "not-run-after-register-block");
        blocked("nvenc_directx_actual_encode", "not-run-after-register-block");
        dlclose(nv_lib);
        return 3;
    }
    const D3D12_RESOURCE_DESC desc = texture_desc();
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(adapter.device->CreateCommittedResource(
                   &heap, D3D12_HEAP_FLAG_NONE, &desc,
                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                   IID_PPV_ARGS(&resource)))) {
        blocked("nvenc_directx_session", "d3d12-resource-init");
        blocked("nvenc_register_d3d12_resource", "not-run-after-session-block");
        blocked("nvenc_directx_map", "not-run-after-register-block");
        blocked("nvenc_directx_actual_encode", "not-run-after-register-block");
        dlclose(nv_lib);
        return 3;
    }

    NV_ENCODE_API_FUNCTION_LIST api = {};
    api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (nv_create(&api) != NV_ENC_SUCCESS ||
        !api.nvEncOpenEncodeSessionEx) {
        blocked("nvenc_directx_session", "nvenc-api-init");
        blocked("nvenc_register_d3d12_resource", "not-run-after-session-block");
        blocked("nvenc_directx_map", "not-run-after-register-block");
        blocked("nvenc_directx_actual_encode", "not-run-after-register-block");
        dlclose(nv_lib);
        return 3;
    }
    void *encoder = nullptr;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open = {};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device = adapter.device.Get();
    open.apiVersion = NVENCAPI_VERSION;
    if (api.nvEncOpenEncodeSessionEx(&open, &encoder) != NV_ENC_SUCCESS) {
        blocked("nvenc_directx_session", "d3d12-device-not-accepted");
        blocked("nvenc_register_d3d12_resource", "not-run-after-session-block");
        blocked("nvenc_directx_map", "not-run-after-register-block");
        blocked("nvenc_directx_actual_encode", "not-run-after-register-block");
        dlclose(nv_lib);
        return 3;
    }
    std::printf("nvenc_directx_session=PASS\n");

    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    NV_ENC_CONFIG config = {};
    if (api.nvEncGetEncodePresetConfigEx(
            encoder, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P1_GUID,
            NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset) != NV_ENC_SUCCESS) {
        blocked("nvenc_register_d3d12_resource", "hevc-preset");
        api.nvEncDestroyEncoder(encoder);
        dlclose(nv_lib);
        return 3;
    }
    config = preset.presetCfg;
    config.profileGUID = NV_ENC_HEVC_PROFILE_FREXT_GUID;
    config.encodeCodecConfig.hevcConfig.chromaFormatIDC = 3;
    config.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
    config.gopLength = 1;
    config.frameIntervalP = 1;
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    NV_ENC_INITIALIZE_PARAMS initialize = {};
    initialize.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
    initialize.presetGUID = NV_ENC_PRESET_P1_GUID;
    initialize.encodeWidth = initialize.darWidth = 3840;
    initialize.encodeHeight = initialize.darHeight = 2160;
    initialize.frameRateNum = 60;
    initialize.frameRateDen = 1;
    initialize.enablePTD = 1;
    initialize.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    initialize.encodeConfig = &config;
    if (api.nvEncInitializeEncoder(encoder, &initialize) != NV_ENC_SUCCESS) {
        blocked("nvenc_register_d3d12_resource", "hevc444-init");
        api.nvEncDestroyEncoder(encoder);
        dlclose(nv_lib);
        return 3;
    }

    NV_ENC_REGISTER_RESOURCE registered = {};
    registered.version = NV_ENC_REGISTER_RESOURCE_VER;
    registered.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    registered.width = 3840;
    registered.height = 2160;
    registered.resourceToRegister = resource.Get();
    registered.bufferFormat = NV_ENC_BUFFER_FORMAT_AYUV;
    registered.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (api.nvEncRegisterResource(encoder, &registered) != NV_ENC_SUCCESS) {
        blocked("nvenc_register_d3d12_resource", "d3d12-resource-registration");
        blocked("nvenc_directx_map", "not-run-after-register-block");
        blocked("nvenc_directx_actual_encode", "not-run-after-register-block");
        api.nvEncDestroyEncoder(encoder);
        dlclose(nv_lib);
        return 3;
    }
    std::printf("nvenc_register_d3d12_resource=PASS\n");

    NV_ENC_MAP_INPUT_RESOURCE mapped = {};
    mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapped.registeredResource = registered.registeredResource;
    if (api.nvEncMapInputResource(encoder, &mapped) != NV_ENC_SUCCESS) {
        blocked("nvenc_directx_map", "d3d12-resource-map");
        blocked("nvenc_directx_actual_encode", "not-run-after-map-block");
        api.nvEncUnregisterResource(encoder, registered.registeredResource);
        api.nvEncDestroyEncoder(encoder);
        dlclose(nv_lib);
        return 3;
    }
    std::printf("nvenc_directx_map=PASS\n");

    NV_ENC_CREATE_BITSTREAM_BUFFER output = {};
    output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (api.nvEncCreateBitstreamBuffer(encoder, &output) != NV_ENC_SUCCESS) {
        blocked("nvenc_directx_actual_encode", "bitstream-buffer");
        api.nvEncUnmapInputResource(encoder, mapped.mappedResource);
        api.nvEncUnregisterResource(encoder, registered.registeredResource);
        api.nvEncDestroyEncoder(encoder);
        dlclose(nv_lib);
        return 3;
    }
    const char *stream_path = "/tmp/appsandbox-d3d12-nvenc-direct.hevc";
    FILE *stream = std::fopen(stream_path, "wb");
    NV_ENC_PIC_PARAMS picture = {};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputBuffer = mapped.mappedResource;
    picture.bufferFmt = mapped.mappedBufferFmt;
    picture.inputWidth = 3840;
    picture.inputHeight = 2160;
    picture.inputPitch = 0;
    picture.outputBitstream = output.bitstreamBuffer;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR |
                             NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    const bool encoded = stream &&
        api.nvEncEncodePicture(encoder, &picture) == NV_ENC_SUCCESS;
    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = output.bitstreamBuffer;
    const bool locked = encoded &&
        api.nvEncLockBitstream(encoder, &lock) == NV_ENC_SUCCESS;
    if (locked && std::fwrite(lock.bitstreamBufferPtr, 1,
                              lock.bitstreamSizeInBytes, stream) !=
                      lock.bitstreamSizeInBytes) {
        api.nvEncUnlockBitstream(encoder, output.bitstreamBuffer);
        if (stream) std::fclose(stream);
        stream = nullptr;
        api.nvEncDestroyBitstreamBuffer(encoder, output.bitstreamBuffer);
        api.nvEncUnmapInputResource(encoder, mapped.mappedResource);
        api.nvEncUnregisterResource(encoder, registered.registeredResource);
        api.nvEncDestroyEncoder(encoder);
        blocked("nvenc_directx_actual_encode", "stream-write");
        dlclose(nv_lib);
        return 3;
    }
    if (locked) api.nvEncUnlockBitstream(encoder, output.bitstreamBuffer);
    if (stream) std::fclose(stream);
    std::string decoded_format;
    const bool decoded = locked && decode_verify(stream_path, &decoded_format);
    if (locked)
        std::printf("nvenc_directx_decoded_pix_fmt=%s\n",
                    decoded_format.empty() ? "unknown" : decoded_format.c_str());
    std::printf("nvenc_directx_actual_encode=%s\n",
                encoded && locked && decoded ? "PASS" : "BLOCKED");
    api.nvEncDestroyBitstreamBuffer(encoder, output.bitstreamBuffer);
    api.nvEncUnmapInputResource(encoder, mapped.mappedResource);
    api.nvEncUnregisterResource(encoder, registered.registeredResource);
    api.nvEncDestroyEncoder(encoder);
    unlink(stream_path);
    dlclose(nv_lib);
    return encoded && locked && decoded ? 0 : 3;
}
