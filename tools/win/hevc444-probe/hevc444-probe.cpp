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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "hevc_access_unit_probe.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "mf.lib")
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

static const char *dxgi_format_reason_name(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_NV12: return "nv12";
    case DXGI_FORMAT_P010: return "p010";
    case DXGI_FORMAT_AYUV: return "ayuv";
    case DXGI_FORMAT_Y410: return "y410";
    default: return "other";
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
    UINT adapter_index = std::numeric_limits<UINT>::max();
    DXGI_ADAPTER_DESC1 adapter_desc = {};
};

static bool create_d3d11_context(UINT requested_adapter, D3D11Context *result)
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
        if (FAILED(adapter->GetDesc1(&desc)))
            continue;
        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (software && requested_adapter != std::numeric_limits<UINT>::max() &&
            index == requested_adapter) {
            std::fprintf(stderr,
                         "BLOCKED stage=adapter reason=software index=%u\n",
                         index);
            continue;
        }
        if ((requested_adapter != std::numeric_limits<UINT>::max() &&
             index != requested_adapter) ||
            (requested_adapter == std::numeric_limits<UINT>::max() && software))
            continue;
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        hr = D3D11CreateDevice(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device,
            nullptr, &context);
        if (FAILED(hr)) {
            const D3D_FEATURE_LEVEL fallback_levels[] = {D3D_FEATURE_LEVEL_11_0};
            hr = D3D11CreateDevice(
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                fallback_levels, ARRAYSIZE(fallback_levels), D3D11_SDK_VERSION,
                &device, nullptr, &context);
        }
        if (SUCCEEDED(hr)) {
            result->adapter = adapter;
            result->device = device;
            result->context = context;
            result->adapter_index = index;
            result->adapter_desc = desc;
            break;
        }
        if (requested_adapter != std::numeric_limits<UINT>::max())
            break;
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
    std::printf("selected_adapter_index=%u\n", result->adapter_index);
    std::printf("selected_adapter_name=%ls\n", result->adapter_desc.Description);
    std::printf("selected_adapter_vendor_id=0x%04x\n",
                result->adapter_desc.VendorId);
    std::printf("selected_adapter_device_id=0x%04x\n",
                result->adapter_desc.DeviceId);
    std::printf("selected_adapter_luid=%08x:%08x\n",
                static_cast<unsigned>(result->adapter_desc.AdapterLuid.HighPart),
                static_cast<unsigned>(result->adapter_desc.AdapterLuid.LowPart));
    return true;
}

struct HevcAnnexBStream {
    std::vector<std::uint8_t> sequence_header;
    HevcAccessUnit first_irap;
    bool has_vps = false;
    bool has_sps = false;
    bool has_pps = false;
    bool first_irap_found = false;
};

static bool parse_hevc_annex_b(const std::vector<std::uint8_t> &bytes,
                               HevcAnnexBStream *stream)
{
    if (!stream)
        return false;
    *stream = {};
    std::vector<hevc_access_unit_probe::Nal> nals;
    if (!hevc_access_unit_probe::split(bytes, &nals))
        return false;

    std::vector<std::uint8_t> vps;
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;
    for (const auto &nal : nals) {
        if (nal.type == 32 && vps.empty()) {
            vps = nal.bytes;
            stream->has_vps = true;
        } else if (nal.type == 33 && sps.empty()) {
            sps = nal.bytes;
            stream->has_sps = true;
        } else if (nal.type == 34 && pps.empty()) {
            pps = nal.bytes;
            stream->has_pps = true;
        }
    }
    if (!stream->has_vps || !stream->has_sps || !stream->has_pps)
        return false;
    stream->sequence_header.insert(stream->sequence_header.end(), vps.begin(), vps.end());
    stream->sequence_header.insert(stream->sequence_header.end(), sps.begin(), sps.end());
    stream->sequence_header.insert(stream->sequence_header.end(), pps.begin(), pps.end());
    stream->first_irap_found =
        hevc_access_unit_probe::extract_first_irap_access_unit(bytes,
                                                               &stream->first_irap);
    return stream->first_irap_found && !stream->sequence_header.empty();
}

