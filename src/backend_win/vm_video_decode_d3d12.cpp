/* SPDX-License-Identifier: MIT */
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <dxgi1_2.h>
#include <dxva.h>
#include <wrl/client.h>
#include <memory>
#include <cstdio>
#include "vm_video_decode_d3d12.h"
#include "vm_hevc444_decode_config.h"
#pragma comment(lib, "d3d12.lib")
using Microsoft::WRL::ComPtr;
static void print_hr(const char *stage, HRESULT hr)
{
    std::fprintf(stderr, "%s hr=0x%08lx\n", stage, hr);
}
#include "vm_video_decode_d3d12_process.inl"

struct VmD3D12Decoder {
    ComPtr<ID3D11Device1> host;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12VideoDevice> video;
    ComPtr<ID3D12VideoDecoder> decoder;
    ComPtr<ID3D12VideoDecoderHeap> heap;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12VideoDecodeCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12QueryHeap> query;
    ComPtr<ID3D12Resource> statistics;
    DXVA_PicParams_HEVC_RangeExt picture = {};
    bool reference_only = false;
    UINT64 serial = 0;
};

static D3D12_RESOURCE_DESC buffer_desc(UINT64 size)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}
static HRESULT resource(VmD3D12Decoder *d, D3D12_HEAP_TYPE type,
    D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC &desc,
    D3D12_RESOURCE_STATES state, ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES p = {};
    p.Type = type; p.CreationNodeMask = p.VisibleNodeMask = 1;
    return d->device->CreateCommittedResource(&p, flags, &desc, state,
                                              nullptr, IID_PPV_ARGS(out));
}
static void transition(ID3D12VideoDecodeCommandList *list, ID3D12Resource *r,
    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

static DXVA_PicParams_HEVC_RangeExt picture_config(
    const appsandbox_hevc444_probe::ParsedConfig &c, bool bundled)
{
    DXVA_PicParams_HEVC_RangeExt picture = {};
    DXVA_PicParams_HEVC &pp = picture.params;
    pp.PicWidthInMinCbsY = static_cast<USHORT>(3840 / 8);
    pp.PicHeightInMinCbsY = static_cast<USHORT>(2160 / 8);
    pp.chroma_format_idc = 3;
    pp.bit_depth_luma_minus8 = 0;
    pp.bit_depth_chroma_minus8 = 0;
    pp.log2_max_pic_order_cnt_lsb_minus4 = 4;
    pp.NoPicReorderingFlag = 1;
    pp.NoBiPredFlag = 1;
    pp.CurrPic.bPicEntry = 0;
    pp.sps_max_dec_pic_buffering_minus1 = 2;
    pp.log2_min_luma_coding_block_size_minus3 = 0;
    pp.log2_diff_max_min_luma_coding_block_size = 3;
    pp.log2_min_transform_block_size_minus2 = 0;
    pp.log2_diff_max_min_transform_block_size = 3;
    pp.max_transform_hierarchy_depth_inter = 0;
    pp.max_transform_hierarchy_depth_intra = 0;
    pp.sample_adaptive_offset_enabled_flag = 1;
    pp.num_short_term_ref_pic_sets = 0;
    pp.num_long_term_ref_pics_sps = 0;
    pp.num_ref_idx_l0_default_active_minus1 = 0;
    pp.num_ref_idx_l1_default_active_minus1 = 0;
    pp.init_qp_minus26 = 0;
    pp.cu_qp_delta_enabled_flag = 1;
    pp.diff_cu_qp_delta_depth = 1;
    pp.pps_cb_qp_offset = 6;
    pp.pps_cr_qp_offset = 6;
    pp.entropy_coding_sync_enabled_flag = 1;
    pp.pps_loop_filter_across_slices_enabled_flag = 1;
    pp.log2_parallel_merge_level_minus2 = 0;
    pp.IrapPicFlag = 1;
    pp.IdrPicFlag = 1;
    pp.IntraPicFlag = 1;
    pp.CurrPicOrderCntVal = 0;
    pp.sps_temporal_mvp_enabled_flag = 1;
    pp.strong_intra_smoothing_enabled_flag = 1;
    for (auto &ref : pp.RefPicList) ref.bPicEntry = 0xff;
    for (auto &poc : pp.PicOrderCntValList) poc = 0;
    for (auto &ref : pp.RefPicSetStCurrBefore) ref = 0xff;
    for (auto &ref : pp.RefPicSetStCurrAfter) ref = 0xff;
    for (auto &ref : pp.RefPicSetLtCurr) ref = 0xff;

    if (!bundled) {
        using namespace appsandbox_hevc444_probe;
        pp.PicWidthInMinCbsY = static_cast<USHORT>(3840 >> (c.min_luma_coding_unit_size + 3));
        pp.PicHeightInMinCbsY = static_cast<USHORT>(2160 >> (c.min_luma_coding_unit_size + 3));
        pp.sps_max_dec_pic_buffering_minus1 = 4;
        pp.log2_min_luma_coding_block_size_minus3 = c.min_luma_coding_unit_size;
        pp.log2_diff_max_min_luma_coding_block_size = c.max_luma_coding_unit_size - c.min_luma_coding_unit_size;
        pp.log2_min_transform_block_size_minus2 = c.min_luma_transform_unit_size;
        pp.log2_diff_max_min_transform_block_size = c.max_luma_transform_unit_size - c.min_luma_transform_unit_size;
        pp.max_transform_hierarchy_depth_inter = c.max_transform_hierarchy_depth_inter;
        pp.max_transform_hierarchy_depth_intra = c.max_transform_hierarchy_depth_intra;
        pp.diff_cu_qp_delta_depth = 0;
        pp.pps_cb_qp_offset = pp.pps_cr_qp_offset = 0;
        pp.entropy_coding_sync_enabled_flag = 0;
        pp.amp_enabled_flag = !!(c.configuration_flags & kConfigUseAsymmetricMotionPartition);
        pp.sample_adaptive_offset_enabled_flag = !!(c.configuration_flags & kConfigEnableSaoFilter);
        pp.sps_temporal_mvp_enabled_flag = !!(c.configuration_flags & kConfigTemporalMvpEnabled);
        pp.strong_intra_smoothing_enabled_flag = !!(c.configuration_flags & kConfigStrongIntraSmoothingEnabled);
        pp.constrained_intra_pred_flag = !!(c.configuration_flags & kConfigUseConstrainedIntraprediction);
        pp.transform_skip_enabled_flag = !!(c.configuration_flags & kConfigEnableTransformSkipping);
        pp.pps_slice_chroma_qp_offsets_present_flag = !!(c.picture_flags & kPictureChromaQpOffsetList);
        picture.transform_skip_rotation_enabled_flag = !!(c.configuration_flags & kConfigTransformSkipRotation);
        picture.transform_skip_context_enabled_flag = !!(c.configuration_flags & kConfigTransformSkipContext);
        picture.implicit_rdpcm_enabled_flag = !!(c.configuration_flags & kConfigImplicitRdpcm);
        picture.explicit_rdpcm_enabled_flag = !!(c.configuration_flags & kConfigExplicitRdpcm);
        picture.extended_precision_processing_flag = !!(c.configuration_flags & kConfigExtendedPrecisionProcessing);
        picture.intra_smoothing_disabled_flag = !!(c.configuration_flags & kConfigIntraSmoothingDisabled);
        picture.high_precision_offsets_enabled_flag = !!(c.configuration_flags & kConfigHighPrecisionOffsets);
        picture.persistent_rice_adaptation_enabled_flag = !!(c.configuration_flags & kConfigPersistentRiceAdaptation);
        picture.cabac_bypass_alignment_enabled_flag = !!(c.configuration_flags & kConfigCabacBypassAlignment);
        picture.cross_component_prediction_enabled_flag = !!(c.picture_flags & kPictureCrossComponentPrediction);
        picture.chroma_qp_offset_list_enabled_flag = !!(c.picture_flags & kPictureChromaQpOffsetList);
        picture.diff_cu_chroma_qp_offset_depth = c.diff_cu_chroma_qp_offset_depth;
        picture.log2_sao_offset_scale_luma = c.log2_sao_offset_scale_luma;
        picture.log2_sao_offset_scale_chroma = c.log2_sao_offset_scale_chroma;
        picture.log2_max_transform_skip_block_size_minus2 = c.log2_max_transform_skip_block_size_minus2;
        picture.chroma_qp_offset_list_len_minus1 = c.chroma_qp_offset_list_len_minus1;
        std::memcpy(picture.cb_qp_offset_list, c.cb_qp_offset_list.data(), 6);
        std::memcpy(picture.cr_qp_offset_list, c.cr_qp_offset_list.data(), 6);
    }
    return picture;
}

extern "C" VmD3D12Decoder *vm_d3d12_create(ID3D11Device *host, UINT width,
    UINT height, UINT fps_num, UINT fps_den, const BYTE *headers, UINT size)
{
    if (!host || width != 3840 || height != 2160 || fps_num != 60 || fps_den != 1)
        return nullptr;
    try {
        appsandbox_hevc444_probe::ParsedConfig parsed;
        bool bundled;
        if (!asb_hevc_decode::configuration(headers, size, &parsed, &bundled)) return nullptr;
        auto d = std::make_unique<VmD3D12Decoder>();
        d->picture = picture_config(parsed, bundled);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(host->QueryInterface(IID_PPV_ARGS(&d->host))) ||
            FAILED(host->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
            FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d->device))) ||
            FAILED(d->device.As(&d->video))) return nullptr;
        D3D12_VIDEO_DECODE_CONFIGURATION config = {};
        config.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN_444;
        D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT support = {};
        support.Configuration = config; support.Width = width; support.Height = height;
        support.DecodeFormat = DXGI_FORMAT_AYUV; support.FrameRate = {60, 1};
        if (FAILED(d->video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &support, sizeof(support))) ||
            !(support.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED)) return nullptr;
        d->reference_only = !!(support.ConfigurationFlags & D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_REFERENCE_ONLY_ALLOCATIONS_REQUIRED);
        D3D12_VIDEO_DECODER_DESC decoder_desc = {};
        decoder_desc.NodeMask = 1;
        decoder_desc.Configuration = config;
        D3D12_VIDEO_DECODER_HEAP_DESC heap_desc = {};
        heap_desc.NodeMask = 1; heap_desc.Configuration = config;
        heap_desc.DecodeWidth = width; heap_desc.DecodeHeight = height;
        heap_desc.Format = DXGI_FORMAT_AYUV; heap_desc.FrameRate = {60, 1};
        heap_desc.MaxDecodePictureBufferCount = 8;
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE;
        D3D12_QUERY_HEAP_DESC query_desc = {D3D12_QUERY_HEAP_TYPE_VIDEO_DECODE_STATISTICS, 1, 1};
        if (FAILED(d->video->CreateVideoDecoder(&decoder_desc, IID_PPV_ARGS(&d->decoder))) ||
            FAILED(d->video->CreateVideoDecoderHeap(&heap_desc, IID_PPV_ARGS(&d->heap))) ||
            FAILED(d->device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&d->queue))) ||
            FAILED(d->device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&d->allocator))) ||
            FAILED(d->device->CreateCommandList(0, queue_desc.Type, d->allocator.Get(), nullptr, IID_PPV_ARGS(&d->list))) ||
            FAILED(d->list->Close()) ||
            FAILED(d->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d->fence))) ||
            FAILED(d->device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&d->query))) ||
            FAILED(resource(d.get(), D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_NONE,
                buffer_desc(sizeof(D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS)),
                D3D12_RESOURCE_STATE_COPY_DEST, &d->statistics))) return nullptr;
        return d.release();
    } catch (...) { return nullptr; }
}

