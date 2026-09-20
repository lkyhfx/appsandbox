/* SPDX-License-Identifier: MIT
 * Media Foundation hardware HEVC decoder producing GPU D3D11 NV12 or AYUV
 * surfaces. The profile is explicit so a 4:4:4 stream can never silently
 * enter the 4:2:0 path.
 */
#include <windows.h>
#define COBJMACROS
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <mfobjects.h>
#include <dxgi1_2.h>
#include <wchar.h>

#include "vm_video_decode.h"
#include "vm_hevc444_probe_sample.h"
#include "vm_video_decode_d3d12.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

struct VmVideoDecoder {
    VmD3D12Decoder *d3d12;
    BOOL mf_started;
    ID3D11Device *device;
    IMFDXGIDeviceManager *manager;
    IMFTransform *transform;
    UINT reset_token;
    UINT width, height, fps_num, fps_den;
    VmVideoDecodeProfile profile;
    LONGLONG next_time;
    DWORD output_stream;
    BOOL provides_samples;
};

static BOOL device_adapter_luid(ID3D11Device *device, LUID *luid)
{
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC desc;
    HRESULT hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxgi, &adapter);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetDesc(adapter, &desc);
    if (SUCCEEDED(hr)) *luid = desc.AdapterLuid;
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgi) IDXGIDevice_Release(dxgi);
    return SUCCEEDED(hr);
}

static const GUID *output_subtype(VmVideoDecodeProfile profile)
{
    return profile == VM_VIDEO_HEVC444 ? &MFVideoFormat_AYUV : &MFVideoFormat_NV12;
}

static BOOL capture_decoder_identity(IMFActivate *activate,
                                     VmVideoDecodeCapability *out)
{
    GUID clsid = GUID_NULL;
    WCHAR *name = NULL;
    UINT32 name_length = 0;
    HRESULT clsid_hr;
    HRESULT name_hr;

    if (!activate || !out) return FALSE;
    clsid_hr = IMFActivate_GetGUID(activate, &MFT_TRANSFORM_CLSID_Attribute,
                                   &clsid);
    name_hr = IMFActivate_GetAllocatedString(
        activate, &MFT_FRIENDLY_NAME_Attribute, &name, &name_length);
    if (SUCCEEDED(clsid_hr)) out->decoder_clsid = clsid;
    if (SUCCEEDED(name_hr) && name) {
        wcsncpy_s(out->decoder_name, ARRAYSIZE(out->decoder_name), name,
                  _TRUNCATE);
    }
    if (name) CoTaskMemFree(name);
    out->decoder_identity_valid =
        !IsEqualGUID(&out->decoder_clsid, &GUID_NULL) ||
        out->decoder_name[0] != L'\0';
    return out->decoder_identity_valid;
}