static bool read_stream_file(const std::wstring &path,
                             std::vector<std::uint8_t> *bytes)
{
    if (!bytes)
        return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        print_hr("stream-open", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 &&
              static_cast<unsigned long long>(size.QuadPart) <=
                  static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max());
    if (!ok) {
        print_hr("stream-size", HRESULT_FROM_WIN32(GetLastError()));
        CloseHandle(file);
        return false;
    }
    bytes->resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t total = 0;
    while (total < bytes->size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            bytes->size() - total, std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (!ReadFile(file, bytes->data() + total, request, &read, nullptr) ||
            read == 0) {
            print_hr("stream-read", HRESULT_FROM_WIN32(GetLastError()));
            CloseHandle(file);
            return false;
        }
        total += read;
    }
    CloseHandle(file);
    return true;
}

struct MfResult {
    bool hardware_decoder = false;
    bool d3d_manager = false;
    bool output_nv12 = false;
    bool output_p010 = false;
    bool output_ayuv = false;
    bool actual_decode = false;
    bool decoded_gpu_surface = false;
    bool decoded_format_ayuv = false;
    UINT decoded_frames = 0;
    ComPtr<ID3D11Texture2D> decoded_texture;
    UINT decoded_subresource = 0;
};

struct MfDecoderResult {
    bool output_nv12 = false;
    bool output_p010 = false;
    bool output_ayuv = false;
};

static void print_mf_output_type(UINT index, IMFMediaType *type,
                                 MfDecoderResult *result)
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

static bool configure_mf_input(IMFTransform *transform,
                               const HevcAnnexBStream &stream)
{
    if (stream.sequence_header.size() > std::numeric_limits<UINT>::max()) {
        std::fputs("BLOCKED stage=mf-hevc-input-type "
                   "reason=sequence-header-too-large\n", stderr);
        return false;
    }
    ComPtr<IMFMediaType> input;
    HRESULT hr = MFCreateMediaType(&input);
    if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE,
                                               kWidth, kHeight);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(
        input.Get(), MF_MT_FRAME_RATE, kFpsNumerator, kFpsDenominator);
    if (SUCCEEDED(hr)) hr = input->SetBlob(
        MF_MT_MPEG_SEQUENCE_HEADER, stream.sequence_header.data(),
        static_cast<UINT>(stream.sequence_header.size()));
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

static bool make_input_sample(const HevcAnnexBStream &stream,
                              IMFSample **sample)
{
    if (!sample || !stream.first_irap_found || stream.first_irap.bytes.empty() ||
        stream.first_irap.bytes.size() > std::numeric_limits<DWORD>::max())
        return false;
    *sample = nullptr;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(
        static_cast<DWORD>(stream.first_irap.bytes.size()),
                                      &buffer);
    if (SUCCEEDED(hr)) {
        BYTE *destination = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;
        hr = buffer->Lock(&destination, &max_length, &current_length);
        if (SUCCEEDED(hr)) {
            std::memcpy(destination, stream.first_irap.bytes.data(),
                        stream.first_irap.bytes.size());
            buffer->Unlock();
            hr = buffer->SetCurrentLength(
                static_cast<DWORD>(stream.first_irap.bytes.size()));
        }
    }
    ComPtr<IMFSample> created;
    if (SUCCEEDED(hr)) hr = MFCreateSample(&created);
    if (SUCCEEDED(hr)) hr = created->AddBuffer(buffer.Get());
    if (SUCCEEDED(hr)) hr = created->SetSampleTime(0);
    if (SUCCEEDED(hr)) hr = created->SetSampleDuration(
        10000000LL / kFpsNumerator);
    if (SUCCEEDED(hr)) hr = created->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    if (FAILED(hr))
        return false;
    *sample = created.Detach();
    return true;
}

static bool make_ayuv_output_sample(const D3D11Context &d3d11,
                                    IMFSample **sample)
{
    if (!sample)
        return false;
    *sample = nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_AYUV;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DECODER;
    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = d3d11.device->CreateTexture2D(&desc, nullptr, &texture);
    ComPtr<IMFMediaBuffer> buffer;
    if (SUCCEEDED(hr)) hr = MFCreateDXGISurfaceBuffer(
        IID_ID3D11Texture2D, texture.Get(), 0, FALSE, &buffer);
    ComPtr<IMFSample> created;
    if (SUCCEEDED(hr)) hr = MFCreateSample(&created);
    if (SUCCEEDED(hr)) hr = created->AddBuffer(buffer.Get());
    if (FAILED(hr))
        return false;
    *sample = created.Detach();
    return true;
}

