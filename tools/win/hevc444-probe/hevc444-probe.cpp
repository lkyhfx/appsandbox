/* SPDX-License-Identifier: MIT
 *
 * Standalone Windows host diagnostic for the HEVC Main 4:4:4 path.
 * This executable intentionally does not share production decoder code.
 */

#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kWidth = 3840;
constexpr UINT kHeight = 2160;
constexpr UINT kFpsNumerator = 60;
constexpr UINT kFpsDenominator = 1;

static void print_hr(const char *stage, HRESULT hr)
{
    std::fprintf(stderr, "BLOCKED stage=%s HRESULT=0x%08lx\n", stage,
                 static_cast<unsigned long>(hr));
}

static const char *dxgi_format_name(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_NV12: return "NV12";
    case DXGI_FORMAT_P010: return "P010";
    case DXGI_FORMAT_AYUV: return "AYUV";
    case DXGI_FORMAT_Y410: return "Y410";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    default: return "OTHER";
    }
}

static std::string guid_string(const GUID &guid)
{
    wchar_t text[64] = {};
    if (!StringFromGUID2(guid, text, ARRAYSIZE(text)))
        return "{unknown}";
    char result[64] = {};
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, result,
                            static_cast<int>(sizeof(result)), nullptr, nullptr) == 0)
        return "{unknown}";
    return result;
}

struct D3D11Context {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> video_device;
    ComPtr<ID3D11VideoContext> video_context;
    ComPtr<ID3D11VideoContext1> video_context1;
};

static bool create_d3d11_context(D3D11Context *result)
{
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        print_hr("host-dxgi-factory", hr);
        return false;
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc)) ||
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            continue;
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        hr = D3D11CreateDevice(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &result->device,
            nullptr, &result->context);
        if (SUCCEEDED(hr)) {
            result->adapter = adapter;
            break;
        }
    }
    if (!result->device) {
        std::fputs("BLOCKED stage=host-d3d11-device\n", stderr);
        return false;
    }
    hr = result->device.As(&result->video_device);
    if (SUCCEEDED(hr)) hr = result->context.As(&result->video_context);
    if (SUCCEEDED(hr)) hr = result->context.As(&result->video_context1);
    if (FAILED(hr)) {
        print_hr("host-d3d11-video-interfaces", hr);
        return false;
    }
    return true;
}

struct MfResult {
    bool hardware_decoder = false;
    bool d3d_manager = false;
    bool output_nv12 = false;
    bool output_p010 = false;
    bool output_ayuv = false;
};

static void print_mf_output_type(UINT index, IMFMediaType *type,
                                 MfResult *result)
{
    GUID subtype = GUID_NULL;
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)))
        return;
    const char *name = "OTHER";
    if (IsEqualGUID(subtype, MFVideoFormat_NV12)) {
        name = "NV12"; result->output_nv12 = true;
    } else if (IsEqualGUID(subtype, MFVideoFormat_P010)) {
        name = "P010"; result->output_p010 = true;
    } else if (IsEqualGUID(subtype, MFVideoFormat_AYUV)) {
        name = "AYUV"; result->output_ayuv = true;
    } else if (IsEqualGUID(subtype, MFVideoFormat_Y410)) {
        name = "Y410";
    }
    std::printf("mf_output_type index=%u subtype=%s guid=%s\n", index, name,
                guid_string(subtype).c_str());
}

static bool configure_mf_input(IMFTransform *transform)
{
    ComPtr<IMFMediaType> input;
    HRESULT hr = MFCreateMediaType(&input);
    if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE,
                                               kWidth, kHeight);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(
        input.Get(), MF_MT_FRAME_RATE, kFpsNumerator, kFpsDenominator);
    if (FAILED(hr)) {
        print_hr("mf-hevc-input-type", hr);
        return false;
    }
    hr = transform->SetInputType(0, input.Get(), 0);
    if (FAILED(hr)) {
        print_hr("mf-hevc-input-type", hr);
        return false;
    }
    return true;
}