static BOOL probe_ayuv_video_processor(ID3D11Device *device, UINT width,
                                       UINT height, ID3D11Texture2D *decoded)
{
    ID3D11VideoDevice *video_device = NULL;
    ID3D11DeviceContext *device_context = NULL;
    ID3D11VideoContext *video_context = NULL;
    ID3D11VideoProcessorEnumerator *enumerator = NULL;
    ID3D11VideoProcessor *processor = NULL;
    ID3D11Texture2D *input_texture = NULL, *output_texture = NULL;
    ID3D11VideoProcessorInputView *input_view = NULL;
    ID3D11VideoProcessorOutputView *output_view = NULL;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc;
    D3D11_TEXTURE2D_DESC texture_desc;
    D3D11_VIDEO_PROCESSOR_STREAM stream;
    HRESULT hr;
    BOOL ok = FALSE;

    if (!device) return FALSE;
    hr = ID3D11Device_QueryInterface(device, &IID_ID3D11VideoDevice,
                                     (void **)&video_device);
    if (SUCCEEDED(hr)) {
        /* ID3D11VideoContext is implemented by the immediate context, not
           by ID3D11Device. Querying the device can succeed on some drivers
           only for unrelated interfaces and is never the valid video path. */
        ID3D11Device_GetImmediateContext(device, &device_context);
        if (!device_context) hr = E_NOINTERFACE;
    }
    if (SUCCEEDED(hr))
        hr = ID3D11DeviceContext_QueryInterface(
            device_context, &IID_ID3D11VideoContext, (void **)&video_context);
    if (FAILED(hr)) goto done;

    ZeroMemory(&content, sizeof(content));
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = width;
    content.InputHeight = height;
    content.OutputWidth = width;
    content.OutputHeight = height;
    content.InputFrameRate.Numerator = content.OutputFrameRate.Numerator = 60;
    content.InputFrameRate.Denominator = content.OutputFrameRate.Denominator = 1;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    hr = ID3D11VideoDevice_CreateVideoProcessorEnumerator(video_device,
                                                          &content, &enumerator);
    if (SUCCEEDED(hr))
        hr = ID3D11VideoDevice_CreateVideoProcessor(video_device, enumerator, 0,
                                                    &processor);
    if (FAILED(hr)) goto done;

    ZeroMemory(&texture_desc, sizeof(texture_desc));
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_DECODER;
    texture_desc.Format = DXGI_FORMAT_AYUV;
    if (decoded) {
        input_texture = decoded;
        ID3D11Texture2D_AddRef(input_texture);
        hr = S_OK;
    } else hr = ID3D11Device_CreateTexture2D(device, &texture_desc, NULL,
                                             &input_texture);
    if (FAILED(hr)) goto done;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    hr = ID3D11Device_CreateTexture2D(device, &texture_desc, NULL,
                                      &output_texture);
    if (FAILED(hr)) goto done;

    ZeroMemory(&input_desc, sizeof(input_desc));
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    hr = ID3D11VideoDevice_CreateVideoProcessorInputView(
        video_device, (ID3D11Resource *)input_texture, enumerator,
        &input_desc, &input_view);
    if (FAILED(hr)) goto done;
    ZeroMemory(&output_desc, sizeof(output_desc));
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    hr = ID3D11VideoDevice_CreateVideoProcessorOutputView(
        video_device, (ID3D11Resource *)output_texture, enumerator,
        &output_desc, &output_view);
    if (FAILED(hr)) goto done;

    ZeroMemory(&stream, sizeof(stream));
    stream.Enable = TRUE;
    stream.pInputSurface = input_view;
    hr = ID3D11VideoContext_VideoProcessorBlt(video_context, processor,
                                              output_view, 0, 1, &stream);
    ok = SUCCEEDED(hr);

done:
    if (output_view) ID3D11VideoProcessorOutputView_Release(output_view);
    if (input_view) ID3D11VideoProcessorInputView_Release(input_view);
    if (output_texture) ID3D11Texture2D_Release(output_texture);
    if (input_texture) ID3D11Texture2D_Release(input_texture);
    if (processor) ID3D11VideoProcessor_Release(processor);
    if (enumerator) ID3D11VideoProcessorEnumerator_Release(enumerator);
    if (video_context) ID3D11VideoContext_Release(video_context);
    if (device_context) ID3D11DeviceContext_Release(device_context);
    if (video_device) ID3D11VideoDevice_Release(video_device);
    return ok;
}

BOOL vm_video_decode_probe_ayuv_surface(ID3D11Device *device,
                                       ID3D11Texture2D *texture)
{
    D3D11_TEXTURE2D_DESC desc;
    if (!texture) return FALSE;
    ID3D11Texture2D_GetDesc(texture, &desc);
    return (desc.Format == DXGI_FORMAT_AYUV || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) &&
        probe_ayuv_video_processor(device, desc.Width, desc.Height, texture);
}

static HRESULT make_probe_input_sample(const BYTE *data, UINT size,
                                       IMFSample **sample)
{
    IMFSample *created = NULL;
    IMFMediaBuffer *buffer = NULL;
    BYTE *dst = NULL;
    HRESULT hr;
    if (!data || !size || !sample) return E_INVALIDARG;
    *sample = NULL;
    hr = MFCreateMemoryBuffer(size, &buffer);
    if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(buffer, &dst, NULL, NULL);
    if (SUCCEEDED(hr)) {
        memcpy(dst, data, size);
        IMFMediaBuffer_Unlock(buffer);
        hr = IMFMediaBuffer_SetCurrentLength(buffer, size);
    }
    if (SUCCEEDED(hr)) hr = MFCreateSample(&created);
    if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(created, buffer);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleTime(created, 0);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleDuration(created, 10000000LL / 60);
    if (buffer) IMFMediaBuffer_Release(buffer);
    if (FAILED(hr)) {
        if (created) IMFSample_Release(created);
        return hr;
    }
    *sample = created;
    return S_OK;
}

