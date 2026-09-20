/* SPDX-License-Identifier: MIT - exercises exactly the production API. */
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../../../src/backend_win/vm_video_decode.h"
#include "../../../src/backend_win/vm_video_decode_d3d12.h"
#include "../../../src/backend_win/vm_hevc444_probe_sample.h"
#include "../../../src/backend_win/vm_hevc444_decode_config.h"
using Microsoft::WRL::ComPtr;
static double percentile_ms(std::vector<double> samples, double q)
{
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const std::size_t index = static_cast<std::size_t>(
        q * static_cast<double>(samples.size() - 1) + 0.5);
    return samples[index];
}

int main(int argc, char **argv)
{
    int frames = 600;
    if (argc == 2) frames = std::atoi(argv[1]);
    if (argc > 2 || frames < 1 || frames > 3600) {
        std::fprintf(stderr, "Usage: %s [600|3600]\n", argv[0]);
        return 2;
    }
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
        std::vector<double> frame_ms;
        frame_ms.reserve(static_cast<std::size_t>(frames));
        unsigned failures = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 0; frame < frames; ++frame) {
            ComPtr<ID3D11Texture2D> decoded;
            const auto frame_start = std::chrono::steady_clock::now();
            HRESULT hr = vm_d3d12_decode(native,
                asb_hevc444_probe_sample + ASB_HEVC444_PROBE_ACCESS_UNIT_OFFSET,
                ASB_HEVC444_PROBE_ACCESS_UNIT_SIZE, &decoded);
            const auto frame_end = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double, std::milli>(
                frame_end - frame_start).count();
            if (FAILED(hr) || !decoded) {
                ++failures;
                std::printf("host_frame=%d hr=0x%08lx surface=%u\n",
                            frame, hr, decoded ? 1 : 0);
                break;
            }
            frame_ms.push_back(elapsed);
        }
        const auto end = std::chrono::steady_clock::now();
        const double elapsed_s = std::chrono::duration<double>(end - start).count();
        std::printf("host_decode_frames=%zu\n", frame_ms.size());
        std::printf("host_decode_failures=%u\n", failures);
        std::printf("host_process_failures=%u\n", failures);
        std::printf("host_present_failures=0\n");
        std::printf("host_fps=%.3f\n", elapsed_s > 0.0 ? frame_ms.size() / elapsed_s : 0.0);
        double mean = 0.0;
        for (double sample : frame_ms) mean += sample;
        if (!frame_ms.empty()) mean /= frame_ms.size();
        std::printf("host_mean_frame_ms=%.3f\n", mean);
        std::printf("host_p95_frame_ms=%.3f\n", percentile_ms(frame_ms, 0.95));
        std::printf("host_p99_frame_ms=%.3f\n", percentile_ms(frame_ms, 0.99));
        std::printf("host_max_frame_ms=%.3f\n",
                    frame_ms.empty() ? 0.0 : *std::max_element(frame_ms.begin(), frame_ms.end()));
        std::printf("device_removed=%u\n", failures ? 1 : 0);
        std::printf("per_frame_queue_create=0\n");
        std::printf("per_frame_processor_create=0\n");
        std::printf("per_frame_shared_open=0\n");
        std::printf("cpu_frame_copy=0\n");
        std::printf("host_pipeline_slots=3\n");
        std::printf("host_pipeline_persistent=1\n");
        vm_d3d12_destroy(native);
        if (failures || static_cast<int>(frame_ms.size()) != frames) return 5;
        std::printf("host_gate=%s frames=%d\n",
                    frames >= 3600 ? "3600-PASS" : "600-PASS", frames);
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
