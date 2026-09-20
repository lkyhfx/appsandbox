/* SPDX-License-Identifier: MIT */
/*
 * Native Windows D3D12 HEVC Main444 capability probe.
 *
 * The public Windows SDK still omits a named Main444 encoder enum. The
 * DirectX runtime value is nevertheless stable and is intentionally kept
 * explicit here. A zero result means that this D3D12 encode path did not
 * expose Main444; it is not a statement about the underlying NVENC engine.
 */
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "../hevc444-probe/hevc_access_unit_probe.h"
#include "../../linux/agent/hevc444_probe_codec.h"
#include "d3d12video_hevc1_compat.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

using Microsoft::WRL::ComPtr;
using appsandbox_d3d12_hevc1::D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC1;
using appsandbox_d3d12_hevc1::D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1;

namespace {

constexpr UINT kWidth = 3840;
constexpr UINT kHeight = 2160;
constexpr UINT kFpsNumerator = 60;
constexpr UINT kFpsDenominator = 1;
/* D3D12's Main444 value is defined by the runtime even when the SDK header
   only names Main and Main10. Keep this isolated and report the raw value. */
constexpr D3D12_VIDEO_ENCODER_PROFILE_HEVC kMain444 =
    static_cast<D3D12_VIDEO_ENCODER_PROFILE_HEVC>(5);

static void print_hr(const char *stage, HRESULT hr)
{
    std::fprintf(stderr, "BLOCKED stage=%s HRESULT=0x%08lx\n", stage,
                 static_cast<unsigned long>(hr));
}

static bool create_video_device(ComPtr<ID3D12VideoDevice3> *video,
                                ComPtr<ID3D12Device> *device_out)
{
    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        print_hr("host-d3d12-factory", hr);
        return false;
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc)) ||
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            continue;
        ComPtr<ID3D12Device> device;
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                               IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) hr = device.As(video);
        if (SUCCEEDED(hr)) {
            *device_out = device;
            std::printf("selected_adapter=%ls\n", desc.Description);
            std::printf("selected_adapter_vendor_id=0x%04x\n", desc.VendorId);
            std::printf("selected_adapter_device_id=0x%04x\n", desc.DeviceId);
            return true;
        }
    }
    std::fputs("BLOCKED stage=host-d3d12-device reason=no-hardware-adapter\n",
               stderr);
    return false;
}

static UINT64 align_up(UINT64 value, UINT64 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 size)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

static bool create_default_resource(ID3D12Device *device,
                                    const D3D12_RESOURCE_DESC &desc,
                                    D3D12_RESOURCE_STATES state,
                                    ComPtr<ID3D12Resource> *resource)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    return SUCCEEDED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
        IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())));
}

static bool create_upload_resource(ID3D12Device *device, UINT64 size,
                                   ComPtr<ID3D12Resource> *resource)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    const D3D12_RESOURCE_DESC desc = buffer_desc(size);
    return SUCCEEDED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())));
}

static bool create_readback_resource(ID3D12Device *device, UINT64 size,
                                     ComPtr<ID3D12Resource> *resource)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    const D3D12_RESOURCE_DESC desc = buffer_desc(size);
    return SUCCEEDED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())));
}