static HRESULT make_probe_output_sample(ID3D11Device *device, UINT width,
                                        UINT height, IMFSample **sample)
{
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D *texture = NULL;
    IMFMediaBuffer *buffer = NULL;
    IMFSample *created = NULL;
    HRESULT hr;
    if (!device || !sample) return E_INVALIDARG;
    *sample = NULL;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = width; desc.Height = height;
    desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_AYUV;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DECODER;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture);
    if (SUCCEEDED(hr)) hr = MFCreateDXGISurfaceBuffer(
        &IID_ID3D11Texture2D, (IUnknown *)texture, 0, FALSE, &buffer);
    if (SUCCEEDED(hr)) hr = MFCreateSample(&created);
    if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(created, buffer);
    if (buffer) IMFMediaBuffer_Release(buffer);
    if (texture) ID3D11Texture2D_Release(texture);
    if (FAILED(hr)) {
        if (created) IMFSample_Release(created);
        return hr;
    }
    *sample = created;
    return S_OK;
}

static BOOL probe_one_decoder(ID3D11Device *device, IMFActivate *activate,
                              UINT decoder_index, UINT width, UINT height,
                              UINT fps_num, UINT fps_den,
                              const BYTE *extradata, UINT extradata_size,
                              const BYTE *access_unit, UINT access_unit_size,
                              VmVideoDecodeProfile profile,
                              VmVideoDecodeCapability *out)
{
    IMFTransform *transform = NULL;
    IMFDXGIDeviceManager *manager = NULL;
    IMFMediaType *input = NULL, *output_type = NULL, *candidate = NULL;
    IMFSample *input_sample = NULL, *output_sample = NULL;
    IMFMediaBuffer *buffer = NULL;
    IMFDXGIBuffer *dxgi = NULL;
    ID3D11Texture2D *texture = NULL;
    MFT_OUTPUT_DATA_BUFFER output;
    MFT_OUTPUT_STREAM_INFO stream_info;
    UINT token = 0, subresource = 0, i;
    DWORD status = 0;
    GUID subtype;
    HRESULT hr;
    BOOL ok = FALSE;

    capture_decoder_identity(activate, out);

    hr = IMFActivate_ActivateObject(activate, &IID_IMFTransform,
                                    (void **)&transform);
    if (FAILED(hr)) goto done;
    hr = MFCreateDXGIDeviceManager(&token, &manager);
    if (SUCCEEDED(hr)) hr = IMFDXGIDeviceManager_ResetDevice(
        manager, (IUnknown *)device, token);
    if (SUCCEEDED(hr)) hr = IMFTransform_ProcessMessage(
        transform, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)manager);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&input);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(input, &MF_MT_MAJOR_TYPE,
                                                   &MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(input, &MF_MT_SUBTYPE,
                                                   &MFVideoFormat_HEVC);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT64(input, &MF_MT_FRAME_SIZE,
                                                   ((UINT64)width << 32) | height);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT64(input, &MF_MT_FRAME_RATE,
                                                   ((UINT64)fps_num << 32) | fps_den);
    if (SUCCEEDED(hr) && extradata_size) hr = IMFMediaType_SetBlob(
        input, &MF_MT_MPEG_SEQUENCE_HEADER, extradata, extradata_size);
    if (SUCCEEDED(hr)) hr = IMFTransform_SetInputType(transform, 0, input, 0);
    if (FAILED(hr)) goto done;

    for (i = 0;; i++) {
        hr = IMFTransform_GetOutputAvailableType(transform, 0, i, &candidate);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) goto done;
        if (SUCCEEDED(IMFMediaType_GetGUID(candidate, &MF_MT_SUBTYPE, &subtype)) &&
            IsEqualGUID(&subtype, output_subtype(profile))) {
            output_type = candidate;
            candidate = NULL;
            break;
        }
        IMFMediaType_Release(candidate);
        candidate = NULL;
    }
    if (!output_type || FAILED(IMFTransform_SetOutputType(transform, 0,
                                                           output_type, 0)))
        goto done;
    if (FAILED(IMFTransform_ProcessMessage(transform,
                                           MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
        FAILED(IMFTransform_ProcessMessage(transform,
                                           MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)) ||
        FAILED(make_probe_input_sample(access_unit, access_unit_size,
                                       &input_sample)) ||
        FAILED(IMFTransform_ProcessInput(transform, 0, input_sample, 0)))
        goto done;
    ZeroMemory(&stream_info, sizeof(stream_info));
    if (FAILED(IMFTransform_GetOutputStreamInfo(transform, 0, &stream_info)))
        goto done;

    for (i = 0; i < 32; i++) {
        ZeroMemory(&output, sizeof(output));
        output.dwStreamID = 0;
        if (!(stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) &&
            FAILED(make_probe_output_sample(device, width, height,
                                             &output.pSample)))
            goto done;
        hr = IMFTransform_ProcessOutput(transform, 0, 1, &output, &status);
        output_sample = output.pSample;
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (output_sample) { IMFSample_Release(output_sample); output_sample = NULL; }
            if (output.pEvents) { IMFCollection_Release(output.pEvents); output.pEvents = NULL; }
            IMFTransform_ProcessMessage(transform, MFT_MESSAGE_COMMAND_DRAIN, 0);
            continue;
        }
        if (output.pEvents) { IMFCollection_Release(output.pEvents); output.pEvents = NULL; }
        if (FAILED(hr) || !output_sample) goto done;
        hr = IMFSample_GetBufferByIndex(output_sample, 0, &buffer);
        if (SUCCEEDED(hr)) hr = IMFMediaBuffer_QueryInterface(buffer,
            &IID_IMFDXGIBuffer, (void **)&dxgi);
        if (SUCCEEDED(hr)) hr = IMFDXGIBuffer_GetResource(dxgi,
            &IID_ID3D11Texture2D, (void **)&texture);
        if (SUCCEEDED(hr)) hr = IMFDXGIBuffer_GetSubresourceIndex(dxgi,
                                                                    &subresource);
        if (SUCCEEDED(hr)) {
            D3D11_TEXTURE2D_DESC desc;
            ID3D11Texture2D_GetDesc(texture, &desc);
            out->actual_decode = TRUE;
            out->gpu_surface = TRUE;
            out->decoded_format = desc.Format;
            out->presentation_format = desc.Format;
            if (desc.Format == (profile == VM_VIDEO_HEVC444
                                ? DXGI_FORMAT_AYUV : DXGI_FORMAT_NV12)) {
                out->ayuv_video_processor = profile == VM_VIDEO_HEVC444
                    ? probe_ayuv_video_processor(device, width, height, texture) : TRUE;
                out->available = out->decoder_identity_valid &&
                    out->ayuv_video_processor;
                out->decoder_index = decoder_index;
                out->backend = VM_VIDEO_DECODE_BACKEND_MF_D3D11;
                out->device_identity = device;
                if (!device_adapter_luid(device, &out->adapter_luid)) out->available = FALSE;
                ok = out->available;
            }
        }
        break;
    }

done:
    if (texture) ID3D11Texture2D_Release(texture);
    if (dxgi) IMFDXGIBuffer_Release(dxgi);
    if (buffer) IMFMediaBuffer_Release(buffer);
    if (output_sample) IMFSample_Release(output_sample);
    if (input_sample) IMFSample_Release(input_sample);
    if (candidate) IMFMediaType_Release(candidate);
    if (output_type) IMFMediaType_Release(output_type);
    if (input) IMFMediaType_Release(input);
    if (transform) IMFTransform_Release(transform);
    if (manager) IMFDXGIDeviceManager_Release(manager);
    return ok;
}