static bool probe_media_foundation(const D3D11Context &d3d11,
                                   MfResult *result)
{
    MFT_REGISTER_TYPE_INFO input = {MFMediaType_Video, MFVideoFormat_HEVC};
    IMFActivate **activates = nullptr;
    UINT count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                           &input, nullptr, &activates, &count);
    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        if (FAILED(hr)) print_hr("mf-hevc-hw-decoder", hr);
        else std::fputs("BLOCKED stage=mf-hevc-hw-decoder count=0\n", stderr);
        return false;
    }
    result->hardware_decoder = true;
    std::printf("PASS stage=mf-hevc-hw-decoder count=%u\n", count);

    IMFTransform *transform = nullptr;
    for (UINT i = 0; i < count; ++i) {
        WCHAR *name = nullptr;
        UINT32 name_length = 0;
        GUID clsid = GUID_NULL;
        (void)activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, &name,
                                      &name_length);
        (void)activates[i]->GetGUID(MFT_CLSID_Attribute, &clsid);
        std::printf("decoder name=%ls CLSID=%s hardware_status=1\n",
                    name ? name : L"(unknown)", guid_string(clsid).c_str());
        if (name) CoTaskMemFree(name);
        if (!transform) {
            IMFTransform *activated = nullptr;
            if (SUCCEEDED(activates[i]->ActivateObject(
                              IID_PPV_ARGS(&activated))))
                transform = activated;
        }
    }
    for (UINT i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (!transform) {
        std::fputs("BLOCKED stage=mf-hevc-transform-activate\n", stderr);
        return false;
    }
    ComPtr<IMFTransform> decoder;
    decoder.Attach(transform);

    UINT reset_token = 0;
    ComPtr<IMFDXGIDeviceManager> manager;
    hr = MFCreateDXGIDeviceManager(&reset_token, &manager);
    if (SUCCEEDED(hr)) hr = manager->ResetDevice(d3d11.device.Get(), reset_token);
    if (SUCCEEDED(hr)) hr = decoder->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get()));
    if (FAILED(hr)) {
        print_hr("mf-d3d-manager", hr);
        return false;
    }
    result->d3d_manager = true;
    std::puts("PASS stage=mf-d3d-manager");

    if (!configure_mf_input(decoder.Get()))
        return false;
    for (UINT index = 0;; ++index) {
        ComPtr<IMFMediaType> type;
        hr = decoder->GetOutputAvailableType(0, index, &type);
        if (hr == MF_E_NO_MORE_TYPES)
            break;
        if (FAILED(hr)) {
            print_hr("mf-output-available-type", hr);
            return false;
        }
        print_mf_output_type(index, type.Get(), result);
    }
    std::printf("mf_output_nv12=%u\n", result->output_nv12 ? 1U : 0U);
    std::printf("mf_output_p010=%u\n", result->output_p010 ? 1U : 0U);
    std::printf("mf_output_ayuv=%u\n", result->output_ayuv ? 1U : 0U);
    return true;
}

struct D3D12Result {
    bool main444 = false;
    bool ayuv = false;
};