extern "C" HRESULT vm_d3d12_decode(VmD3D12Decoder *d, const BYTE *data,
    UINT size, ID3D11Texture2D **texture)
{
    if (!texture) return E_POINTER;
    *texture = nullptr;
    if (!d) return E_INVALIDARG;
    try {
        std::vector<std::uint8_t> compressed;
        if (!asb_hevc_decode::idr_slice(data, size, &compressed)) return E_INVALIDARG;
        ComPtr<ID3D12Resource> upload, output, reference;
        HRESULT hr = resource(d, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_NONE,
            buffer_desc(compressed.size()), D3D12_RESOURCE_STATE_GENERIC_READ, &upload);
        if (FAILED(hr)) return hr;
        void *mapped = nullptr;
        D3D12_RANGE no_read = {0, 0};
        if (FAILED(hr = upload->Map(0, &no_read, &mapped))) return hr;
        std::memcpy(mapped, compressed.data(), compressed.size());
        upload->Unmap(0, nullptr);
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 3840; desc.Height = 2160; desc.DepthOrArraySize = 1;
        desc.MipLevels = 1; desc.Format = DXGI_FORMAT_AYUV; desc.SampleDesc.Count = 1;
        // A fresh shared surface owns each presented frame; the render thread
        // may retain it while the next packet is decoded. Never overwrite it.
        if (FAILED(hr = resource(d, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_SHARED,
            desc, D3D12_RESOURCE_STATE_COMMON, &output))) return hr;
        if (d->reference_only) {
            desc.Flags = D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
            if (FAILED(hr = resource(d, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE,
                desc, D3D12_RESOURCE_STATE_COMMON, &reference))) return hr;
        }
        if (FAILED(hr = d->allocator->Reset()) || FAILED(hr = d->list->Reset(d->allocator.Get()))) return hr;
        DXVA_Slice_HEVC_Short slice = {};
        slice.SliceBytesInBuffer = static_cast<UINT>(compressed.size());
        d->picture.params.StatusReportFeedbackNumber = static_cast<UINT>(d->serial + 1);
        D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS in = {};
        in.NumFrameArguments = 2;
        in.FrameArguments[0] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, sizeof(d->picture), &d->picture};
        in.FrameArguments[1] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, sizeof(slice), &slice};
        in.CompressedBitstream = {upload.Get(), 0, compressed.size()};
        in.pHeap = d->heap.Get();
        D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS out = {};
        out.pOutputTexture2D = output.Get();
        ID3D12Resource *refs[] = {reference.Get()};
        UINT subresources[] = {0};
        if (d->reference_only) {
            out.ConversionArguments.Enable = TRUE;
            out.ConversionArguments.pReferenceTexture2D = reference.Get();
            in.ReferenceFrames.NumTexture2Ds = 1;
            in.ReferenceFrames.ppTexture2Ds = refs;
            in.ReferenceFrames.pSubresources = subresources;
            transition(d->list.Get(), reference.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE);
        }
        transition(d->list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE);
        d->list->DecodeFrame(d->decoder.Get(), &out, &in);
        transition(d->list.Get(), output.Get(), D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE, D3D12_RESOURCE_STATE_COMMON);
        if (reference) transition(d->list.Get(), reference.Get(), D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE, D3D12_RESOURCE_STATE_COMMON);
        d->list->EndQuery(d->query.Get(), D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, 0);
        d->list->ResolveQueryData(d->query.Get(), D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, 0, 1, d->statistics.Get(), 0);
        if (FAILED(hr = d->list->Close())) return hr;
        ID3D12CommandList *lists[] = {d->list.Get()};
        d->queue->ExecuteCommandLists(1, lists);
        if (FAILED(hr = d->queue->Signal(d->fence.Get(), ++d->serial))) return hr;
        // Keep all submitted resources alive until the queue completes (or
        // the device reports removal). Polling avoids closing a live event.
        while (d->fence->GetCompletedValue() < d->serial) {
            if (FAILED(hr = d->device->GetDeviceRemovedReason())) return hr;
            Sleep(1);
        }
        if (FAILED(hr = d->device->GetDeviceRemovedReason())) return hr;
        D3D12_RANGE range = {0, sizeof(D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS)};
        if (FAILED(hr = d->statistics->Map(0, &range, &mapped))) return hr;
        const auto status = static_cast<D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS *>(mapped)->Status;
        d->statistics->Unmap(0, &no_read);
        if (status != D3D12_VIDEO_DECODE_STATUS_OK) return E_FAIL;
        // D3D11 cannot open a D3D12 AYUV allocation on all drivers. Convert
        // on the video-process queue, then share the BGRA target instead.
        ComPtr<ID3D12Resource> rgb;
        if (!convert_ayuv_to_rgb(d->device.Get(), d->video.Get(), output.Get(), &rgb)) return E_FAIL;
        HANDLE shared = nullptr;
        if (FAILED(hr = d->device->CreateSharedHandle(rgb.Get(), nullptr, GENERIC_ALL, nullptr, &shared))) return hr;
        hr = d->host->OpenSharedResource1(shared, IID_PPV_ARGS(texture));
        if (FAILED(hr)) print_hr("d3d12-shared-bgra-open", hr);
        CloseHandle(shared);
        return hr;
    } catch (...) { return E_OUTOFMEMORY; }
}
extern "C" void vm_d3d12_destroy(VmD3D12Decoder *d) { delete d; }