static HRESULT activate_hevc_decoder(VmVideoDecodeProfile profile,
                                     IMFTransform **result)
{
    MFT_REGISTER_TYPE_INFO input = { MFMediaType_Video, MFVideoFormat_HEVC };
    MFT_REGISTER_TYPE_INFO output = { MFMediaType_Video, MFVideoFormat_NV12 };
    IMFActivate **activates = NULL;
    UINT32 count = 0, i;
    output.guidSubtype = *output_subtype(profile);
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input, &output, &activates, &count);
    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        return FAILED(hr) ? hr : MF_E_TOPO_CODEC_NOT_FOUND;
    }
    hr = MF_E_TOPO_CODEC_NOT_FOUND;
    /* Some systems enumerate more than one hardware MFT. Try every one; a
       registration hit is not proof that the transform exposes GPU-backed
       output in the requested profile. */
    for (i = 0; i < count; i++) {
        IMFTransform *candidate = NULL;
        HRESULT candidate_hr = IMFActivate_ActivateObject(
            activates[i], &IID_IMFTransform, (void **)&candidate);
        if (SUCCEEDED(candidate_hr)) {
            *result = candidate;
            hr = S_OK;
            break;
        }
    }
    for (i = 0; i < count; i++) IMFActivate_Release(activates[i]);
    CoTaskMemFree(activates);
    return hr;
}

/* Configure every candidate far enough to prove that it accepts this stream
   and exposes the requested output type. The actual ProcessInput/Output gate
   remains vm_video_decode_probe_profile; this helper prevents production
   decoder creation from stopping at the first merely activatable MFT. */