static bool probe_d3d12_decode(const D3D11Context &d3d11,
                               D3D12Result *result)
{
    ComPtr<ID3D12Device> device;
    HRESULT hr = D3D12CreateDevice(d3d11.adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        print_hr("d3d12-device", hr);
        return false;
    }
    ComPtr<ID3D12VideoDevice> video;
    hr = device.As(&video);
    if (FAILED(hr)) {
        print_hr("d3d12-video-device", hr);
        return false;
    }

    D3D12_VIDEO_DECODE_CONFIGURATION configuration = {};
    configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN_444;
    configuration.BitstreamEncryption = D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE;
    configuration.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;
    D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT support = {};
    support.NodeIndex = 0;
    support.Configuration = configuration;
    support.Width = kWidth;
    support.Height = kHeight;
    support.DecodeFormat = DXGI_FORMAT_AYUV;
    support.FrameRate = {kFpsNumerator, kFpsDenominator};
    hr = video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT,
                                    &support, sizeof(support));
    const bool supported = SUCCEEDED(hr) &&
        (support.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED) != 0;
    if (FAILED(hr)) print_hr("d3d12-decode-main444", hr);
    std::printf("%s stage=d3d12-decode-main444 resolution=%ux%u fps=60/1 "
                "supported=%u support_flags=0x%08x configuration_flags=0x%08x\n",
                supported ? "PASS" : "BLOCKED", kWidth, kHeight,
                supported ? 1U : 0U,
                static_cast<unsigned>(support.SupportFlags),
                static_cast<unsigned>(support.ConfigurationFlags));
    if (!supported)
        return false;
    result->ayuv = true;

    D3D12_FEATURE_DATA_VIDEO_DECODE_FORMAT_COUNT format_count = {};
    format_count.NodeIndex = 0;
    format_count.Configuration = configuration;
    hr = video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_FORMAT_COUNT,
                                    &format_count, sizeof(format_count));
    if (SUCCEEDED(hr) && format_count.FormatCount != 0) {
        std::vector<DXGI_FORMAT> formats(format_count.FormatCount);
        D3D12_FEATURE_DATA_VIDEO_DECODE_FORMATS formats_query = {};
        formats_query.NodeIndex = 0;
        formats_query.Configuration = configuration;
        formats_query.FormatCount = format_count.FormatCount;
        formats_query.pOutputFormats = formats.data();
        hr = video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_FORMATS,
                                        &formats_query, sizeof(formats_query));
        if (SUCCEEDED(hr)) {
            for (DXGI_FORMAT format : formats)
                std::printf("d3d12_decode_output_format=%s dxgi_format=%u\n",
                            dxgi_format_name(format),
                            static_cast<unsigned>(format));
        }
    }

    D3D12_VIDEO_DECODER_DESC decoder_desc = {};
    decoder_desc.NodeMask = 1;
    decoder_desc.Configuration = configuration;
    ComPtr<ID3D12VideoDecoder> decoder;
    hr = video->CreateVideoDecoder(&decoder_desc, IID_PPV_ARGS(&decoder));
    if (FAILED(hr)) {
        print_hr("d3d12-decode-create-decoder", hr);
        return false;
    }
    D3D12_VIDEO_DECODER_HEAP_DESC heap_desc = {};
    heap_desc.NodeMask = 1;
    heap_desc.Configuration = configuration;
    heap_desc.DecodeWidth = kWidth;
    heap_desc.DecodeHeight = kHeight;
    heap_desc.Format = DXGI_FORMAT_AYUV;
    heap_desc.FrameRate = {kFpsNumerator, kFpsDenominator};
    heap_desc.MaxDecodePictureBufferCount = 8;
    ComPtr<ID3D12VideoDecoderHeap> heap;
    hr = video->CreateVideoDecoderHeap(&heap_desc, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) {
        print_hr("d3d12-decode-create-heap", hr);
        return false;
    }
    result->main444 = true;
    std::puts("PASS stage=d3d12-decode-objects decoder=1 heap=1 output=AYUV");
    std::printf("d3d12_decode_main444=%u\n", result->main444 ? 1U : 0U);
    std::printf("d3d12_decode_ayuv=%u\n", result->ayuv ? 1U : 0U);
    return true;
}