static bool find_hevc444_configuration(
    ID3D12VideoDevice3 *video,
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile,
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC *configuration,
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1 *picture_defaults)
{
    if (!configuration || !picture_defaults)
        return false;
    const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE cu_sizes[] = {
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_8x8,
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_16x16,
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_32x32,
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_64x64};
    const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE tu_sizes[] = {
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_4x4,
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_8x8,
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_16x16,
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_32x32};

    for (const auto min_cu : cu_sizes) {
        for (const auto max_cu : cu_sizes) {
            if (static_cast<int>(max_cu) < static_cast<int>(min_cu)) continue;
            for (const auto min_tu : tu_sizes) {
                for (const auto max_tu : tu_sizes) {
                    if (static_cast<int>(max_tu) < static_cast<int>(min_tu)) continue;
                    for (UINT depth = 0; depth <= 4; ++depth) {
                        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC1 limits = {};
                        limits.MinLumaCodingUnitSize = min_cu;
                        limits.MaxLumaCodingUnitSize = max_cu;
                        limits.MinLumaTransformUnitSize = min_tu;
                        limits.MaxLumaTransformUnitSize = max_tu;
                        limits.max_transform_hierarchy_depth_inter =
                            static_cast<UCHAR>(depth);
                        limits.max_transform_hierarchy_depth_intra =
                            static_cast<UCHAR>(depth);

                        D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT query = {};
                        query.NodeIndex = 0;
                        query.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
                        query.Profile = profile;
                        appsandbox_d3d12_hevc1::install_hevc1_support(&query, &limits);
                        const HRESULT hr = video->CheckFeatureSupport(
                            D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT,
                            &query, sizeof(query));
                        if (FAILED(hr) || query.IsSupported == FALSE)
                            continue;

                        /* The HEVC1 contract returns masks for all syntax fields
                           used by Main444. Select the first legal value rather
                           than sending zeroes that may be outside the driver
                           advertised domain. */
                        std::uint32_t config_flags = 0;
                        std::uint32_t picture_flags = 0;
                        if (!appsandbox_hevc444_probe::apply_required_configuration_flags(
                                static_cast<std::uint32_t>(limits.SupportFlags),
                                limits.SupportFlags1, &config_flags, &picture_flags))
                            continue;

                        const auto first_allowed = [](UINT mask, unsigned max,
                                                      UCHAR *value) {
                            for (unsigned candidate = 0; candidate <= max; ++candidate) {
                                if ((mask & (UINT{1} << candidate)) != 0) {
                                    *value = static_cast<UCHAR>(candidate);
                                    return true;
                                }
                            }
                            return false;
                        };
                        const auto first_qp_offset = [](UINT mask, CHAR *value) {
                            for (int candidate = -12; candidate <= 12; ++candidate) {
                                const unsigned bit = static_cast<unsigned>(candidate + 12);
                                if ((mask & (UINT{1} << bit)) != 0) {
                                    *value = static_cast<CHAR>(candidate);
                                    return true;
                                }
                            }
                            return false;
                        };
                        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1 picture = {};
                        if (!first_allowed(limits.allowed_diff_cu_chroma_qp_offset_depth_values,
                                           3, &picture.diff_cu_chroma_qp_offset_depth) ||
                            !first_allowed(limits.allowed_log2_sao_offset_scale_luma_values,
                                           6, &picture.log2_sao_offset_scale_luma) ||
                            !first_allowed(limits.allowed_log2_sao_offset_scale_chroma_values,
                                           6, &picture.log2_sao_offset_scale_chroma) ||
                            !first_allowed(limits.allowed_log2_max_transform_skip_block_size_minus2_values,
                                           3, &picture.log2_max_transform_skip_block_size_minus2) ||
                            !first_allowed(limits.allowed_chroma_qp_offset_list_len_minus1_values,
                                           5, &picture.chroma_qp_offset_list_len_minus1))
                            continue;
                        const unsigned list_count = picture.chroma_qp_offset_list_len_minus1 + 1;
                        bool picture_values_valid = true;
                        for (unsigned i = 0; i < list_count; ++i) {
                            if (!first_qp_offset(limits.allowed_cb_qp_offset_list_values[i],
                                                 &picture.cb_qp_offset_list[i]) ||
                                !first_qp_offset(limits.allowed_cr_qp_offset_list_values[i],
                                                 &picture.cr_qp_offset_list[i])) {
                                picture_values_valid = false;
                                break;
                            }
                        }
                        if (!picture_values_valid)
                            continue;

                        picture.Flags =
                            static_cast<D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC_FLAGS>(picture_flags);
                        configuration->ConfigurationFlags =
                            static_cast<D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAGS>(config_flags);
                        configuration->MinLumaCodingUnitSize = limits.MinLumaCodingUnitSize;
                        configuration->MaxLumaCodingUnitSize = limits.MaxLumaCodingUnitSize;
                        configuration->MinLumaTransformUnitSize = limits.MinLumaTransformUnitSize;
                        configuration->MaxLumaTransformUnitSize = limits.MaxLumaTransformUnitSize;
                        configuration->max_transform_hierarchy_depth_inter =
                            limits.max_transform_hierarchy_depth_inter;
                        configuration->max_transform_hierarchy_depth_intra =
                            limits.max_transform_hierarchy_depth_intra;
                        *picture_defaults = picture;
                        std::printf("PASS stage=hevc444-codec-config-support flags=0x%08x "
                                    "flags1=0x%08x picture_flags=0x%08x\n",
                                    static_cast<unsigned>(limits.SupportFlags),
                                    static_cast<unsigned>(limits.SupportFlags1),
                                    static_cast<unsigned>(picture_flags));
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static bool run_actual_encode(ID3D12Device *device, ID3D12VideoDevice3 *video,
                              D3D12_VIDEO_ENCODER_PROFILE_DESC profile,
                              D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC config,
                              D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1 picture_defaults,
                              UINT64 metadata_bytes, UINT64 bitstream_alignment)
{
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION config_union = {};
    config_union.DataSize = sizeof(config);
    config_union.pHEVCConfig = &config;
    D3D12_VIDEO_ENCODER_DESC encoder_desc = {};
    encoder_desc.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    encoder_desc.EncodeProfile = profile;
    encoder_desc.InputFormat = DXGI_FORMAT_AYUV;
    encoder_desc.CodecConfiguration = config_union;
    encoder_desc.MaxMotionEstimationPrecision =
        D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_MAXIMUM;

    ComPtr<ID3D12VideoEncoder> encoder;
    HRESULT hr = video->CreateVideoEncoder(&encoder_desc, IID_PPV_ARGS(&encoder));
    if (FAILED(hr)) {
        print_hr("host-d3d12-create-video-encoder", hr);
        return false;
    }
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC level_value = {};
    level_value.Level = D3D12_VIDEO_ENCODER_LEVELS_HEVC_51;
    level_value.Tier = D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN;
    D3D12_VIDEO_ENCODER_LEVEL_SETTING level = {};
    level.DataSize = sizeof(level_value);
    level.pHEVCLevelSetting = &level_value;
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC resolution = {kWidth, kHeight};
    D3D12_VIDEO_ENCODER_HEAP_DESC heap_desc = {};
    heap_desc.NodeMask = 0;
    heap_desc.Flags = D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE;
    heap_desc.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    heap_desc.EncodeProfile = profile;
    heap_desc.EncodeLevel = level;
    heap_desc.ResolutionsListCount = 1;
    heap_desc.pResolutionList = &resolution;
    ComPtr<ID3D12VideoEncoderHeap> encoder_heap;
    hr = video->CreateVideoEncoderHeap(&heap_desc, IID_PPV_ARGS(&encoder_heap));
    if (FAILED(hr)) {
        print_hr("host-d3d12-create-video-encoder-heap", hr);
        return false;
    }

    const UINT64 metadata_size = align_up(
        std::max<UINT64>(metadata_bytes, sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA)), 256);
    const UINT64 bitstream_size = align_up(8 * 1024 * 1024,
                                           std::max<UINT64>(bitstream_alignment, 1));
    const D3D12_RESOURCE_DESC input_desc = {
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kWidth, kHeight, 1, 1,
        DXGI_FORMAT_AYUV, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_NONE};
    ComPtr<ID3D12Resource> input;
    ComPtr<ID3D12Resource> bitstream;
    ComPtr<ID3D12Resource> metadata_hw;
    ComPtr<ID3D12Resource> metadata_resolved;
    if (!create_default_resource(device, input_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                 &input) ||
        !create_default_resource(device, buffer_desc(bitstream_size),
                                 D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
                                 &bitstream) ||
        !create_default_resource(device, buffer_desc(metadata_size),
                                 D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
                                 &metadata_hw) ||
        !create_default_resource(device, buffer_desc(metadata_size),
                                 D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
                                 &metadata_resolved)) {
        std::fputs("BLOCKED stage=host-d3d12-encode-resources\n", stderr);
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 upload_size = 0;
    device->GetCopyableFootprints(&input_desc, 0, 1, 0, &footprint, &rows,
                                  &row_size, &upload_size);
    ComPtr<ID3D12Resource> upload;
    if (!create_upload_resource(device, upload_size, &upload)) {
        std::fputs("BLOCKED stage=host-d3d12-encode-upload\n", stderr);
        return false;
    }
    ComPtr<ID3D12Resource> bitstream_readback;
    ComPtr<ID3D12Resource> metadata_readback;
    if (!create_readback_resource(device, bitstream_size, &bitstream_readback) ||
        !create_readback_resource(device, metadata_size, &metadata_readback)) {
        std::fputs("BLOCKED stage=host-d3d12-encode-readback-resources\n", stderr);
        return false;
    }
    void *mapped = nullptr;
    D3D12_RANGE no_read = {0, 0};
    if (FAILED(upload->Map(0, &no_read, &mapped))) return false;
    auto *pixels = static_cast<std::uint8_t *>(mapped);
    for (UINT y = 0; y < kHeight; ++y) {
        auto *row = pixels + footprint.Offset + static_cast<UINT64>(y) * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < kWidth; ++x) {
            row[x * 4 + 0] = static_cast<std::uint8_t>(16 + (x + y) % 220);
            row[x * 4 + 1] = 128;
            row[x * 4 + 2] = 128;
            row[x * 4 + 3] = 255;
        }
    }
    upload->Unmap(0, nullptr);

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> copy_queue;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&copy_queue)))) return false;
    ComPtr<ID3D12CommandAllocator> copy_allocator;
    ComPtr<ID3D12GraphicsCommandList> copy_list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&copy_allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          copy_allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&copy_list)))) return false;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = input.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    copy_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER input_barrier = {};
    input_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    input_barrier.Transition.pResource = input.Get();
    input_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    input_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    input_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    copy_list->ResourceBarrier(1, &input_barrier);
    if (FAILED(copy_list->Close())) return false;
    ID3D12CommandList *copy_lists[] = {copy_list.Get()};
    copy_queue->ExecuteCommandLists(1, copy_lists);
    ComPtr<ID3D12Fence> copy_fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&copy_fence))) ||
        FAILED(copy_queue->Signal(copy_fence.Get(), 1)))
        return false;

    D3D12_COMMAND_QUEUE_DESC encode_queue_desc = {};
    encode_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;
    ComPtr<ID3D12CommandQueue> encode_queue;
    if (FAILED(device->CreateCommandQueue(&encode_queue_desc,
                                          IID_PPV_ARGS(&encode_queue)))) return false;
    ComPtr<ID3D12CommandAllocator> encode_allocator;
    ComPtr<ID3D12VideoEncodeCommandList2> encode_list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
                                               IID_PPV_ARGS(&encode_allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
                                          encode_allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&encode_list)))) return false;
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC gop_value = {};
    gop_value.GOPLength = 1;
    gop_value.PPicturePeriod = 0;
    gop_value.log2_max_pic_order_cnt_lsb_minus4 = 4;
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop = {};
    gop.DataSize = sizeof(gop_value);
    gop.pHEVCGroupOfPictures = &gop_value;
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP cqp = {};
    cqp.ConstantQP_FullIntracodedFrame = 28;
    cqp.ConstantQP_InterPredictedFrame_PrevRefOnly = 28;
    cqp.ConstantQP_InterPredictedFrame_BiDirectionalRef = 28;
    D3D12_VIDEO_ENCODER_RATE_CONTROL rate = {};
    rate.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
    rate.ConfigParams.DataSize = sizeof(cqp);
    rate.ConfigParams.pConfiguration_CQP = &cqp;
    rate.TargetFrameRate = {kFpsNumerator, kFpsDenominator};
    D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_DESC sequence = {};
    sequence.Flags = D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_NONE;
    sequence.RateControl = rate;
    sequence.PictureTargetResolution = resolution;
    sequence.SelectedLayoutMode = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
    sequence.CodecGopSequence = gop;
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1 picture_data =
        picture_defaults;
    picture_data.FrameType = D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_I_FRAME;
    picture_data.PictureOrderCountNumber = 0;
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_DESC picture = {};
    appsandbox_d3d12_hevc1::install_hevc1_picture_control(&picture, &picture_data);
    D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS encode_input = {};
    encode_input.SequenceControlDesc = sequence;
    encode_input.PictureControlDesc = picture;
    encode_input.pInputFrame = input.Get();
    D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS encode_output = {};
    encode_output.Bitstream.pBuffer = bitstream.Get();
    encode_output.Bitstream.FrameStartOffset = 0;
    encode_output.EncoderOutputMetadata.pBuffer = metadata_hw.Get();
    encode_output.EncoderOutputMetadata.Offset = 0;
    input_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    input_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
    encode_list->ResourceBarrier(1, &input_barrier);
    encode_list->EncodeFrame(encoder.Get(), encoder_heap.Get(), &encode_input,
                              &encode_output);
    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS resolve_input = {};
    resolve_input.EncoderCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    resolve_input.EncoderProfile = profile;
    resolve_input.EncoderInputFormat = DXGI_FORMAT_AYUV;
    resolve_input.EncodedPictureEffectiveResolution = resolution;
    resolve_input.HWLayoutMetadata = encode_output.EncoderOutputMetadata;
    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS resolve_output = {};
    resolve_output.ResolvedLayoutMetadata.pBuffer = metadata_resolved.Get();
    encode_list->ResolveEncoderOutputMetadata(&resolve_input, &resolve_output);
    D3D12_RESOURCE_BARRIER output_barrier = {};
    output_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    output_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
    output_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    output_barrier.Transition.pResource = bitstream.Get();
    encode_list->ResourceBarrier(1, &output_barrier);
    output_barrier.Transition.pResource = metadata_hw.Get();
    encode_list->ResourceBarrier(1, &output_barrier);
    output_barrier.Transition.pResource = metadata_resolved.Get();
    encode_list->ResourceBarrier(1, &output_barrier);
    if (FAILED(encode_list->Close())) return false;
    ID3D12CommandList *encode_lists[] = {encode_list.Get()};
    encode_queue->Wait(copy_fence.Get(), 1);
    encode_queue->ExecuteCommandLists(1, encode_lists);
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&fence)))) return false;
    constexpr UINT64 signal_value = 1;
    if (FAILED(encode_queue->Signal(fence.Get(), signal_value))) return false;
    HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_handle) return false;
    const HRESULT wait_registration = fence->SetEventOnCompletion(signal_value, event_handle);
    if (SUCCEEDED(wait_registration)) WaitForSingleObject(event_handle, INFINITE);
    CloseHandle(event_handle);
    if (FAILED(wait_registration)) return false;

    ComPtr<ID3D12CommandAllocator> readback_allocator;
    ComPtr<ID3D12GraphicsCommandList> readback_list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&readback_allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          readback_allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&readback_list)))) return false;
    D3D12_RESOURCE_BARRIER metadata_barrier = {};
    metadata_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    metadata_barrier.Transition.pResource = metadata_resolved.Get();
    metadata_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    metadata_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    metadata_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    readback_list->ResourceBarrier(1, &metadata_barrier);
    readback_list->CopyBufferRegion(metadata_readback.Get(), 0,
                                    metadata_resolved.Get(), 0,
                                    sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA));
    if (FAILED(readback_list->Close())) return false;
    ID3D12CommandList *readback_lists[] = {readback_list.Get()};
    copy_queue->Wait(fence.Get(), signal_value);
    copy_queue->ExecuteCommandLists(1, readback_lists);
    ComPtr<ID3D12Fence> readback_fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&readback_fence))) ||
        FAILED(copy_queue->Signal(readback_fence.Get(), signal_value))) return false;
    event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_handle) return false;
    const HRESULT readback_wait = readback_fence->SetEventOnCompletion(signal_value, event_handle);
    if (SUCCEEDED(readback_wait)) WaitForSingleObject(event_handle, INFINITE);
    CloseHandle(event_handle);
    if (FAILED(readback_wait)) return false;
    void *metadata_mapped = nullptr;
    const D3D12_RANGE metadata_range =
        {0, sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA)};
    if (FAILED(metadata_readback->Map(0, &metadata_range, &metadata_mapped)))
        return false;
    const auto *metadata = static_cast<const D3D12_VIDEO_ENCODER_OUTPUT_METADATA *>(
        metadata_mapped);
    const UINT64 written_bytes = metadata->EncodedBitstreamWrittenBytesCount;
    const bool metadata_ok = metadata->EncodeErrorFlags == 0 &&
        written_bytes > 0 && written_bytes <= bitstream_size;
    std::printf("host_d3d12_hevc444_encode_error_flags=0x%llx\n",
                static_cast<unsigned long long>(metadata->EncodeErrorFlags));
    std::printf("host_d3d12_hevc444_encoded_bytes=%llu\n",
                static_cast<unsigned long long>(written_bytes));
    metadata_readback->Unmap(0, nullptr);
    if (!metadata_ok) {
        std::printf("host_d3d12_hevc444_encode_submission=0\n");
        return false;
    }

    if (FAILED(readback_allocator->Reset()) ||
        FAILED(readback_list->Reset(readback_allocator.Get(), nullptr)))
        return false;
    D3D12_RESOURCE_BARRIER bitstream_barrier = {};
    bitstream_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bitstream_barrier.Transition.pResource = bitstream.Get();
    bitstream_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bitstream_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bitstream_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    readback_list->ResourceBarrier(1, &bitstream_barrier);
    readback_list->CopyBufferRegion(bitstream_readback.Get(), 0, bitstream.Get(), 0,
                                    written_bytes);
    if (FAILED(readback_list->Close())) return false;
    copy_queue->ExecuteCommandLists(1, readback_lists);
    const UINT64 bitstream_readback_value = signal_value + 1;
    if (FAILED(copy_queue->Signal(readback_fence.Get(), bitstream_readback_value)))
        return false;
    event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_handle) return false;
    const HRESULT bitstream_wait = readback_fence->SetEventOnCompletion(
        bitstream_readback_value, event_handle);
    if (SUCCEEDED(bitstream_wait)) WaitForSingleObject(event_handle, INFINITE);
    CloseHandle(event_handle);
    if (FAILED(bitstream_wait)) return false;

    void *encoded = nullptr;
    const D3D12_RANGE encoded_range = {0, written_bytes};
    if (FAILED(bitstream_readback->Map(0, &encoded_range, &encoded))) return false;
    const auto *bytes = static_cast<const std::uint8_t *>(encoded);
    std::vector<std::uint8_t> stream(bytes, bytes + written_bytes);
    bitstream_readback->Unmap(0, nullptr);

    std::vector<hevc_access_unit_probe::Nal> nals;
    const bool split_ok = hevc_access_unit_probe::split(stream, &nals);
    bool have_vps = false, have_sps = false, have_pps = false, have_irap = false;
    for (const auto &nal : nals) {
        have_vps |= nal.type == 32;
        have_sps |= nal.type == 33;
        have_pps |= nal.type == 34;
        have_irap |= hevc_access_unit_probe::is_irap(nal.type);
    }
    appsandbox_hevc444_probe::ParsedConfig parsed = {};
    const bool parsed_sequence =
        appsandbox_hevc444_probe::parse_hevc444_sequence_headers(stream, &parsed);
    const bool sequence_header_ok = split_ok && parsed_sequence && have_vps &&
        have_sps && have_pps && parsed.width == kWidth && parsed.height == kHeight &&
        parsed.configuration_flags ==
            static_cast<std::uint32_t>(config.ConfigurationFlags) &&
        parsed.picture_flags == static_cast<std::uint32_t>(picture_defaults.Flags);
    const bool stream_ok = sequence_header_ok && have_irap;
    std::printf("host_d3d12_hevc444_sequence_header_444=%u\n",
                sequence_header_ok ? 1U : 0U);
    std::printf("host_d3d12_hevc444_irap=%u\n", have_irap ? 1U : 0U);
    std::printf("host_d3d12_hevc444_encode_submission=%u\n", stream_ok ? 1U : 0U);
    return stream_ok;
}