static HRESULT activate_configured_decoder(ID3D11Device *device,
                                           IMFDXGIDeviceManager *manager,
                                           UINT width, UINT height,
                                           UINT fps_num, UINT fps_den,
                                           const BYTE *extradata,
                                           UINT extradata_size,
                                           VmVideoDecodeProfile profile,
                                           const VmVideoDecodeCapability *capability,
                                           IMFTransform **result)
{
    MFT_REGISTER_TYPE_INFO mft_input = { MFMediaType_Video, MFVideoFormat_HEVC };
    MFT_REGISTER_TYPE_INFO mft_output = { MFMediaType_Video, MFVideoFormat_NV12 };
    IMFActivate **activates = NULL;
    UINT32 count = 0, i;
    HRESULT hr;
    if (!device || !manager || !result) return E_INVALIDARG;
    *result = NULL;
    mft_output.guidSubtype = *output_subtype(profile);
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                   &mft_input, &mft_output, &activates, &count);
    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        return FAILED(hr) ? hr : MF_E_TOPO_CODEC_NOT_FOUND;
    }
    hr = MF_E_TOPO_CODEC_NOT_FOUND;
    for (i = 0; i < count; i++) {
        IMFTransform *candidate = NULL;
        IMFMediaType *input = NULL, *output = NULL, *available = NULL;
        GUID subtype;
        UINT type_index;
        if (capability && !capability->decoder_identity_valid) continue;
        if (capability) {
            GUID clsid = GUID_NULL;
            WCHAR *name = NULL;
            UINT32 name_length = 0;
            HRESULT identity_hr = IMFActivate_GetGUID(
                activates[i], &MFT_TRANSFORM_CLSID_Attribute, &clsid);
            if (!IsEqualGUID(&capability->decoder_clsid, &GUID_NULL)) {
                if (FAILED(identity_hr) ||
                    !IsEqualGUID(&clsid, &capability->decoder_clsid))
                    continue;
            } else {
                identity_hr = IMFActivate_GetAllocatedString(
                    activates[i], &MFT_FRIENDLY_NAME_Attribute, &name,
                    &name_length);
                if (FAILED(identity_hr) || !name ||
                    wcscmp(name, capability->decoder_name) != 0) {
                    if (name) CoTaskMemFree(name);
                    continue;
                }
            }
            if (name) CoTaskMemFree(name);
        }
        HRESULT candidate_hr = IMFActivate_ActivateObject(
            activates[i], &IID_IMFTransform, (void **)&candidate);
        if (FAILED(candidate_hr)) continue;
        candidate_hr = IMFTransform_ProcessMessage(candidate,
            MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)manager);
        if (SUCCEEDED(candidate_hr)) candidate_hr = MFCreateMediaType(&input);
        if (SUCCEEDED(candidate_hr)) candidate_hr = IMFMediaType_SetGUID(
            input, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        if (SUCCEEDED(candidate_hr)) candidate_hr = IMFMediaType_SetGUID(
            input, &MF_MT_SUBTYPE, &MFVideoFormat_HEVC);
        if (SUCCEEDED(candidate_hr)) candidate_hr = IMFMediaType_SetUINT64(
            input, &MF_MT_FRAME_SIZE, ((UINT64)width << 32) | height);
        if (SUCCEEDED(candidate_hr)) candidate_hr = IMFMediaType_SetUINT64(
            input, &MF_MT_FRAME_RATE, ((UINT64)fps_num << 32) | fps_den);
        if (SUCCEEDED(candidate_hr) && extradata_size) candidate_hr =
            IMFMediaType_SetBlob(input, &MF_MT_MPEG_SEQUENCE_HEADER,
                                 extradata, extradata_size);
        if (SUCCEEDED(candidate_hr)) candidate_hr = IMFTransform_SetInputType(
            candidate, 0, input, 0);
        for (type_index = 0; SUCCEEDED(candidate_hr); type_index++) {
            candidate_hr = IMFTransform_GetOutputAvailableType(candidate, 0,
                                                               type_index,
                                                               &available);
            if (candidate_hr == MF_E_NO_MORE_TYPES) {
                candidate_hr = MF_E_INVALIDMEDIATYPE;
                break;
            }
            if (FAILED(candidate_hr)) break;
            if (SUCCEEDED(IMFMediaType_GetGUID(available, &MF_MT_SUBTYPE,
                                               &subtype)) &&
                IsEqualGUID(&subtype, output_subtype(profile))) {
                output = available;
                available = NULL;
                candidate_hr = IMFTransform_SetOutputType(candidate, 0,
                                                          output, 0);
                break;
            }
            IMFMediaType_Release(available);
            available = NULL;
        }
        if (available) IMFMediaType_Release(available);
        if (input) IMFMediaType_Release(input);
        if (output) IMFMediaType_Release(output);
        if (SUCCEEDED(candidate_hr)) {
            *result = candidate;
            hr = S_OK;
            break;
        }
        IMFTransform_Release(candidate);
    }
    for (i = 0; i < count; i++) IMFActivate_Release(activates[i]);
    CoTaskMemFree(activates);
    return hr;
}