static bool inspect_mf_output(IMFSample *sample, MfResult *result,
                              ComPtr<ID3D11Texture2D> *texture,
                              UINT *subresource, const char *prefix)
{
    if (!sample || !result || !texture || !subresource)
        return false;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    ComPtr<IMFDXGIBuffer> dxgi_buffer;
    if (SUCCEEDED(hr)) hr = buffer.As(&dxgi_buffer);
    if (FAILED(hr) || !dxgi_buffer) {
        std::fprintf(stderr,
                     "BLOCKED stage=mf-hevc444-actual-decode "
                     "reason=not-dxgi-backed decoder=%s\n", prefix);
        return false;
    }
    UINT output_subresource = 0;
    if (SUCCEEDED(hr)) hr = dxgi_buffer->GetSubresourceIndex(&output_subresource);
    ComPtr<ID3D11Texture2D> output_texture;
    if (SUCCEEDED(hr)) hr = dxgi_buffer->GetResource(
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void **>(output_texture.GetAddressOf()));
    if (FAILED(hr) || !output_texture) {
        std::fprintf(stderr,
                     "BLOCKED stage=mf-hevc444-actual-decode "
                     "reason=not-dxgi-backed decoder=%s\n", prefix);
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    output_texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_AYUV) {
        std::fprintf(stderr,
                     "BLOCKED stage=mf-hevc444-actual-decode "
                     "reason=decoder-returned-%s decoder=%s\n",
                     dxgi_format_reason_name(desc.Format), prefix);
        return false;
    }
    *texture = output_texture;
    *subresource = output_subresource;
    result->decoded_gpu_surface = true;
    result->decoded_format_ayuv = true;
    return true;
}

