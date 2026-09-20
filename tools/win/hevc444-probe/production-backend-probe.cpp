/* SPDX-License-Identifier: MIT - exercises exactly the production API. */
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include "../../../src/backend_win/vm_video_decode.h"
#include "../../../src/backend_win/vm_video_decode_d3d12.h"
#include "../../../src/backend_win/vm_hevc444_probe_sample.h"
#include "../../../src/backend_win/vm_hevc444_decode_config.h"
using Microsoft::WRL::ComPtr;
int main()
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 2;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D11CreateDevice(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr))) break;
        adapter.Reset();
    }
    if (!device) return 2;
    std::vector<std::uint8_t> compressed;
    std::printf("native_parse=%u\n", asb_hevc_decode::idr_slice(
        asb_hevc444_probe_sample + ASB_HEVC444_PROBE_ACCESS_UNIT_OFFSET,
        ASB_HEVC444_PROBE_ACCESS_UNIT_SIZE, &compressed));
    auto *native = vm_d3d12_create(device.Get(), 3840, 2160, 60, 1,
        asb_hevc444_probe_sample, ASB_HEVC444_PROBE_EXTRADATA_SIZE);
    std::printf("native_create=%u\n", native ? 1 : 0);
    if (native) {
        ComPtr<ID3D11Texture2D> decoded;
        HRESULT hr = vm_d3d12_decode(native,
            asb_hevc444_probe_sample + ASB_HEVC444_PROBE_ACCESS_UNIT_OFFSET,
            ASB_HEVC444_PROBE_ACCESS_UNIT_SIZE, &decoded);
        std::printf("native_decode_hr=0x%08lx surface=%u\n", hr, decoded ? 1 : 0);
        vm_d3d12_destroy(native);
    }
    VmVideoDecodeCapability cap = {};
    const BOOL ready = vm_video_decode_probe_builtin_hevc444(device.Get(), &cap);
    std::printf("production_probe=%s backend=%u actual=%u gpu=%u ayuv_vp=%u\n",
        ready ? "PASS" : "BLOCKED", cap.backend, cap.actual_decode,
        cap.gpu_surface, cap.ayuv_video_processor);
    if (!ready) return 3;
    auto *decoder = vm_video_decoder_create_with_capability(device.Get(), 3840, 2160, 60, 1,
        asb_hevc444_probe_sample, ASB_HEVC444_PROBE_EXTRADATA_SIZE, VM_VIDEO_HEVC444, &cap);
    if (!decoder) return 4;
    for (int frame = 0; frame < 3; ++frame) {
        ComPtr<ID3D11Texture2D> texture;
        UINT subresource;
        HRESULT hr = vm_video_decoder_decode(decoder,
            asb_hevc444_probe_sample + ASB_HEVC444_PROBE_ACCESS_UNIT_OFFSET,
            ASB_HEVC444_PROBE_ACCESS_UNIT_SIZE, frame * 166667LL, &texture, &subresource);
        std::printf("production_frame=%d hr=0x%08lx surface=%u\n", frame, hr, texture ? 1 : 0);
        if (FAILED(hr) || !texture) { vm_video_decoder_destroy(decoder); return 5; }
    }
    vm_video_decoder_destroy(decoder);
    auto invalid = cap;
    invalid.device_identity = nullptr;
    decoder = vm_video_decoder_create_with_capability(device.Get(), 3840, 2160, 60, 1,
        asb_hevc444_probe_sample, ASB_HEVC444_PROBE_EXTRADATA_SIZE, VM_VIDEO_HEVC444, &invalid);
    if (decoder) { vm_video_decoder_destroy(decoder); return 6; }
    std::puts("production_selector_invalid_identity=REJECTED");
    invalid = cap;
    invalid.adapter_luid.LowPart ^= 1;
    decoder = vm_video_decoder_create_with_capability(device.Get(), 3840, 2160, 60, 1,
        asb_hevc444_probe_sample, ASB_HEVC444_PROBE_EXTRADATA_SIZE, VM_VIDEO_HEVC444, &invalid);
    if (decoder) { vm_video_decoder_destroy(decoder); return 7; }
    invalid = cap;
    invalid.actual_decode = FALSE;
    decoder = vm_video_decoder_create_with_capability(device.Get(), 3840, 2160, 60, 1,
        asb_hevc444_probe_sample, ASB_HEVC444_PROBE_EXTRADATA_SIZE, VM_VIDEO_HEVC444, &invalid);
    if (decoder) { vm_video_decoder_destroy(decoder); return 8; }
    std::puts("production_capability_only=REJECTED");
    if (!vm_video_decode_probe_builtin_hevc444(device.Get(), &cap)) return 9;
    decoder = vm_video_decoder_create_with_capability(device.Get(), 3840, 2160, 60, 1,
        asb_hevc444_probe_sample, ASB_HEVC444_PROBE_EXTRADATA_SIZE, VM_VIDEO_HEVC444, &cap);
    if (!decoder) return 10;
    vm_video_decoder_destroy(decoder);
    std::puts("production_reprobe_create=PASS");
    return 0;
}