BOOL vm_video_decode_supported(ID3D11Device *device)
{
    return vm_video_decode_supported_profile(device, VM_VIDEO_HEVC420);
}

BOOL vm_video_decode_probe_profile(ID3D11Device *device,
                                   UINT width, UINT height,
                                   UINT fps_num, UINT fps_den,
                                   const BYTE *extradata, UINT extradata_size,
                                   const BYTE *access_unit, UINT access_unit_size,
                                   VmVideoDecodeProfile profile,
                                   VmVideoDecodeCapability *out)
{
    MFT_REGISTER_TYPE_INFO input = { MFMediaType_Video, MFVideoFormat_HEVC };
    IMFActivate **activates = NULL;
    UINT count = 0, i;
    HRESULT hr;
    BOOL found = FALSE;

    if (!out) return FALSE;
    ZeroMemory(out, sizeof(*out));
    out->decoded_format = DXGI_FORMAT_UNKNOWN;
    if (!device || !width || !height || !fps_num || !fps_den ||
        !access_unit || !access_unit_size)
        return FALSE;

    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) return FALSE;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                   &input, NULL, &activates, &count);
    if (SUCCEEDED(hr)) {
        for (i = 0; i < count; i++) {
            VmVideoDecodeCapability candidate;
            ZeroMemory(&candidate, sizeof(candidate));
            candidate.decoded_format = DXGI_FORMAT_UNKNOWN;
            if (probe_one_decoder(device, activates[i], i, width, height,
                                  fps_num, fps_den, extradata, extradata_size,
                                  access_unit, access_unit_size, profile,
                                  &candidate) && candidate.available && !found) {
                *out = candidate;
                found = TRUE;
            }
        }
    }
    if (activates) {
        for (i = 0; i < count; i++) IMFActivate_Release(activates[i]);
        CoTaskMemFree(activates);
    }
    MFShutdown();
    return found;
}

BOOL vm_video_decode_probe_builtin_hevc444(ID3D11Device *device,
                                           VmVideoDecodeCapability *out)
{
    BOOL mf_ok = vm_video_decode_probe_profile(
        device, ASB_HEVC444_PROBE_WIDTH, ASB_HEVC444_PROBE_HEIGHT,
        ASB_HEVC444_PROBE_FPS_NUM, ASB_HEVC444_PROBE_FPS_DEN,
        asb_hevc444_probe_sample, ASB_HEVC444_PROBE_EXTRADATA_SIZE,
        asb_hevc444_probe_sample + ASB_HEVC444_PROBE_ACCESS_UNIT_OFFSET,
        ASB_HEVC444_PROBE_ACCESS_UNIT_SIZE, VM_VIDEO_HEVC444, out);
    BOOL d3d12_ok = FALSE;
    if (!mf_ok) d3d12_ok = vm_d3d12_probe(device, out);
    return vm_video_decode_select_backend(mf_ok, d3d12_ok) != VM_VIDEO_DECODE_BACKEND_NONE;
}

BOOL vm_video_decode_supported_profile(ID3D11Device *device,
                                       VmVideoDecodeProfile profile)
{
    IMFTransform *transform = NULL;
    IMFDXGIDeviceManager *manager = NULL;
    UINT token = 0;
    HRESULT hr;
    if (!device) return FALSE;
    /* HostHello is sent before the guest ASVC supplies a real access unit.
       Do not turn registration/activation into a false HEVC444 capability. A
       caller with a real production access unit must use the selector above. */
    if (profile == VM_VIDEO_HEVC444)
        return FALSE;
    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) return FALSE;
    hr = MFCreateDXGIDeviceManager(&token, &manager);
    if (SUCCEEDED(hr)) hr = IMFDXGIDeviceManager_ResetDevice(manager, (IUnknown *)device, token);
    if (SUCCEEDED(hr)) hr = activate_hevc_decoder(profile, &transform);
    if (SUCCEEDED(hr))
        hr = IMFTransform_ProcessMessage(transform, MFT_MESSAGE_SET_D3D_MANAGER,
                                          (ULONG_PTR)manager);
    if (transform) IMFTransform_Release(transform);
    if (manager) IMFDXGIDeviceManager_Release(manager);
    MFShutdown();
    return SUCCEEDED(hr);
}