static bool query_capability(ID3D12Device *device, ID3D12VideoDevice3 *video)
{
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC codec = {};
    codec.NodeIndex = 0;
    codec.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    HRESULT hr = video->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_CODEC, &codec, sizeof(codec));
    const bool codec_ok = SUCCEEDED(hr) && codec.IsSupported != FALSE;
    std::printf("host_d3d12_hevc_codec=%u\n", codec_ok ? 1U : 0U);

    D3D12_VIDEO_ENCODER_PROFILE_HEVC profile_value = kMain444;
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile = {};
    profile.DataSize = sizeof(profile_value);
    profile.pHEVCProfile = &profile_value;
    D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL profile_level = {};
    profile_level.NodeIndex = 0;
    profile_level.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    profile_level.Profile = profile;
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC min_level = {};
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC max_level = {};
    min_level.Level = D3D12_VIDEO_ENCODER_LEVELS_HEVC_1;
    min_level.Tier = D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN;
    max_level.Level = D3D12_VIDEO_ENCODER_LEVELS_HEVC_51;
    max_level.Tier = D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN;
    profile_level.MinSupportedLevel.DataSize = sizeof(min_level);
    profile_level.MinSupportedLevel.pHEVCLevelSetting = &min_level;
    profile_level.MaxSupportedLevel.DataSize = sizeof(max_level);
    profile_level.MaxSupportedLevel.pHEVCLevelSetting = &max_level;
    hr = video->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL, &profile_level,
        sizeof(profile_level));
    const bool profile_ok = SUCCEEDED(hr) && profile_level.IsSupported != FALSE;
    std::printf("host_d3d12_hevc444_profile=%u\n", profile_ok ? 1U : 0U);
    if (FAILED(hr)) print_hr("host-d3d12-hevc444-profile", hr);

    D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT input = {};
    input.NodeIndex = 0;
    input.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    input.Profile = profile;
    input.Format = DXGI_FORMAT_AYUV;
    hr = video->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, &input, sizeof(input));
    const bool ayuv_ok = SUCCEEDED(hr) && input.IsSupported != FALSE;
    std::printf("host_d3d12_hevc444_ayuv=%u\n", ayuv_ok ? 1U : 0U);
    if (FAILED(hr)) print_hr("host-d3d12-hevc444-ayuv", hr);

    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC requested_config = {};
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1 picture_defaults = {};
    const bool config_ok = profile_ok && find_hevc444_configuration(
        video, profile, &requested_config, &picture_defaults);
    std::printf("host_d3d12_hevc444_codec_configuration=%u\n",
                config_ok ? 1U : 0U);

    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS requirements = {};
    requirements.NodeIndex = 0;
    requirements.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    requirements.Profile = profile;
    requirements.InputFormat = DXGI_FORMAT_AYUV;
    requirements.PictureTargetResolution = {kWidth, kHeight};
    hr = video->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS, &requirements,
        sizeof(requirements));
    const bool resource_ok = SUCCEEDED(hr) && requirements.IsSupported != FALSE;
    std::printf("host_d3d12_hevc444_resource_requirements=%u\n",
                resource_ok ? 1U : 0U);
    if (FAILED(hr)) print_hr("host-d3d12-hevc444-resource-requirements", hr);

    const bool four_k60 = codec_ok && profile_ok && ayuv_ok && config_ok &&
        resource_ok;
    std::printf("host_d3d12_hevc444_4k60=%u\n", four_k60 ? 1U : 0U);
    if (!four_k60) {
        std::printf("host_d3d12_hevc444_actual_encode=BLOCKED "
                    "reason=capability-query-failed\n");
        return false;
    }

    if (!config_ok) {
        std::printf("host_d3d12_hevc444_actual_encode=BLOCKED "
                    "reason=no-valid-main444-configuration\n");
        return false;
    }
    const UINT64 metadata_bytes = std::max<UINT64>(
        requirements.MaxEncoderOutputMetadataBufferSize,
        sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA));
    const bool encoded = run_actual_encode(
        device, video, profile, requested_config, picture_defaults, metadata_bytes,
        requirements.CompressedBitstreamBufferAccessAlignment);
    std::printf("host_d3d12_hevc444_actual_encode=%s\n",
                encoded ? "PASS" : "BLOCKED");
    return encoded;
}

} // namespace

int wmain()
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12VideoDevice3> video;
    if (!create_video_device(&video, &device))
        return 1;
    const bool ok = query_capability(device.Get(), video.Get());
    return ok ? 0 : 1;
}