static bool probe_ayuv_to_rgb(const D3D11Context &d3d11)
{
    UINT ayuv_caps = 0, bgra_caps = 0;
    HRESULT hr = d3d11.video_device->CheckVideoProcessorFormat(
        DXGI_FORMAT_AYUV, &ayuv_caps);
    if (SUCCEEDED(hr)) hr = d3d11.video_device->CheckVideoProcessorFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM, &bgra_caps);
    const bool formats_ok = SUCCEEDED(hr) &&
        (ayuv_caps & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) != 0 &&
        (bgra_caps & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) != 0;
    if (!formats_ok) {
        if (FAILED(hr)) print_hr("host-ayuv-to-rgb-format-query", hr);
        else std::fputs("BLOCKED stage=host-ayuv-to-rgb-video-processor "
                        "reason=format-support\n", stderr);
        return false;
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {kFpsNumerator, kFpsDenominator};
    content.InputWidth = kWidth;
    content.InputHeight = kHeight;
    content.OutputFrameRate = {kFpsNumerator, kFpsDenominator};
    content.OutputWidth = kWidth;
    content.OutputHeight = kHeight;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    hr = d3d11.video_device->CreateVideoProcessorEnumerator(&content,
                                                             &enumerator);
    if (SUCCEEDED(hr)) {
        D3D11_VIDEO_PROCESSOR_CAPS caps = {};
        hr = enumerator->GetVideoProcessorCaps(&caps);
    }
    ComPtr<ID3D11VideoProcessor> processor;
    if (SUCCEEDED(hr)) hr = d3d11.video_device->CreateVideoProcessor(
        enumerator.Get(), 0, &processor);
    if (FAILED(hr)) {
        print_hr("host-ayuv-to-rgb-video-processor", hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC input_desc = {};
    input_desc.Width = kWidth;
    input_desc.Height = kHeight;
    input_desc.MipLevels = 1;
    input_desc.ArraySize = 1;
    input_desc.Format = DXGI_FORMAT_AYUV;
    input_desc.SampleDesc.Count = 1;
    input_desc.Usage = D3D11_USAGE_DEFAULT;
    input_desc.BindFlags = D3D11_BIND_DECODER;
    ComPtr<ID3D11Texture2D> input_texture;
    hr = d3d11.device->CreateTexture2D(&input_desc, nullptr, &input_texture);
    D3D11_TEXTURE2D_DESC output_desc = input_desc;
    output_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    output_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> output_texture;
    if (SUCCEEDED(hr)) hr = d3d11.device->CreateTexture2D(
        &output_desc, nullptr, &output_texture);
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc = {};
    input_view_desc.FourCC = 0;
    input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_view_desc.Texture2D.MipSlice = 0;
    ComPtr<ID3D11VideoProcessorInputView> input_view;
    if (SUCCEEDED(hr)) hr = d3d11.video_device->CreateVideoProcessorInputView(
        input_texture.Get(), enumerator.Get(), &input_view_desc, &input_view);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc = {};
    output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_view_desc.Texture2D.MipSlice = 0;
    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    if (SUCCEEDED(hr)) hr = d3d11.video_device->CreateVideoProcessorOutputView(
        output_texture.Get(), enumerator.Get(), &output_view_desc, &output_view);
    if (FAILED(hr)) {
        print_hr("host-ayuv-to-rgb-video-processor-views", hr);
        return false;
    }

    d3d11.video_context1->VideoProcessorSetStreamColorSpace1(
        processor.Get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    d3d11.video_context1->VideoProcessorSetOutputColorSpace1(
        processor.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    hr = d3d11.video_context->VideoProcessorBlt(
        processor.Get(), output_view.Get(), 0, 1, &stream);
    if (SUCCEEDED(hr)) d3d11.context->Flush();
    if (FAILED(hr)) {
        print_hr("host-ayuv-to-rgb-video-processor", hr);
        return false;
    }
    std::puts("PASS stage=host-ayuv-to-rgb-video-processor input=AYUV "
              "output=BGRA8 color_in=BT.709-studio color_out=BT.709-full");
    return true;
}

} // namespace

int wmain()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        print_hr("com-initialize", hr);
        return 1;
    }
    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        print_hr("mf-startup", hr);
        CoUninitialize();
        return 1;
    }

    D3D11Context d3d11;
    MfResult mf;
    D3D12Result d3d12;
    const bool device_ok = create_d3d11_context(&d3d11);
    const bool mf_ok = device_ok && probe_media_foundation(d3d11, &mf);
    const bool d3d12_ok = device_ok && probe_d3d12_decode(d3d11, &d3d12);
    const bool presentation_ok = device_ok && probe_ayuv_to_rgb(d3d11);
    const bool mf_path = mf_ok && mf.hardware_decoder && mf.d3d_manager &&
                         mf.output_ayuv && presentation_ok;
    const bool d3d12_path = d3d12_ok && d3d12.main444 && d3d12.ayuv &&
                            presentation_ok;

    std::puts("=== AppSandbox HEVC444 Host Capability ===");
    std::printf("mf_hevc_hw_decoder=%u\n", mf.hardware_decoder ? 1U : 0U);
    std::printf("mf_d3d_manager=%u\n", mf.d3d_manager ? 1U : 0U);
    std::printf("mf_output_nv12=%u\n", mf.output_nv12 ? 1U : 0U);
    std::printf("mf_output_p010=%u\n", mf.output_p010 ? 1U : 0U);
    std::printf("mf_output_ayuv=%u\n", mf.output_ayuv ? 1U : 0U);
    std::printf("d3d12_decode_main444=%u\n", d3d12.main444 ? 1U : 0U);
    std::printf("d3d12_decode_ayuv=%u\n", d3d12.ayuv ? 1U : 0U);
    std::printf("ayuv_to_rgb_video_processor=%u\n",
                presentation_ok ? 1U : 0U);
    std::puts("hardware_decode_only=1");
    const char *path = mf_path ? "MF" : (d3d12_path ? "D3D12" : "UNAVAILABLE");
    std::printf("HEVC444_PATH=%s\n", path);

    MFShutdown();
    CoUninitialize();
    return (mf_path || d3d12_path) ? 0 : 1;
}
