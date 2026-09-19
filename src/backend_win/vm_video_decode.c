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

#include "vm_video_decode.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

struct VmVideoDecoder {
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

static const GUID *output_subtype(VmVideoDecodeProfile profile)
{
    return profile == VM_VIDEO_HEVC444 ? &MFVideoFormat_AYUV : &MFVideoFormat_NV12;
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

BOOL vm_video_decode_supported(ID3D11Device *device)
{
    return vm_video_decode_supported_profile(device, VM_VIDEO_HEVC420);
}

BOOL vm_video_decode_supported_profile(ID3D11Device *device,
                                       VmVideoDecodeProfile profile)
{
    IMFTransform *transform = NULL;
    IMFDXGIDeviceManager *manager = NULL;
    UINT token = 0;
    HRESULT hr;
    if (!device) return FALSE;
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

VmVideoDecoder *vm_video_decoder_create(ID3D11Device *device,
                                        UINT width, UINT height,
                                        UINT fps_num, UINT fps_den,
                                        const BYTE *extradata,
                                        UINT extradata_size,
                                        VmVideoDecodeProfile profile)
{
    VmVideoDecoder *d = NULL;
    IMFMediaType *input = NULL, *output = NULL, *candidate = NULL;
    DWORD i;
    GUID subtype;
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) return NULL;
    d = (VmVideoDecoder *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d) { MFShutdown(); return NULL; }
    d->width = width; d->height = height;
    d->fps_num = fps_num; d->fps_den = fps_den;
    d->profile = profile;
    d->device = device; ID3D11Device_AddRef(device);

    hr = MFCreateDXGIDeviceManager(&d->reset_token, &d->manager);
    if (SUCCEEDED(hr))
        hr = IMFDXGIDeviceManager_ResetDevice(d->manager, (IUnknown *)device, d->reset_token);
    if (SUCCEEDED(hr)) hr = activate_hevc_decoder(profile, &d->transform);
    if (SUCCEEDED(hr))
        hr = IMFTransform_ProcessMessage(d->transform, MFT_MESSAGE_SET_D3D_MANAGER,
                                         (ULONG_PTR)d->manager);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&input);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(input, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(input, &MF_MT_SUBTYPE, &MFVideoFormat_HEVC);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT64(input, &MF_MT_FRAME_SIZE,
                                                   ((UINT64)width << 32) | height);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT64(input, &MF_MT_FRAME_RATE,
                                                   ((UINT64)fps_num << 32) | fps_den);
    if (SUCCEEDED(hr) && extradata_size)
        hr = IMFMediaType_SetBlob(input, &MF_MT_MPEG_SEQUENCE_HEADER,
                                  extradata, extradata_size);
    if (SUCCEEDED(hr)) hr = IMFTransform_SetInputType(d->transform, 0, input, 0);

    for (i = 0; SUCCEEDED(hr); i++) {
        hr = IMFTransform_GetOutputAvailableType(d->transform, 0, i, &candidate);
        if (FAILED(hr)) break;
        if (SUCCEEDED(IMFMediaType_GetGUID(candidate, &MF_MT_SUBTYPE, &subtype)) &&
            IsEqualGUID(&subtype, output_subtype(profile))) {
            output = candidate; candidate = NULL;
            hr = S_OK;
            break;
        }
        IMFMediaType_Release(candidate); candidate = NULL;
    }
    if (SUCCEEDED(hr) && output)
        hr = IMFTransform_SetOutputType(d->transform, 0, output, 0);
    else if (SUCCEEDED(hr)) hr = MF_E_INVALIDMEDIATYPE;
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

    if (candidate) IMFMediaType_Release(candidate);
    if (output) IMFMediaType_Release(output);
    if (input) IMFMediaType_Release(input);
    if (FAILED(hr)) { vm_video_decoder_destroy(d); return NULL; }
    return d;
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
    *texture = NULL; *subresource = 0;
    if (!d || !data || !size) return E_INVALIDARG;

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
    if (d->transform) {
        IMFTransform_ProcessMessage(d->transform, MFT_MESSAGE_COMMAND_FLUSH, 0);
        IMFTransform_ProcessMessage(d->transform, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFTransform_Release(d->transform);
    }
    if (d->manager) IMFDXGIDeviceManager_Release(d->manager);
    if (d->device) ID3D11Device_Release(d->device);
    HeapFree(GetProcessHeap(), 0, d);
    MFShutdown();
}