static bool probe_one_media_foundation_decoder(
    const D3D11Context &d3d11, IMFActivate *activate, UINT index,
    const HevcAnnexBStream &stream, MfResult *aggregate,
    ComPtr<ID3D11Texture2D> *decoded_texture, UINT *decoded_subresource)
{
    WCHAR *name = nullptr;
    UINT32 name_length = 0;
    GUID clsid = GUID_NULL;
    (void)activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name,
                                        &name_length);
    (void)activate->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid);
    const wchar_t *decoder_name = name ? name : L"(unknown)";
    std::printf("decoder[%u] name=%ls CLSID=%s\n", index, decoder_name,
                guid_string(clsid).c_str());

    ComPtr<IMFTransform> decoder;
    HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&decoder));
    if (FAILED(hr)) {
        std::printf("decoder[%u] mf_output_ayuv=0 actual_decode=BLOCKED "
                    "reason=activate\n", index);
        if (name) CoTaskMemFree(name);
        return false;
    }
    UINT reset_token = 0;
    ComPtr<IMFDXGIDeviceManager> manager;
    hr = MFCreateDXGIDeviceManager(&reset_token, &manager);
    if (SUCCEEDED(hr)) hr = manager->ResetDevice(d3d11.device.Get(), reset_token);
    if (SUCCEEDED(hr)) hr = decoder->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(manager.Get()));
    if (FAILED(hr)) {
        print_hr("mf-d3d-manager", hr);
        std::printf("decoder[%u] mf_output_ayuv=0 actual_decode=BLOCKED "
                    "reason=d3d-manager\n", index);
        if (name) CoTaskMemFree(name);
        return false;
    }
    aggregate->d3d_manager = true;
    if (!configure_mf_input(decoder.Get(), stream)) {
        std::printf("decoder[%u] mf_output_ayuv=0 actual_decode=BLOCKED "
                    "reason=input-type\n", index);
        if (name) CoTaskMemFree(name);
        return false;
    }

    MfDecoderResult decoder_result;
    ComPtr<IMFMediaType> ayuv_type;
    for (UINT type_index = 0;; ++type_index) {
        ComPtr<IMFMediaType> type;
        hr = decoder->GetOutputAvailableType(0, type_index, &type);
        if (hr == MF_E_NO_MORE_TYPES)
            break;
        if (FAILED(hr))
            break;
        print_mf_output_type(type_index, type.Get(), &decoder_result);
        GUID subtype = GUID_NULL;
        if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            IsEqualGUID(subtype, MFVideoFormat_AYUV) && !ayuv_type)
            ayuv_type = type;
    }
    aggregate->output_nv12 |= decoder_result.output_nv12;
    aggregate->output_p010 |= decoder_result.output_p010;
    aggregate->output_ayuv |= decoder_result.output_ayuv;
    std::printf("decoder[%u] mf_output_ayuv=%u\n", index,
                decoder_result.output_ayuv ? 1U : 0U);
    if (!ayuv_type) {
        std::printf("decoder[%u] actual_decode=BLOCKED reason=no-ayuv-output\n",
                    index);
        if (name) CoTaskMemFree(name);
        return false;
    }
    hr = decoder->SetOutputType(0, ayuv_type.Get(), 0);
    if (FAILED(hr)) {
        print_hr("mf-output-ayuv", hr);
        std::printf("decoder[%u] actual_decode=BLOCKED reason=set-output-type\n",
                    index);
        if (name) CoTaskMemFree(name);
        return false;
    }
    if (FAILED(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
        FAILED(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) {
        std::printf("decoder[%u] actual_decode=BLOCKED reason=begin-streaming\n",
                    index);
        if (name) CoTaskMemFree(name);
        return false;
    }
    ComPtr<IMFSample> input_sample;
    if (!make_input_sample(stream, &input_sample)) {
        std::printf("decoder[%u] actual_decode=BLOCKED reason=input-sample\n",
                    index);
        if (name) CoTaskMemFree(name);
        return false;
    }
    hr = decoder->ProcessInput(0, input_sample.Get(), 0);
    if (FAILED(hr)) {
        std::fprintf(stderr,
                     "BLOCKED stage=mf-hevc444-actual-decode "
                     "reason=process-input decoder=%u HRESULT=0x%08lx\n",
                     index, static_cast<unsigned long>(hr));
        std::printf("decoder[%u] actual_decode=BLOCKED reason=process-input\n",
                    index);
        if (name) CoTaskMemFree(name);
        return false;
    }

    MFT_OUTPUT_STREAM_INFO stream_info = {};
    hr = decoder->GetOutputStreamInfo(0, &stream_info);
    if (FAILED(hr)) {
        std::printf("decoder[%u] actual_decode=BLOCKED reason=output-info\n",
                    index);
        if (name) CoTaskMemFree(name);
        return false;
    }
    for (unsigned attempt = 0; attempt != 32; ++attempt) {
        ComPtr<IMFSample> supplied_sample;
        if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0 &&
            !make_ayuv_output_sample(d3d11, &supplied_sample))
            break;
        MFT_OUTPUT_DATA_BUFFER output = {};
        output.dwStreamID = 0;
        output.pSample = supplied_sample.Get();
        DWORD status = 0;
        hr = decoder->ProcessOutput(0, 1, &output, &status);
        ComPtr<IMFSample> produced_sample;
        if (output.pSample == supplied_sample.Get())
            produced_sample = supplied_sample;
        else if (output.pSample)
            produced_sample.Attach(output.pSample);
        if (output.pEvents)
            output.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            (void)decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            continue;
        }
        if (FAILED(hr)) {
            std::fprintf(stderr,
                         "BLOCKED stage=mf-hevc444-actual-decode "
                         "reason=process-output decoder=%u HRESULT=0x%08lx\n",
                         index, static_cast<unsigned long>(hr));
            break;
        }
        if (!produced_sample) {
            std::fprintf(stderr,
                         "BLOCKED stage=mf-hevc444-actual-decode "
                         "reason=process-output decoder=%u\n", index);
            break;
        }
        ComPtr<ID3D11Texture2D> texture;
        UINT subresource = 0;
        if (!inspect_mf_output(produced_sample.Get(), aggregate, &texture,
                               &subresource, "mft"))
            break;
        aggregate->actual_decode = true;
        aggregate->decoded_frames = 1;
        aggregate->decoded_texture = texture;
        aggregate->decoded_subresource = subresource;
        *decoded_texture = texture;
        *decoded_subresource = subresource;
        std::printf("decoder[%u] actual_decode=PASS decoded_frames=1\n", index);
        if (name) CoTaskMemFree(name);
        return true;
    }
    std::printf("decoder[%u] actual_decode=BLOCKED\n", index);
    if (name) CoTaskMemFree(name);
    return false;
}