// Diagnostic readback is used only once per device capability probe. The
// production decode function never maps an uncompressed frame.
static bool known_sample_written(ID3D11Device *host, ID3D11Texture2D *texture)
{
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(host->CreateTexture2D(&desc, nullptr, &staging))) return false;
    host->GetImmediateContext(&context);
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    std::uint64_t hash = 1469598103934665603ull;
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto *row = static_cast<const BYTE *>(map.pData) + size_t(y) * map.RowPitch;
        for (UINT x = 0; x < desc.Width * 4; ++x) { hash ^= row[x]; hash *= 1099511628211ull; }
    }
    context->Unmap(staging.Get(), 0);
    return hash == 0xa3e0e636ed338383ull;
}

extern "C" BOOL vm_d3d12_probe(ID3D11Device *host, VmVideoDecodeCapability *out)
{
    if (!host || !out) return FALSE;
    *out = {};
    std::unique_ptr<VmD3D12Decoder> d(vm_d3d12_create(host, 3840, 2160, 60, 1,
        asb_hevc444_probe_sample, ASB_HEVC444_PROBE_EXTRADATA_SIZE));
    if (!d) return FALSE;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(vm_d3d12_decode(d.get(), asb_hevc444_probe_sample + ASB_HEVC444_PROBE_ACCESS_UNIT_OFFSET,
        ASB_HEVC444_PROBE_ACCESS_UNIT_SIZE, &texture)) || !known_sample_written(host, texture.Get())) return FALSE;
    out->backend = VM_VIDEO_DECODE_BACKEND_D3D12;
    out->adapter_luid = d->device->GetAdapterLuid();
    out->device_identity = host;
    out->actual_decode = out->gpu_surface = out->decoder_identity_valid = TRUE;
    out->decoded_format = DXGI_FORMAT_AYUV;
    out->presentation_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    wcscpy_s(out->decoder_name, L"D3D12 HEVC Main444");
    // Exercise the window renderer's D3D11 VideoProcessorBlt with the exact
    // BGRA surface produced by D3D12 AYUV conversion, before advertising.
    out->ayuv_video_processor = vm_video_decode_probe_ayuv_surface(host, texture.Get());
    out->available = vm_video_decode_evidence_ready(out->actual_decode, TRUE,
        out->gpu_surface, out->ayuv_video_processor, FALSE);
    return out->available;
}