VmVideoDecoder *vm_video_decoder_create_with_capability(
                                        ID3D11Device *device,
                                        UINT width, UINT height,
                                        UINT fps_num, UINT fps_den,
                                        const BYTE *extradata,
                                        UINT extradata_size,
                                        VmVideoDecodeProfile profile,
                                        const VmVideoDecodeCapability *capability)
{
    VmVideoDecoder *d = NULL;
    HRESULT hr;
    LUID luid;
    if (!device || !width || !height || !fps_num || !fps_den) return NULL;
    if (profile == VM_VIDEO_HEVC444 &&
        (!capability || !capability->available || !capability->actual_decode ||
         !capability->gpu_surface || !capability->ayuv_video_processor ||
         capability->decoded_format != DXGI_FORMAT_AYUV ||
         capability->device_identity != device || !capability->decoder_identity_valid))
        return NULL;
    if (profile == VM_VIDEO_HEVC444 &&
        (!device_adapter_luid(device, &luid) ||
         luid.LowPart != capability->adapter_luid.LowPart ||
         luid.HighPart != capability->adapter_luid.HighPart)) return NULL;
    if (profile == VM_VIDEO_HEVC444 && capability->backend == VM_VIDEO_DECODE_BACKEND_D3D12) {
        if (capability->presentation_format != DXGI_FORMAT_B8G8R8A8_UNORM) return NULL;
        d = (VmVideoDecoder *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
        if (!d) return NULL;
        d->d3d12 = vm_d3d12_create(device, width, height, fps_num, fps_den, extradata, extradata_size);
        if (!d->d3d12) { HeapFree(GetProcessHeap(), 0, d); return NULL; }
        return d;
    }
    if (profile == VM_VIDEO_HEVC444 && capability->backend != VM_VIDEO_DECODE_BACKEND_MF_D3D11)
        return NULL;
    if (profile == VM_VIDEO_HEVC444 && capability->presentation_format != DXGI_FORMAT_AYUV)
        return NULL;
    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) return NULL;
    d = (VmVideoDecoder *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d) { MFShutdown(); return NULL; }
    d->mf_started = TRUE;
    d->width = width; d->height = height;
    d->fps_num = fps_num; d->fps_den = fps_den;
    d->profile = profile;
    d->device = device; ID3D11Device_AddRef(device);

    hr = MFCreateDXGIDeviceManager(&d->reset_token, &d->manager);
    if (SUCCEEDED(hr))
        hr = IMFDXGIDeviceManager_ResetDevice(d->manager, (IUnknown *)device, d->reset_token);
    if (SUCCEEDED(hr)) hr = activate_configured_decoder(
        device, d->manager, width, height, fps_num, fps_den,
        extradata, extradata_size, profile, capability, &d->transform);
    if (SUCCEEDED(hr)) hr = IMFTransform_ProcessMessage(d->transform,
                                      MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(hr)) hr = IMFTransform_ProcessMessage(d->transform,
                                      MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (SUCCEEDED(hr)) {
        MFT_OUTPUT_STREAM_INFO info;
        ZeroMemory(&info, sizeof(info));
        hr = IMFTransform_GetOutputStreamInfo(d->transform, 0, &info);
        if (SUCCEEDED(hr))
            d->provides_samples =
                (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                 MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    }

    if (FAILED(hr)) { vm_video_decoder_destroy(d); return NULL; }
    return d;
}

VmVideoDecoder *vm_video_decoder_create(ID3D11Device *device,
                                        UINT width, UINT height,
                                        UINT fps_num, UINT fps_den,
                                        const BYTE *extradata,
                                        UINT extradata_size,
                                        VmVideoDecodeProfile profile)
{
    return vm_video_decoder_create_with_capability(
        device, width, height, fps_num, fps_den, extradata, extradata_size,
        profile, NULL);
}

HRESULT vm_video_decoder_decode(VmVideoDecoder *d,
                                const BYTE *data, UINT size,
                                LONGLONG capture_time_100ns,
                                ID3D11Texture2D **texture,
                                UINT *subresource)
{
    IMFSample *input_sample = NULL, *output_sample = NULL;
    IMFMediaBuffer *buffer = NULL;
    IMFDXGIBuffer *dxgi_buffer = NULL;
    BYTE *dst = NULL;
    MFT_OUTPUT_DATA_BUFFER output = {0};
    DWORD status = 0;
    HRESULT hr;
    if (!texture || !subresource) return E_POINTER;
    *texture = NULL; *subresource = 0;
    if (!d || !data || !size) return E_INVALIDARG;
    if (d->d3d12) return vm_d3d12_decode(d->d3d12, data, size, texture);

    hr = MFCreateSample(&input_sample);
    if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(size, &buffer);
    if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(buffer, &dst, NULL, NULL);
    if (SUCCEEDED(hr)) { memcpy(dst, data, size); IMFMediaBuffer_Unlock(buffer); }
    if (SUCCEEDED(hr)) hr = IMFMediaBuffer_SetCurrentLength(buffer, size);
    if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(input_sample, buffer);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleTime(input_sample,
                              capture_time_100ns ? capture_time_100ns : d->next_time);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleDuration(input_sample,
                              10000000LL * d->fps_den / d->fps_num);
    if (SUCCEEDED(hr)) hr = IMFTransform_ProcessInput(d->transform, 0, input_sample, 0);
    d->next_time += 10000000LL * d->fps_den / d->fps_num;
    if (buffer) IMFMediaBuffer_Release(buffer);
    if (input_sample) IMFSample_Release(input_sample);
    if (FAILED(hr)) return hr;

    output.dwStreamID = d->output_stream;
    if (!d->provides_samples) {
        D3D11_TEXTURE2D_DESC desc;
        ID3D11Texture2D *surface = NULL;
        ZeroMemory(&desc, sizeof(desc));
        desc.Width = d->width;
        desc.Height = d->height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = d->profile == VM_VIDEO_HEVC444 ? DXGI_FORMAT_AYUV : DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
        hr = ID3D11Device_CreateTexture2D(d->device, &desc, NULL, &surface);
        if (SUCCEEDED(hr)) hr = MFCreateSample(&output.pSample);
        if (SUCCEEDED(hr)) hr = MFCreateDXGISurfaceBuffer(
            &IID_ID3D11Texture2D, (IUnknown *)surface, 0, FALSE, &buffer);
        if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(output.pSample, buffer);
        if (buffer) { IMFMediaBuffer_Release(buffer); buffer = NULL; }
        if (surface) ID3D11Texture2D_Release(surface);
        if (FAILED(hr)) {
            if (output.pSample) IMFSample_Release(output.pSample);
            return hr;
        }
    }
    hr = IMFTransform_ProcessOutput(d->transform, 0, 1, &output, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (output.pSample) IMFSample_Release(output.pSample);
        return S_FALSE;
    }
    if (FAILED(hr)) {
        if (output.pSample) IMFSample_Release(output.pSample);
        return hr;
    }
    output_sample = output.pSample;
    if (!output_sample) return MF_E_TRANSFORM_STREAM_CHANGE;
    hr = IMFSample_GetBufferByIndex(output_sample, 0, &buffer);
    if (SUCCEEDED(hr)) hr = IMFMediaBuffer_QueryInterface(buffer,
                                      &IID_IMFDXGIBuffer, (void **)&dxgi_buffer);
    if (SUCCEEDED(hr)) hr = IMFDXGIBuffer_GetResource(dxgi_buffer,
                                      &IID_ID3D11Texture2D, (void **)texture);
    if (SUCCEEDED(hr) && *texture) {
        D3D11_TEXTURE2D_DESC decoded_desc;
        ID3D11Texture2D_GetDesc(*texture, &decoded_desc);
        if (decoded_desc.Format != (d->profile == VM_VIDEO_HEVC444
                                    ? DXGI_FORMAT_AYUV : DXGI_FORMAT_NV12)) {
            ID3D11Texture2D_Release(*texture);
            *texture = NULL;
            hr = MF_E_INVALIDMEDIATYPE;
        }
    }
    if (SUCCEEDED(hr)) hr = IMFDXGIBuffer_GetSubresourceIndex(dxgi_buffer, subresource);
    if (dxgi_buffer) IMFDXGIBuffer_Release(dxgi_buffer);
    if (buffer) IMFMediaBuffer_Release(buffer);
    IMFSample_Release(output_sample);
    if (output.pEvents) IMFCollection_Release(output.pEvents);
    return hr;
}

void vm_video_decoder_destroy(VmVideoDecoder *d)
{
    if (!d) return;
    if (d->d3d12) vm_d3d12_destroy(d->d3d12);
    if (d->transform) {
        IMFTransform_ProcessMessage(d->transform, MFT_MESSAGE_COMMAND_FLUSH, 0);
        IMFTransform_ProcessMessage(d->transform, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFTransform_Release(d->transform);
    }
    if (d->manager) IMFDXGIDeviceManager_Release(d->manager);
    if (d->device) ID3D11Device_Release(d->device);
    if (d->mf_started) MFShutdown();
    HeapFree(GetProcessHeap(), 0, d);
}