static bool probe_media_foundation(const D3D11Context &d3d11,
                                   const HevcAnnexBStream &stream,
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
    std::printf("mf_hevc_hw_decoder_count=%u\n", count);
    bool actual = false;
    ComPtr<ID3D11Texture2D> decoded_texture;
    UINT decoded_subresource = 0;
    for (UINT i = 0; i < count; ++i) {
        actual = probe_one_media_foundation_decoder(
                     d3d11, activates[i], i, stream, result, &decoded_texture,
                     &decoded_subresource) || actual;
    }
    for (UINT i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    result->actual_decode = actual;
    if (actual) {
        result->decoded_texture = decoded_texture;
        result->decoded_subresource = decoded_subresource;
        std::printf("PASS stage=mf-hevc444-actual-decode decoded_frames=%u "
                    "decoded_format=AYUV gpu_surface=1 software_fallback=0 "
                    "cpu_frame_copy=0\n", result->decoded_frames);
    }
    return actual;
}

struct D3D12Result {
    bool main444_capability = false;
    bool ayuv_capability = false;
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
    result->main444_capability = true;
    result->ayuv_capability = true;

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
    std::puts("PASS stage=d3d12-decode-objects decoder=1 heap=1 output=AYUV "
              "capability_only=1");
    std::printf("d3d12_decode_main444_capability=%u\n",
                result->main444_capability ? 1U : 0U);
    std::printf("d3d12_decode_ayuv_capability=%u\n",
                result->ayuv_capability ? 1U : 0U);
    std::puts("d3d12_actual_decode=not-tested");
    return true;
}

static bool probe_ayuv_to_rgb(const D3D11Context &d3d11,
                              ID3D11Texture2D *decoded_texture,
                              UINT decoded_subresource)
{
    if (!decoded_texture) {
        std::fputs("BLOCKED stage=host-ayuv-to-rgb-video-processor "
                   "reason=no-decoded-surface\n", stderr);
        return false;
    }
    D3D11_TEXTURE2D_DESC decoded_desc = {};
    decoded_texture->GetDesc(&decoded_desc);
    if (decoded_desc.Format != DXGI_FORMAT_AYUV) {
        std::fputs("BLOCKED stage=host-ayuv-to-rgb-video-processor "
                   "reason=input-not-ayuv\n", stderr);
        return false;
    }
    const UINT mip_levels = std::max<UINT>(decoded_desc.MipLevels, 1);
    const UINT array_slice = decoded_subresource / mip_levels;
    const UINT mip_slice = decoded_subresource % mip_levels;
    if (array_slice >= decoded_desc.ArraySize) {
        std::fputs("BLOCKED stage=host-ayuv-to-rgb-video-processor "
                   "reason=invalid-subresource\n", stderr);
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
    HRESULT hr = d3d11.video_device->CreateVideoProcessorEnumerator(
        &content, &enumerator);
    if (SUCCEEDED(hr)) {
        D3D11_VIDEO_PROCESSOR_CAPS caps = {};
        hr = enumerator->GetVideoProcessorCaps(&caps);
    }
    UINT ayuv_caps = 0, bgra_caps = 0;
    if (SUCCEEDED(hr)) hr = enumerator->CheckVideoProcessorFormat(
        DXGI_FORMAT_AYUV, &ayuv_caps);
    if (SUCCEEDED(hr)) hr = enumerator->CheckVideoProcessorFormat(
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
    ComPtr<ID3D11VideoProcessor> processor;
    if (SUCCEEDED(hr)) hr = d3d11.video_device->CreateVideoProcessor(
        enumerator.Get(), 0, &processor);
    if (FAILED(hr)) {
        print_hr("host-ayuv-to-rgb-video-processor", hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc = {};
    input_view_desc.FourCC = 0;
    input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_view_desc.Texture2D.MipSlice = mip_slice;
    input_view_desc.Texture2D.ArraySlice = array_slice;
    ComPtr<ID3D11VideoProcessorInputView> input_view;
    hr = d3d11.video_device->CreateVideoProcessorInputView(
        decoded_texture, enumerator.Get(), &input_view_desc, &input_view);
    D3D11_TEXTURE2D_DESC output_desc = decoded_desc;
    output_desc.Width = kWidth;
    output_desc.Height = kHeight;
    output_desc.MipLevels = 1;
    output_desc.ArraySize = 1;
    output_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    output_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> output_texture;
    if (SUCCEEDED(hr)) hr = d3d11.device->CreateTexture2D(
        &output_desc, nullptr, &output_texture);
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

static bool parse_command_line(int argc, wchar_t **argv, UINT *adapter_index,
                               std::wstring *stream_path)
{
    if (!adapter_index || !stream_path)
        return false;
    *adapter_index = std::numeric_limits<UINT>::max();
    stream_path->clear();
    if (argc == 2 && std::wcscmp(argv[1], L"--help") == 0) {
        std::wprintf(L"Usage: %ls [--adapter <index>] <hevc444-stream>\n", argv[0]);
        return false;
    }
    if (argc == 2) {
        *stream_path = argv[1];
        return true;
    }
    if (argc == 4 && std::wcscmp(argv[1], L"--adapter") == 0) {
        wchar_t *end = nullptr;
        const unsigned long parsed = std::wcstoul(argv[2], &end, 10);
        if (!end || *end != L'\0' || parsed > std::numeric_limits<UINT>::max()) {
            std::fputs("BLOCKED stage=command-line reason=invalid-adapter\n", stderr);
            return false;
        }
        *adapter_index = static_cast<UINT>(parsed);
        *stream_path = argv[3];
        return true;
    }
    std::fputs("BLOCKED stage=command-line reason=usage\n", stderr);
    return false;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    UINT requested_adapter = std::numeric_limits<UINT>::max();
    std::wstring stream_path;
    if (!parse_command_line(argc, argv, &requested_adapter, &stream_path))
        return 2;
    std::vector<std::uint8_t> stream_bytes;
    if (!read_stream_file(stream_path, &stream_bytes))
        return 1;
    HevcAnnexBStream stream;
    if (!parse_hevc_annex_b(stream_bytes, &stream)) {
        std::fputs("BLOCKED stage=stream-parse reason=missing-vps-sps-pps-or-irap\n",
                   stderr);
        return 1;
    }
    std::printf("guest_stream_vps=%u\n", stream.has_vps ? 1U : 0U);
    std::printf("guest_stream_sps=%u\n", stream.has_sps ? 1U : 0U);
    std::printf("guest_stream_pps=%u\n", stream.has_pps ? 1U : 0U);
    std::printf("guest_first_irap_found=%u\n",
                stream.first_irap_found ? 1U : 0U);
    std::printf("guest_first_irap_bytes=%zu\n",
                stream.first_irap.bytes.size());
    std::printf("guest_first_irap_nal_count=%zu\n",
                stream.first_irap.nal_count);
    std::printf("guest_first_irap_clean_point=1\n");

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
    const bool device_ok = create_d3d11_context(requested_adapter, &d3d11);
    const bool mf_ok = device_ok && probe_media_foundation(d3d11, stream, &mf);
    const bool d3d12_ok = device_ok && probe_d3d12_decode(d3d11, &d3d12);
    const bool presentation_ok =
        mf_ok && mf.actual_decode && mf.decoded_gpu_surface &&
        mf.decoded_format_ayuv &&
        probe_ayuv_to_rgb(d3d11, mf.decoded_texture.Get(),
                          mf.decoded_subresource);
    const bool mf_path = mf_ok && mf.hardware_decoder && mf.d3d_manager &&
                         mf.actual_decode && mf.decoded_gpu_surface &&
                         mf.decoded_format_ayuv && presentation_ok;
    const bool d3d12_path = false;

    std::puts("=== AppSandbox HEVC444 Host Capability ===");
    std::printf("mf_hevc_hw_decoder=%u\n", mf.hardware_decoder ? 1U : 0U);
    std::printf("mf_d3d_manager=%u\n", mf.d3d_manager ? 1U : 0U);
    std::printf("mf_output_nv12=%u\n", mf.output_nv12 ? 1U : 0U);
    std::printf("mf_output_p010=%u\n", mf.output_p010 ? 1U : 0U);
    std::printf("mf_output_ayuv=%u\n", mf.output_ayuv ? 1U : 0U);
    std::printf("mf_actual_hevc444_decode=%u\n", mf.actual_decode ? 1U : 0U);
    std::printf("mf_decoded_frames=%u\n", mf.decoded_frames);
    std::printf("mf_decoded_gpu_surface=%u\n",
                mf.decoded_gpu_surface ? 1U : 0U);
    std::printf("mf_decoded_format_ayuv=%u\n",
                mf.decoded_format_ayuv ? 1U : 0U);
    std::printf("d3d12_decode_main444_capability=%u\n",
                d3d12.main444_capability ? 1U : 0U);
    std::printf("d3d12_decode_ayuv_capability=%u\n",
                d3d12.ayuv_capability ? 1U : 0U);
    (void)d3d12_ok;
    std::puts("d3d12_actual_decode=not-tested");
    std::printf("ayuv_to_rgb_video_processor=%u\n",
                presentation_ok ? 1U : 0U);
    std::puts("hardware_decode_only=1");
    const char *path = mf_path ? "MF" : (d3d12_path ? "D3D12" : "UNAVAILABLE");
    std::printf("HEVC444_PATH=%s\n", path);

    MFShutdown();
    CoUninitialize();
    return (mf_path || d3d12_path) ? 0 : 1;
}
