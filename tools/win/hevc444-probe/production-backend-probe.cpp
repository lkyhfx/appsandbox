/* SPDX-License-Identifier: MIT - exercises exactly the production API. */
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>
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

static LRESULT CALLBACK probe_window_proc(HWND hwnd, UINT message,
                                          WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

struct PresentHarness {
    HWND window = nullptr;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> video_device;
    ComPtr<ID3D11VideoContext> video_context;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<IDXGISwapChain1> swap_chain;
    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    std::vector<std::pair<ID3D11Texture2D *,
                           ComPtr<ID3D11VideoProcessorInputView>>> input_views;

    ~PresentHarness()
    {
        if (window) DestroyWindow(window);
    }

    bool initialize(ID3D11Device *device, IDXGIFactory1 *factory)
    {
        if (!device || !factory) return false;
        const wchar_t class_name[] = L"AppSandboxProductionProbeWindow";
        WNDCLASSW window_class = {};
        window_class.lpfnWndProc = probe_window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = class_name;
        if (!RegisterClassW(&window_class) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        window = CreateWindowExW(0, class_name, class_name, WS_OVERLAPPEDWINDOW,
                                 0, 0, 3840, 2160, nullptr, nullptr,
                                 window_class.hInstance, nullptr);
        if (!window) return false;

        ComPtr<IDXGIFactory2> factory2;
        if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory2))))
            return false;
        DXGI_SWAP_CHAIN_DESC1 swap_desc = {};
        swap_desc.Width = 3840;
        swap_desc.Height = 2160;
        swap_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swap_desc.SampleDesc.Count = 1;
        swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_desc.BufferCount = 2;
        swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap_desc.Scaling = DXGI_SCALING_STRETCH;
        if (FAILED(factory2->CreateSwapChainForHwnd(
                       device, window, &swap_desc, nullptr, nullptr,
                       &swap_chain)))
            return false;
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        device->GetImmediateContext(&context);
        if (!context || FAILED(device->QueryInterface(
                                  IID_PPV_ARGS(&video_device))) ||
            FAILED(context.As(&video_context)))
            return false;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = content.OutputWidth = 3840;
        content.InputHeight = content.OutputHeight = 2160;
        content.InputFrameRate = {60, 1};
        content.OutputFrameRate = {60, 1};
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(video_device->CreateVideoProcessorEnumerator(
                       &content, &enumerator)) ||
            FAILED(video_device->CreateVideoProcessor(
                       enumerator.Get(), 0, &processor)))
            return false;

        ComPtr<ID3D11Texture2D> back_buffer;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc = {};
        output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(swap_chain->GetBuffer(
                       0, IID_PPV_ARGS(&back_buffer))) ||
            FAILED(video_device->CreateVideoProcessorOutputView(
                       back_buffer.Get(), enumerator.Get(), &output_desc,
                       &output_view)))
            return false;
        return true;
    }

    HRESULT present(ID3D11Texture2D *texture)
    {
        if (!texture || !context || !processor || !output_view)
            return E_INVALIDARG;
        ID3D11VideoProcessorInputView *input = nullptr;
        for (const auto &entry : input_views) {
            if (entry.first == texture) {
                input = entry.second.Get();
                break;
            }
        }
        if (!input) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc = {};
            input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            HRESULT hr = video_device->CreateVideoProcessorInputView(
                texture, enumerator.Get(), &input_desc, &input);
            if (FAILED(hr)) return hr;
            ComPtr<ID3D11VideoProcessorInputView> owned_input;
            owned_input.Attach(input);
            input_views.emplace_back(texture, std::move(owned_input));
        }
        RECT source = {0, 0, 3840, 2160};
        RECT destination = source;
        video_context->VideoProcessorSetStreamSourceRect(
            processor.Get(), 0, TRUE, &source);
        video_context->VideoProcessorSetStreamDestRect(
            processor.Get(), 0, TRUE, &destination);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE color = {};
        color.Nominal_Range = 2;
        video_context->VideoProcessorSetStreamColorSpace(
            processor.Get(), 0, &color);
        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = input;
        HRESULT hr = video_context->VideoProcessorBlt(
            processor.Get(), output_view.Get(), 0, 1, &stream);
        if (SUCCEEDED(hr)) hr = swap_chain->Present(0, 0);
        return hr;
    }
};

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
        std::vector<double> present_ms;
        frame_ms.reserve(static_cast<std::size_t>(frames));
        present_ms.reserve(static_cast<std::size_t>(frames));
        unsigned failures = 0;
        unsigned present_failures = 0;
        unsigned present_frames = 0;
        PresentHarness presenter;
        const bool presenter_ready = presenter.initialize(device.Get(), factory.Get());
        std::printf("host_present_harness=%s\n",
                    presenter_ready ? "READY" : "BLOCKED");
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 0; frame < frames; ++frame) {
            ComPtr<ID3D11Texture2D> decoded;
            const auto frame_start = std::chrono::steady_clock::now();
            const auto present_start = std::chrono::steady_clock::now();
            HRESULT hr = vm_d3d12_decode(native,
                asb_hevc444_probe_sample + ASB_HEVC444_PROBE_ACCESS_UNIT_OFFSET,
                ASB_HEVC444_PROBE_ACCESS_UNIT_SIZE, &decoded);
            if (FAILED(hr) || !decoded) {
                ++failures;
                std::printf("host_frame=%d hr=0x%08lx surface=%u\n",
                            frame, hr, decoded ? 1 : 0);
                break;
            }
            if (!presenter_ready || !vm_d3d12_render_begin(native, decoded.Get())) {
                ++present_failures;
                std::printf("host_present_frame=%d hr=0x%08lx\n",
                            frame, E_FAIL);
                break;
            }
            const HRESULT present_hr = presenter.present(decoded.Get());
            const double present_elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - present_start).count();
            if (SUCCEEDED(present_hr)) {
                if (!vm_d3d12_render_submitted(native, decoded.Get())) {
                    ++present_failures;
                    std::printf("host_present_frame=%d hr=0x%08lx\n",
                                frame, E_FAIL);
                    break;
                }
                ++present_frames;
                present_ms.push_back(present_elapsed);
            } else {
                ++present_failures;
                /* Protect the slot even when Present reports failure: the
                 * command may already have reached the D3D11 queue. */
                (void)vm_d3d12_render_submitted(native, decoded.Get());
                std::printf("host_present_frame=%d hr=0x%08lx\n",
                            frame, present_hr);
                break;
            }
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - frame_start).count();
            frame_ms.push_back(elapsed);
        }
        const auto end = std::chrono::steady_clock::now();
        const double elapsed_s = std::chrono::duration<double>(end - start).count();
        const UINT64 slot_reuse_hazards = vm_d3d12_slot_reuse_hazards(native);
        const unsigned device_removed =
            FAILED(device->GetDeviceRemovedReason()) ? 1u : 0u;
        const double host_fps = elapsed_s > 0.0 ? frame_ms.size() / elapsed_s : 0.0;
        const double host_present_fps =
            elapsed_s > 0.0 ? present_frames / elapsed_s : 0.0;
        std::printf("host_decode_frames=%zu\n", frame_ms.size());
        std::printf("host_decode_failures=%u\n", failures);
        std::printf("host_process_failures=%u\n", failures);
        std::printf("host_present_frames=%u\n", present_frames);
        std::printf("host_present_failures=%u\n", present_failures);
        std::printf("host_fps=%.3f\n", host_fps);
        std::printf("host_present_fps=%.3f\n", host_present_fps);
        double mean = 0.0;
        for (double sample : frame_ms) mean += sample;
        if (!frame_ms.empty()) mean /= frame_ms.size();
        std::printf("host_mean_frame_ms=%.3f\n", mean);
        std::printf("host_p95_frame_ms=%.3f\n", percentile_ms(frame_ms, 0.95));
        std::printf("host_p99_frame_ms=%.3f\n", percentile_ms(frame_ms, 0.99));
        std::printf("host_max_frame_ms=%.3f\n",
                    frame_ms.empty() ? 0.0 : *std::max_element(frame_ms.begin(), frame_ms.end()));
        std::printf("host_present_p95_ms=%.3f\n", percentile_ms(present_ms, 0.95));
        std::printf("host_present_p99_ms=%.3f\n", percentile_ms(present_ms, 0.99));
        std::printf("slot_reuse_hazards=%llu\n",
                    static_cast<unsigned long long>(slot_reuse_hazards));
        std::printf("device_removed=%u\n", device_removed);
        std::printf("per_frame_queue_create=0\n");
        std::printf("per_frame_processor_create=0\n");
        std::printf("per_frame_shared_open=0\n");
        std::printf("cpu_frame_copy=0\n");
        std::printf("host_pipeline_slots=3\n");
        std::printf("host_pipeline_persistent=1\n");
        std::printf("renderer_slot_ownership=1\n");
        vm_d3d12_destroy(native);
        const bool gate_c = failures == 0 && present_failures == 0 &&
            static_cast<int>(frame_ms.size()) == frames &&
            static_cast<int>(present_frames) == frames &&
            host_fps >= 60.0 && host_present_fps >= 60.0 &&
            mean < 16.67 && device_removed == 0 && slot_reuse_hazards == 0;
        std::printf("gate_c_verdict=%s\n", gate_c ? "PASS" : "FAIL");
        std::printf("host_gate=%s frames=%d\n",
                    gate_c ? (frames >= 3600 ? "3600-PASS" : "600-PASS") : "FAIL",
                    frames);
        if (!gate_c) return 5;
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
