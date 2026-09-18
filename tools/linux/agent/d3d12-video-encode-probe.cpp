/* SPDX-License-Identifier: MIT
 *
 * D3D12 shared BGRA texture -> GPU NV12 -> D3D12 HEVC encode probe.
 *
 * The existing cross-process share probe is included deliberately: this keeps
 * the SCM_RIGHTS, eventfd, independent exec, and triple-buffer protocol
 * identical while replacing only the consumer GPU operation.
 */

#include <wsl/winadapter.h>
#ifndef _In_count_
#define _In_count_(count)
#endif
#ifndef _In_opt_count_
#define _In_opt_count_(count)
#endif
#include <directx/d3d12.h>
#include <directx/d3d12video.h>
#include <directx/dxcore.h>
#include <dxguids/dxguids.h>

#define main d3d12_cross_process_share_probe_unused_main
#include "d3d12-cross-process-share-probe.cpp"
#undef main

#include <algorithm>
#include <fstream>
#include <limits>

namespace {

constexpr D3D12_VIDEO_ENCODER_CODEC kEncodeCodec =
    D3D12_VIDEO_ENCODER_CODEC_HEVC;
constexpr UINT kNodeIndex = 0;
constexpr UINT kFrameRateNumerator = 60;
constexpr UINT kFrameRateDenominator = 1;
constexpr UINT64 kBitstreamCapacity = 8ULL * 1024ULL * 1024ULL;

// D3D12 Video Encode returns picture payload NAL units; sequence headers are
// host-owned. These VPS/SPS/PPS NALs match the fixed probe configuration:
// HEVC Main, 3840x2160, level 5.1, 32x32 CTU, 8x8 minimum CU, 8-bit POC.
// They are metadata only and never contain framebuffer pixels.
static constexpr std::uint8_t kHevcSequenceHeaders[] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x04,
    0x08, 0x00, 0x00, 0x03, 0x00, 0x9F, 0xA8, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x99, 0xBA, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01,
    0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x9F, 0xA8, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x99, 0xA0, 0x01, 0xE0, 0x20, 0x02, 0x1C, 0x59,
    0x6E, 0xAE, 0x46, 0xC2, 0xF0, 0x16, 0x80, 0x80, 0x00, 0x00, 0x03,
    0x00, 0x80, 0x00, 0x00, 0x1E, 0x04, 0x00, 0x00, 0x00, 0x01, 0x44,
    0x01, 0xC0, 0x71, 0x83, 0x12};

static D3D12_VIDEO_ENCODER_PROFILE_DESC hevc_profile(
    D3D12_VIDEO_ENCODER_PROFILE_HEVC *profile_value)
{
    *profile_value = D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN;
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile = {};
    profile.DataSize = sizeof(*profile_value);
    profile.pHEVCProfile = profile_value;
    return profile;
}

static D3D12_VIDEO_ENCODER_LEVEL_SETTING hevc_level(
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC *level_value)
{
    level_value->Level = D3D12_VIDEO_ENCODER_LEVELS_HEVC_51;
    level_value->Tier = D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN;
    D3D12_VIDEO_ENCODER_LEVEL_SETTING level = {};
    level.DataSize = sizeof(*level_value);
    level.pHEVCLevelSetting = level_value;
    return level;
}

template <typename T>
static bool query_video_feature(ID3D12VideoDevice3 *video_device,
                                D3D12_FEATURE_VIDEO feature, T *data,
                                const char *stage)
{
    const HRESULT hr = video_device->CheckFeatureSupport(
        feature, data, static_cast<UINT>(sizeof(*data)));
    return hr_ok(hr, stage);
}

struct HevcConfiguration {
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC requested = {};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC reported = {};
};

static bool find_hevc_configuration(ID3D12VideoDevice3 *video_device,
                                    const D3D12_VIDEO_ENCODER_PROFILE_DESC &profile,
                                    HevcConfiguration *found)
{
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
            if (static_cast<int>(max_cu) < static_cast<int>(min_cu))
                continue;
            for (const auto min_tu : tu_sizes) {
                for (const auto max_tu : tu_sizes) {
                    if (static_cast<int>(max_tu) < static_cast<int>(min_tu))
                        continue;
                    for (UINT depth = 0; depth <= 4; ++depth) {
                        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC candidate = {};
                        candidate.MinLumaCodingUnitSize = min_cu;
                        candidate.MaxLumaCodingUnitSize = max_cu;
                        candidate.MinLumaTransformUnitSize = min_tu;
                        candidate.MaxLumaTransformUnitSize = max_tu;
                        candidate.max_transform_hierarchy_depth_inter =
                            static_cast<UCHAR>(depth);
                        candidate.max_transform_hierarchy_depth_intra =
                            static_cast<UCHAR>(depth);

                        for (unsigned retry = 0; retry != 2; ++retry) {
                            D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT limits = {};
                            limits.DataSize = sizeof(candidate);
                            limits.pHEVCSupport = &candidate;
                            D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT query = {};
                            query.NodeIndex = kNodeIndex;
                            query.Codec = kEncodeCodec;
                            query.Profile = profile;
                            query.CodecSupportLimits = limits;
                            if (!query_video_feature(
                                    video_device,
                                    D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT,
                                    &query,
                                    "d3d12-video-codec-configuration-query"))
                                return false;
                            if (query.IsSupported == FALSE)
                                break;

                            const bool asymmetric_required =
                                (candidate.SupportFlags &
                                 D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_ASYMETRIC_MOTION_PARTITION_REQUIRED) !=
                                0;
                            if (asymmetric_required && retry == 0) {
                                candidate.SupportFlags =
                                    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_ASYMETRIC_MOTION_PARTITION_SUPPORT |
                                    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_ASYMETRIC_MOTION_PARTITION_REQUIRED;
                                continue;
                            }
                            found->requested.ConfigurationFlags =
                                asymmetric_required
                                    ? D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION
                                    : D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_NONE;
                            found->requested.MinLumaCodingUnitSize =
                                candidate.MinLumaCodingUnitSize;
                            found->requested.MaxLumaCodingUnitSize =
                                candidate.MaxLumaCodingUnitSize;
                            found->requested.MinLumaTransformUnitSize =
                                candidate.MinLumaTransformUnitSize;
                            found->requested.MaxLumaTransformUnitSize =
                                candidate.MaxLumaTransformUnitSize;
                            found->requested.max_transform_hierarchy_depth_inter =
                                candidate.max_transform_hierarchy_depth_inter;
                            found->requested.max_transform_hierarchy_depth_intra =
                                candidate.max_transform_hierarchy_depth_intra;
                            found->reported = candidate;
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

static bool probe_codec(ID3D12VideoDevice3 *video_device,
                        D3D12_VIDEO_ENCODER_CODEC codec)
{
    D3D12_VIDEO_ENCODER_PROFILE_H264 h264_profile_value = {};
    D3D12_VIDEO_ENCODER_PROFILE_HEVC hevc_profile_value = {};
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile = {};
    if (codec == D3D12_VIDEO_ENCODER_CODEC_H264) {
        h264_profile_value = D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN;
        profile.DataSize = sizeof(h264_profile_value);
        profile.pH264Profile = &h264_profile_value;
    } else {
        profile = hevc_profile(&hevc_profile_value);
    }

    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC codec_query = {};
    codec_query.NodeIndex = kNodeIndex;
    codec_query.Codec = codec;
    if (!query_video_feature(video_device, D3D12_FEATURE_VIDEO_ENCODER_CODEC,
                             &codec_query, "d3d12-video-codec-query"))
        return false;
    std::printf("%s stage=d3d12-video-codec codec=%s supported=%u\n",
                codec_query.IsSupported ? "PASS" : "BLOCKED",
                codec == D3D12_VIDEO_ENCODER_CODEC_H264 ? "H264" : "HEVC",
                codec_query.IsSupported ? 1U : 0U);
    if (!codec_query.IsSupported)
        return true;

    D3D12_VIDEO_ENCODER_LEVELS_H264 h264_min = {};
    D3D12_VIDEO_ENCODER_LEVELS_H264 h264_max = {};
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC hevc_min = {};
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC hevc_max = {};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL profile_query = {};
    profile_query.NodeIndex = kNodeIndex;
    profile_query.Codec = codec;
    profile_query.Profile = profile;
    if (codec == D3D12_VIDEO_ENCODER_CODEC_H264) {
        h264_min = D3D12_VIDEO_ENCODER_LEVELS_H264_1;
        h264_max = D3D12_VIDEO_ENCODER_LEVELS_H264_51;
        profile_query.MinSupportedLevel.DataSize = sizeof(h264_min);
        profile_query.MinSupportedLevel.pH264LevelSetting = &h264_min;
        profile_query.MaxSupportedLevel.DataSize = sizeof(h264_max);
        profile_query.MaxSupportedLevel.pH264LevelSetting = &h264_max;
    } else {
        hevc_min.Level = D3D12_VIDEO_ENCODER_LEVELS_HEVC_1;
        hevc_min.Tier = D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN;
        hevc_max.Level = D3D12_VIDEO_ENCODER_LEVELS_HEVC_51;
        hevc_max.Tier = D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN;
        profile_query.MinSupportedLevel.DataSize = sizeof(hevc_min);
        profile_query.MinSupportedLevel.pHEVCLevelSetting = &hevc_min;
        profile_query.MaxSupportedLevel.DataSize = sizeof(hevc_max);
        profile_query.MaxSupportedLevel.pHEVCLevelSetting = &hevc_max;
    }
    if (!query_video_feature(video_device,
                             D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL,
                             &profile_query,
                             "d3d12-video-profile-level-query"))
        return false;
    std::printf("%s stage=d3d12-video-profile codec=%s supported=%u\n",
                profile_query.IsSupported ? "PASS" : "BLOCKED",
                codec == D3D12_VIDEO_ENCODER_CODEC_H264 ? "H264" : "HEVC",
                profile_query.IsSupported ? 1U : 0U);
    if (!profile_query.IsSupported)
        return true;

    if (codec == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
        HevcConfiguration config = {};
        if (!find_hevc_configuration(video_device, profile, &config))
            return false;
        std::printf("PASS stage=d3d12-video-codec-configuration codec=HEVC "
                    "flags=0x%08x cu=%u..%u tu=%u..%u hierarchy=%u/%u\n",
                    static_cast<unsigned>(config.reported.SupportFlags),
                    static_cast<unsigned>(config.requested.MinLumaCodingUnitSize),
                    static_cast<unsigned>(config.requested.MaxLumaCodingUnitSize),
                    static_cast<unsigned>(config.requested.MinLumaTransformUnitSize),
                    static_cast<unsigned>(config.requested.MaxLumaTransformUnitSize),
                    static_cast<unsigned>(config.requested.max_transform_hierarchy_depth_inter),
                    static_cast<unsigned>(config.requested.max_transform_hierarchy_depth_intra));
    } else {
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264 limits = {};
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT config_limits = {};
        config_limits.DataSize = sizeof(limits);
        config_limits.pH264Support = &limits;
        D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT config = {};
        config.NodeIndex = kNodeIndex;
        config.Codec = codec;
        config.Profile = profile;
        config.CodecSupportLimits = config_limits;
        if (!query_video_feature(
                video_device,
                D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT,
                &config, "d3d12-video-codec-configuration-query"))
            return false;
        std::printf("%s stage=d3d12-video-codec-configuration codec=H264 "
                    "supported=%u flags=0x%08x\n",
                    config.IsSupported ? "PASS" : "BLOCKED",
                    config.IsSupported ? 1U : 0U,
                    static_cast<unsigned>(limits.SupportFlags));
    }

    D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT format = {};
    format.NodeIndex = kNodeIndex;
    format.Codec = codec;
    format.Profile = profile;
    format.Format = DXGI_FORMAT_NV12;
    if (!query_video_feature(video_device,
                             D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, &format,
                             "d3d12-video-nv12-query"))
        return false;
    std::printf("%s stage=d3d12-video-nv12-input codec=%s format=NV12 "
                "supported=%u\n",
                format.IsSupported ? "PASS" : "BLOCKED",
                codec == D3D12_VIDEO_ENCODER_CODEC_H264 ? "H264" : "HEVC",
                format.IsSupported ? 1U : 0U);

    D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT ratio_count = {};
    ratio_count.NodeIndex = kNodeIndex;
    ratio_count.Codec = codec;
    if (!query_video_feature(
            video_device,
            D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT,
            &ratio_count, "d3d12-video-resolution-ratio-count"))
        return false;
    std::vector<D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_RATIO_DESC> ratios(
        ratio_count.ResolutionRatiosCount);
    D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION resolution = {};
    resolution.NodeIndex = kNodeIndex;
    resolution.Codec = codec;
    resolution.ResolutionRatiosCount = ratio_count.ResolutionRatiosCount;
    resolution.pResolutionRatios = ratios.empty() ? nullptr : ratios.data();
    if (!query_video_feature(video_device,
                             D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION,
                             &resolution, "d3d12-video-resolution-query"))
        return false;
    const bool resolution_ok =
        resolution.IsSupported &&
        resolution.MinResolutionSupported.Width <= kWidth &&
        resolution.MinResolutionSupported.Height <= kHeight &&
        resolution.MaxResolutionSupported.Width >= kWidth &&
        resolution.MaxResolutionSupported.Height >= kHeight &&
        (resolution.ResolutionWidthMultipleRequirement == 0 ||
         kWidth % resolution.ResolutionWidthMultipleRequirement == 0) &&
        (resolution.ResolutionHeightMultipleRequirement == 0 ||
         kHeight % resolution.ResolutionHeightMultipleRequirement == 0);
    std::printf("%s stage=d3d12-video-4k-resolution codec=%s supported=%u "
                "min=%ux%u max=%ux%u multiples=%ux%u ratios=%u\n",
                resolution_ok ? "PASS" : "BLOCKED",
                codec == D3D12_VIDEO_ENCODER_CODEC_H264 ? "H264" : "HEVC",
                resolution_ok ? 1U : 0U, resolution.MinResolutionSupported.Width,
                resolution.MinResolutionSupported.Height,
                resolution.MaxResolutionSupported.Width,
                resolution.MaxResolutionSupported.Height,
                resolution.ResolutionWidthMultipleRequirement,
                resolution.ResolutionHeightMultipleRequirement,
                ratio_count.ResolutionRatiosCount);
    return true;
}

struct EncoderSetup {
    ComPtr<ID3D12VideoDevice3> video_device;
    ComPtr<ID3D12VideoEncoder> encoder;
    ComPtr<ID3D12VideoEncoderHeap> heap;
    D3D12_VIDEO_ENCODER_PROFILE_HEVC profile_value = {};
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile = {};
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC level_value = {};
    D3D12_VIDEO_ENCODER_LEVEL_SETTING level = {};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC configuration = {};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION configuration_union = {};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC gop_value = {};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop = {};
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP cqp_value = {};
    D3D12_VIDEO_ENCODER_RATE_CONTROL rate_control = {};
    UINT64 metadata_bytes = sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA);
    UINT64 bitstream_alignment = 1;
};

static bool initialize_encoder(DeviceContext *context, EncoderSetup *setup)
{
    if (FAILED(context->device.As(&setup->video_device))) {
        std::fputs("BLOCKED stage=d3d12-video-device-interface\n", stderr);
        return false;
    }
    setup->profile = hevc_profile(&setup->profile_value);
    setup->level = hevc_level(&setup->level_value);
    HevcConfiguration config = {};
    if (!find_hevc_configuration(setup->video_device.Get(), setup->profile,
                                 &config)) {
        std::fputs("BLOCKED stage=d3d12-video-codec-configuration\n", stderr);
        return false;
    }
    setup->configuration = config.requested;
    setup->configuration_union.DataSize = sizeof(setup->configuration);
    setup->configuration_union.pHEVCConfig = &setup->configuration;
    setup->gop_value.GOPLength = 1;
    setup->gop_value.PPicturePeriod = 0;
    setup->gop_value.log2_max_pic_order_cnt_lsb_minus4 = 4;
    setup->gop.DataSize = sizeof(setup->gop_value);
    setup->gop.pHEVCGroupOfPictures = &setup->gop_value;
    setup->cqp_value.ConstantQP_FullIntracodedFrame = 28;
    setup->cqp_value.ConstantQP_InterPredictedFrame_PrevRefOnly = 28;
    setup->cqp_value.ConstantQP_InterPredictedFrame_BiDirectionalRef = 28;
    setup->rate_control.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
    setup->rate_control.ConfigParams.DataSize = sizeof(setup->cqp_value);
    setup->rate_control.ConfigParams.pConfiguration_CQP = &setup->cqp_value;
    setup->rate_control.TargetFrameRate.Numerator = kFrameRateNumerator;
    setup->rate_control.TargetFrameRate.Denominator = kFrameRateDenominator;

    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC resolution = {kWidth, kHeight};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS limits = {};
    D3D12_VIDEO_ENCODER_PROFILE_HEVC suggested_profile_value = {};
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC suggested_level_value = {};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT support = {};
    support.NodeIndex = kNodeIndex;
    support.Codec = kEncodeCodec;
    support.InputFormat = DXGI_FORMAT_NV12;
    support.CodecConfiguration = setup->configuration_union;
    support.CodecGopSequence = setup->gop;
    support.RateControl = setup->rate_control;
    support.IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
    support.SubregionFrameEncoding =
        D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
    support.ResolutionsListCount = 1;
    support.pResolutionList = &resolution;
    support.pResolutionDependentSupport = &limits;
    support.SuggestedProfile.DataSize = sizeof(suggested_profile_value);
    support.SuggestedProfile.pHEVCProfile = &suggested_profile_value;
    support.SuggestedLevel.DataSize = sizeof(suggested_level_value);
    support.SuggestedLevel.pHEVCLevelSetting = &suggested_level_value;
    if (!query_video_feature(setup->video_device.Get(),
                             D3D12_FEATURE_VIDEO_ENCODER_SUPPORT, &support,
                             "d3d12-video-60fps-support"))
        return false;
    const bool support_ok =
        (support.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK) !=
            0 &&
        support.ValidationFlags == D3D12_VIDEO_ENCODER_VALIDATION_FLAG_NONE;
    std::printf("%s stage=d3d12-video-60fps-configuration codec=HEVC "
                "supported=%u validation=0x%08x support_flags=0x%08x\n",
                support_ok ? "PASS" : "BLOCKED", support_ok ? 1U : 0U,
                static_cast<unsigned>(support.ValidationFlags),
                static_cast<unsigned>(support.SupportFlags));
    if (!support_ok)
        return false;

    D3D12_VIDEO_ENCODER_DESC encoder_desc = {};
    encoder_desc.EncodeCodec = kEncodeCodec;
    encoder_desc.EncodeProfile = setup->profile;
    encoder_desc.InputFormat = DXGI_FORMAT_NV12;
    encoder_desc.CodecConfiguration = setup->configuration_union;
    encoder_desc.MaxMotionEstimationPrecision =
        D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_MAXIMUM;
    if (!hr_ok(setup->video_device->CreateVideoEncoder(
                   &encoder_desc, IID_PPV_ARGS(&setup->encoder)),
               "d3d12-video-create-encoder"))
        return false;
    const D3D12_VIDEO_ENCODER_HEAP_DESC heap_desc = {
        0, D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE, kEncodeCodec, setup->profile,
        setup->level, 1, &resolution};
    if (!hr_ok(setup->video_device->CreateVideoEncoderHeap(
                   &heap_desc, IID_PPV_ARGS(&setup->heap)),
               "d3d12-video-create-encoder-heap"))
        return false;

    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS requirements = {};
    requirements.NodeIndex = kNodeIndex;
    requirements.Codec = kEncodeCodec;
    requirements.Profile = setup->profile;
    requirements.InputFormat = DXGI_FORMAT_NV12;
    requirements.PictureTargetResolution = resolution;
    if (!query_video_feature(
            setup->video_device.Get(),
            D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS, &requirements,
            "d3d12-video-resource-requirements") ||
        !requirements.IsSupported)
        return false;
    setup->metadata_bytes = std::max<UINT64>(
        requirements.MaxEncoderOutputMetadataBufferSize,
        sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA));
    setup->bitstream_alignment = std::max<UINT64>(
        requirements.CompressedBitstreamBufferAccessAlignment, 1);
    std::printf("PASS stage=d3d12-video-objects-constructed codec=HEVC "
                "encoder=1 heap=1 resolution=%ux%u input=NV12 fps=60/1\n",
                kWidth, kHeight);
    return true;
}

struct ProcessSetup {
    ComPtr<ID3D12VideoProcessor> processor;
    D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC input_desc = {};
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC output_desc = {};
};

struct EncodeSlot {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> nv12;
    ComPtr<ID3D12Resource> bitstream;
    ComPtr<ID3D12Resource> metadata_hw;
    ComPtr<ID3D12Resource> metadata_resolved;
    ComPtr<ID3D12Resource> bitstream_readback;
    ComPtr<ID3D12Resource> metadata_readback;
    ComPtr<ID3D12Resource> input_readback;
    ComPtr<ID3D12CommandAllocator> process_allocator;
    ComPtr<ID3D12CommandAllocator> encode_allocator;
    ComPtr<ID3D12CommandAllocator> copy_allocator;
    ComPtr<ID3D12VideoProcessCommandList> process_list;
    ComPtr<ID3D12VideoEncodeCommandList2> encode_list;
    ComPtr<ID3D12GraphicsCommandList> copy_list;
    int resource_fd = -1;
    int ready_fd = -1;
    int done_fd = -1;
    int encode_fd = -1;
    int copy_fd = -1;
    UINT64 copy_value = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT input_footprint = {};
    std::uint64_t frame = 0;
    std::uint64_t encode_submit_ns = 0;
    std::uint64_t pipeline_start_ns = 0;
    std::array<std::uint8_t, 4> expected_bgra = {0, 0, 0, 255};
    bool copy_pending = false;
    bool input_diagnostic_pending = false;
    bool output_initialized = false;
    bool nv12_first_use = true;
};

static bool make_process_setup(DeviceContext *context, ProcessSetup *setup)
{
    setup->input_desc = {};
    setup->input_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    setup->input_desc.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    setup->input_desc.SourceAspectRatio = {1, 1};
    setup->input_desc.DestinationAspectRatio = {1, 1};
    setup->input_desc.FrameRate = {kFrameRateNumerator, kFrameRateDenominator};
    setup->input_desc.SourceSizeRange = {kWidth, kHeight, kWidth, kHeight};
    setup->input_desc.DestinationSizeRange = {kWidth, kHeight, kWidth, kHeight};
    setup->input_desc.StereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    setup->input_desc.FieldType = D3D12_VIDEO_FIELD_TYPE_NONE;
    setup->input_desc.DeinterlaceMode = D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_NONE;
    setup->output_desc = {};
    setup->output_desc.Format = DXGI_FORMAT_NV12;
    setup->output_desc.ColorSpace =
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    setup->output_desc.AlphaFillMode = D3D12_VIDEO_PROCESS_ALPHA_FILL_MODE_OPAQUE;
    setup->output_desc.FrameRate = {kFrameRateNumerator, kFrameRateDenominator};

    ComPtr<ID3D12VideoDevice3> video_device;
    if (!hr_ok(context->device.As(&video_device),
               "d3d12-video-processor-interface"))
        return false;
    D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT support = {};
    support.NodeIndex = kNodeIndex;
    support.InputSample.Width = kWidth;
    support.InputSample.Height = kHeight;
    support.InputSample.Format.Format = setup->input_desc.Format;
    support.InputSample.Format.ColorSpace = setup->input_desc.ColorSpace;
    support.InputFieldType = setup->input_desc.FieldType;
    support.InputStereoFormat = setup->input_desc.StereoFormat;
    support.InputFrameRate = setup->input_desc.FrameRate;
    support.OutputFormat.Format = setup->output_desc.Format;
    support.OutputFormat.ColorSpace = setup->output_desc.ColorSpace;
    support.OutputStereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    support.OutputFrameRate = setup->output_desc.FrameRate;
    if (!query_video_feature(video_device.Get(), D3D12_FEATURE_VIDEO_PROCESS_SUPPORT,
                             &support, "d3d12-video-bgra-nv12-support"))
        return false;
    if ((support.SupportFlags & D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED) == 0) {
        std::fputs("BLOCKED stage=d3d12-video-bgra-nv12-support\n", stderr);
        return false;
    }
    if (!hr_ok(video_device->CreateVideoProcessor(
                   0, &setup->output_desc, 1, &setup->input_desc,
                   IID_PPV_ARGS(&setup->processor)),
               "d3d12-video-create-processor"))
        return false;
    std::puts("PASS stage=bgra-to-nv12-gpu-only support=1 cpu_conversion=0");
    return true;
}

static bool create_default_resource(ID3D12Device *device,
                                    const D3D12_RESOURCE_DESC &desc,
                                    D3D12_RESOURCE_STATES state,
                                    ComPtr<ID3D12Resource> *resource)
{
    const D3D12_HEAP_PROPERTIES heap = default_heap_properties();
    return hr_ok(device->CreateCommittedResource(
                     &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                     IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())),
                 "d3d12-video-resource");
}

static bool create_readback_resource(ID3D12Device *device, UINT64 size,
                                     ComPtr<ID3D12Resource> *resource)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    const D3D12_RESOURCE_DESC desc = buffer_desc(size);
    return hr_ok(device->CreateCommittedResource(
                     &heap, D3D12_HEAP_FLAG_NONE, &desc,
                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                     IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())),
                 "d3d12-video-readback-resource");
}

static UINT64 align_up(UINT64 value, UINT64 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

static bool collect_encoded_slot(EncodeSlot *slot, std::ofstream *stream,
                                 UINT64 bitstream_capacity,
                                 UINT64 metadata_size,
                                 std::uint64_t *encoded_frames,
                                 std::uint64_t *encoded_bytes,
                                 std::uint64_t *encode_failures,
                                 std::uint64_t *timeouts,
                                 std::uint64_t *mismatches,
                                 std::uint64_t *diagnostic_checks,
                                 std::vector<std::uint64_t> *encode_complete_times,
                                 std::vector<std::uint64_t> *total_pipeline_times,
                                 std::vector<std::uint64_t> *slot_recycle_times)
{
    if (!slot->copy_pending)
        return true;
    std::uint64_t wake_ns = 0;
    if (!wait_eventfd(slot->encode_fd, "d3d12-video-encode", &wake_ns,
                      timeouts))
        return false;
    if (wake_ns >= slot->encode_submit_ns)
        encode_complete_times->push_back(wake_ns - slot->encode_submit_ns);
    if (wake_ns >= slot->pipeline_start_ns)
        total_pipeline_times->push_back(wake_ns - slot->pipeline_start_ns);
    wake_ns = 0;
    if (!wait_eventfd(slot->copy_fd, "bitstream-readback", &wake_ns, timeouts))
        return false;
    if (wake_ns >= slot->pipeline_start_ns)
        slot_recycle_times->push_back(wake_ns - slot->pipeline_start_ns);

    if (slot->input_diagnostic_pending) {
        void *input_ptr = nullptr;
        const UINT64 input_size =
            static_cast<UINT64>(slot->input_footprint.Footprint.RowPitch) *
            slot->input_footprint.Footprint.Height;
        const D3D12_RANGE input_range = {0, input_size};
        if (!hr_ok(slot->input_readback->Map(0, &input_range, &input_ptr),
                   "mutter-diagnostic-readback-map"))
            return false;
        const auto *bytes = static_cast<const std::uint8_t *>(input_ptr) +
                            slot->input_footprint.Offset;
        const std::array<std::pair<UINT, UINT>, 3> points = {
            std::make_pair(0U, 0U),
            std::make_pair(kWidth / 2, kHeight / 2),
            std::make_pair(kWidth - 1, kHeight - 1)};
        bool frame_ok = true;
        for (const auto &point : points) {
            const std::size_t offset =
                static_cast<std::size_t>(point.second) *
                    slot->input_footprint.Footprint.RowPitch +
                static_cast<std::size_t>(point.first) * 4;
            if (!close_enough(bytes + offset, slot->expected_bgra))
                frame_ok = false;
        }
        slot->input_readback->Unmap(0, nullptr);
        ++*diagnostic_checks;
        if (!frame_ok) {
            ++*mismatches;
            std::fprintf(stderr,
                         "FAIL stage=diagnostic-frame-sequence frame=%llu\n",
                         static_cast<unsigned long long>(slot->frame));
        }
        slot->input_diagnostic_pending = false;
    }

    void *metadata_ptr = nullptr;
    const D3D12_RANGE read_range = {0, metadata_size};
    if (!hr_ok(slot->metadata_readback->Map(0, &read_range, &metadata_ptr),
               "d3d12-video-metadata-map"))
        return false;
    D3D12_VIDEO_ENCODER_OUTPUT_METADATA metadata = {};
    std::memcpy(&metadata, metadata_ptr,
                std::min<UINT64>(sizeof(metadata), metadata_size));
    slot->metadata_readback->Unmap(0, nullptr);
    if (metadata.EncodeErrorFlags != 0)
        ++*encode_failures;
    const UINT64 bytes = metadata.EncodedBitstreamWrittenBytesCount;
    if (bytes == 0 || bytes > bitstream_capacity) {
        ++*encode_failures;
        std::fprintf(stderr,
                     "FAIL stage=encoded-bitstream-size frame=%llu bytes=%llu\n",
                     static_cast<unsigned long long>(slot->frame),
                     static_cast<unsigned long long>(bytes));
    } else {
        void *bitstream_ptr = nullptr;
        const D3D12_RANGE bitstream_range = {0, bytes};
        if (!hr_ok(slot->bitstream_readback->Map(0, &bitstream_range,
                                                  &bitstream_ptr),
                   "d3d12-video-bitstream-map"))
            return false;
        stream->write(static_cast<const char *>(bitstream_ptr),
                      static_cast<std::streamsize>(bytes));
        slot->bitstream_readback->Unmap(0, nullptr);
        if (!*stream)
            return false;
        ++*encoded_frames;
        *encoded_bytes += bytes;
    }
    slot->copy_pending = false;
    return true;
}

static bool encode_consumer_main(int control_fd)
{
    DeviceContext context;
    if (!create_device("encoder-device", &context))
        return false;
    ResourceBundleMessage bundle = {};
    std::vector<int> received_fds;
    std::size_t bundle_size = 0;
    if (!receive_packet(control_fd, &bundle, sizeof(bundle), &bundle_size,
                        &received_fds, kSocketTimeoutMs) ||
        bundle_size != sizeof(bundle) || bundle.magic != kProtocolMagic ||
        bundle.type != kResourceBundle || bundle.width != kWidth ||
        bundle.height != kHeight || bundle.slots != kSlotCount ||
        bundle.frames != kFrameCount || received_fds.size() != kMaxTransferFds) {
        close_fd_vector(&received_fds);
        std::fputs("FAIL stage=cross-process-resource-fd-transfer\n", stderr);
        return false;
    }

    if (bundle.synthetic_source == 0) {
        if (bundle.format != kDxgiFormatB8G8R8A8Unorm ||
            bundle.buffer_count != kSlotCount) {
            std::fprintf(stderr,
                         "FAIL stage=mutter-real-render-target format=%u "
                         "buffer_count=%u\n",
                         bundle.format, bundle.buffer_count);
            return false;
        }
        std::printf("PASS stage=mutter-real-render-target synthetic_source=0 "
                    "width=%u height=%u format=BGRA8\n",
                    bundle.width, bundle.height);
        std::printf("PASS stage=mutter-shared-resource "
                    "native_d3d12_shared=1 gpu_copy=%u cpu_copy=0\n",
                    bundle.gpu_copy);
    }

    std::array<EncodeSlot, kSlotCount> slots;
    std::uint64_t reopen_failures = 0;
    for (std::uint32_t slot = 0; slot < kSlotCount; ++slot) {
        slots[slot].resource_fd = received_fds[slot * 2];
        slots[slot].ready_fd = received_fds[slot * 2 + 1];
        const HRESULT hr = context.device->OpenSharedHandle(
            reinterpret_cast<HANDLE>(
                static_cast<intptr_t>(slots[slot].resource_fd)),
            IID_PPV_ARGS(&slots[slot].texture));
        close_fd(&slots[slot].resource_fd);
        if (FAILED(hr)) {
            ++reopen_failures;
            std::fprintf(stderr,
                         "FAIL stage=open-shared-resource slot=%u HRESULT=0x%08x\n",
                         slot, static_cast<unsigned>(hr));
        }
    }
    received_fds.clear();
    if (reopen_failures != 0)
        return false;
    std::puts("PASS stage=cross-process-open-shared-resource slots=3 "
              "consumer_device=independent");
    std::puts("PASS stage=resource-transport-fd-close side=consumer count=3");

    EncoderSetup encoder;
    if (!initialize_encoder(&context, &encoder))
        return false;
    ProcessSetup process;
    if (!make_process_setup(&context, &process))
        return false;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    ComPtr<ID3D12CommandQueue> process_queue;
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS;
    if (!hr_ok(context.device->CreateCommandQueue(
                   &queue_desc, IID_PPV_ARGS(&process_queue)),
               "d3d12-video-process-queue"))
        return false;
    ComPtr<ID3D12CommandQueue> encode_queue;
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;
    if (!hr_ok(context.device->CreateCommandQueue(
                   &queue_desc, IID_PPV_ARGS(&encode_queue)),
               "d3d12-video-encode-queue"))
        return false;
    ComPtr<ID3D12CommandQueue> copy_queue;
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!hr_ok(context.device->CreateCommandQueue(
                   &queue_desc, IID_PPV_ARGS(&copy_queue)),
               "d3d12-video-copy-queue"))
        return false;
    ComPtr<ID3D12Fence> process_fence;
    ComPtr<ID3D12Fence> encode_fence;
    ComPtr<ID3D12Fence> copy_fence;
    if (!hr_ok(context.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                           IID_PPV_ARGS(&process_fence)),
               "d3d12-video-process-fence") ||
        !hr_ok(context.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                           IID_PPV_ARGS(&encode_fence)),
               "d3d12-video-encode-fence") ||
        !hr_ok(context.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                           IID_PPV_ARGS(&copy_fence)),
               "d3d12-video-copy-fence"))
        return false;
    const int process_eventfd = make_eventfd("d3d12-video-process-eventfd");
    if (process_eventfd < 0)
        return false;

    const D3D12_RESOURCE_DESC nv12_desc = {
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kWidth, kHeight, 1, 1,
        DXGI_FORMAT_NV12, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_NONE};
    const UINT64 metadata_size = align_up(
        encoder.metadata_bytes, 256);
    const UINT64 bitstream_size = align_up(
        std::max<UINT64>(kBitstreamCapacity, encoder.bitstream_alignment),
        encoder.bitstream_alignment);
    for (std::uint32_t slot_index = 0; slot_index < kSlotCount;
         ++slot_index) {
        EncodeSlot &slot = slots[slot_index];
        UINT64 input_readback_size = 0;
        const D3D12_RESOURCE_DESC input_desc = texture_desc();
        context.device->GetCopyableFootprints(
            &input_desc, 0, 1, 0, &slot.input_footprint, nullptr, nullptr,
            &input_readback_size);
        if (!create_default_resource(
                context.device.Get(), nv12_desc,
                D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE, &slot.nv12) ||
            !create_default_resource(
                context.device.Get(), buffer_desc(bitstream_size),
                D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, &slot.bitstream) ||
            !create_default_resource(
                context.device.Get(), buffer_desc(metadata_size),
                D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, &slot.metadata_hw) ||
            !create_default_resource(
                context.device.Get(), buffer_desc(metadata_size),
                D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE,
                &slot.metadata_resolved) ||
            !create_readback_resource(context.device.Get(), bitstream_size,
                                      &slot.bitstream_readback) ||
            !create_readback_resource(context.device.Get(), metadata_size,
                                      &slot.metadata_readback) ||
            !create_readback_resource(context.device.Get(), input_readback_size,
                                      &slot.input_readback))
            return false;
        if (!hr_ok(context.device->CreateCommandAllocator(
                       D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS,
                       IID_PPV_ARGS(&slot.process_allocator)),
                   "d3d12-video-process-allocator") ||
            !hr_ok(context.device->CreateCommandList(
                       0, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS,
                       slot.process_allocator.Get(), nullptr,
                       IID_PPV_ARGS(&slot.process_list)),
                   "d3d12-video-process-list") ||
            !hr_ok(slot.process_list->Close(), "d3d12-video-process-close") ||
            !hr_ok(context.device->CreateCommandAllocator(
                       D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
                       IID_PPV_ARGS(&slot.encode_allocator)),
                   "d3d12-video-encode-allocator") ||
            !hr_ok(context.device->CreateCommandList(
                       0, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
                       slot.encode_allocator.Get(), nullptr,
                       IID_PPV_ARGS(&slot.encode_list)),
                   "d3d12-video-encode-list") ||
            !hr_ok(slot.encode_list->Close(), "d3d12-video-encode-close") ||
            !hr_ok(context.device->CreateCommandAllocator(
                       D3D12_COMMAND_LIST_TYPE_DIRECT,
                       IID_PPV_ARGS(&slot.copy_allocator)),
                   "d3d12-video-copy-allocator") ||
            !hr_ok(context.device->CreateCommandList(
                       0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                       slot.copy_allocator.Get(), nullptr,
                       IID_PPV_ARGS(&slot.copy_list)),
                   "d3d12-video-copy-list") ||
            !hr_ok(slot.copy_list->Close(), "d3d12-video-copy-close"))
            return false;
        slot.copy_fd = make_eventfd("d3d12-video-copy-eventfd");
        slot.encode_fd = make_eventfd("d3d12-video-encode-eventfd");
        if (slot.copy_fd < 0 || slot.encode_fd < 0)
            return false;
    }

    std::vector<int> done_transfer_fds;
    for (std::uint32_t slot = 0; slot < kSlotCount; ++slot) {
        slots[slot].done_fd = make_eventfd("consumer-done-eventfd");
        if (slots[slot].done_fd < 0)
            return false;
        done_transfer_fds.push_back(slots[slot].done_fd);
    }
    const ConsumerReadyMessage ready = {
        kProtocolMagic, kConsumerReady, 0, kSlotCount, kSlotCount};
    if (!send_packet(control_fd, &ready, sizeof(ready), done_transfer_fds))
        return false;
    std::puts("PASS stage=consumer-done-eventfd slots=3 "
              "set_event_on_completion=1 scm_rights=1");
    std::puts("PASS stage=consumer-gpu-operation bgra_to_nv12=GPU-only "
              "d3d12_encode=GPU-only cpu_framebuffer_copy=0");

    const char *path = std::getenv("D3D12_VIDEO_BITSTREAM_PATH");
    const char *stream_path = path != nullptr ? path
                                                : "/tmp/d3d12-video-probe.hevc";
    std::ofstream stream(stream_path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return false;
    stream.write(reinterpret_cast<const char *>(kHevcSequenceHeaders),
                 sizeof(kHevcSequenceHeaders));
    if (!stream)
        return false;
    std::printf("PASS stage=hevc-sequence-headers vps=1 sps=1 pps=1 bytes=%zu "
                "host_generated=1 framebuffer_bytes=0\n",
                sizeof(kHevcSequenceHeaders));
    std::vector<std::uint64_t> wake_latencies;
    std::vector<std::uint64_t> conversion_times;
    std::vector<std::uint64_t> encode_submit_times;
    std::vector<std::uint64_t> encode_complete_times;
    std::vector<std::uint64_t> total_pipeline_times;
    std::vector<std::uint64_t> slot_recycle_times;
    std::uint64_t timeouts = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t diagnostic_checks = 0;
    std::uint64_t encoded_frames = 0;
    std::uint64_t encoded_bytes = 0;
    std::uint64_t encode_failures = 0;
    bool reuse_logged = false;
    const std::uint64_t consumer_start_ns = monotonic_ns();

    for (std::uint64_t frame = 0; frame < kFrameCount; ++frame) {
        FrameInfoMessage info = {};
        std::vector<int> unexpected_fds;
        std::size_t info_size = 0;
        if (!receive_packet(control_fd, &info, sizeof(info), &info_size,
                            &unexpected_fds, kSocketTimeoutMs) ||
            info_size != sizeof(info) || info.magic != kProtocolMagic ||
            info.type != kFrameInfo || info.frame != frame ||
            info.slot >= kSlotCount || !unexpected_fds.empty()) {
            close_fd_vector(&unexpected_fds);
            return false;
        }
        close_fd_vector(&unexpected_fds);
        EncodeSlot &slot = slots[info.slot];
        if (frame >= kSlotCount) {
            if (!collect_encoded_slot(&slot, &stream, bitstream_size,
                                      metadata_size, &encoded_frames,
                                      &encoded_bytes, &encode_failures,
                                      &timeouts, &mismatches,
                                      &diagnostic_checks,
                                      &encode_complete_times,
                                      &total_pipeline_times,
                                      &slot_recycle_times))
                return false;
            if (!reuse_logged) {
                std::puts("PASS stage=triple-buffer-reuse slots=3");
                reuse_logged = true;
            }
        }
        std::uint64_t ready_ns = 0;
        if (!wait_eventfd(slot.ready_fd, "consumer-ready-eventfd", &ready_ns,
                          &timeouts))
            return false;
        if (ready_ns >= info.producer_signal_ns)
            wake_latencies.push_back(ready_ns - info.producer_signal_ns);
        const std::uint64_t pipeline_start_ns = ready_ns;

        if (!hr_ok(slot.process_allocator->Reset(),
                   "d3d12-video-process-allocator-reset") ||
            !hr_ok(slot.process_list->Reset(slot.process_allocator.Get()),
                   "d3d12-video-process-list-reset"))
            return false;
        D3D12_RESOURCE_BARRIER process_barrier = {};
        process_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        process_barrier.Transition.pResource = slot.texture.Get();
        process_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        process_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        process_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ;
        slot.process_list->ResourceBarrier(1, &process_barrier);
        if (!slot.nv12_first_use) {
            process_barrier.Transition.pResource = slot.nv12.Get();
            process_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            process_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE;
            slot.process_list->ResourceBarrier(1, &process_barrier);
        }
        D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS process_input = {};
        process_input.InputStream[0].pTexture2D = slot.texture.Get();
        process_input.InputStream[0].Subresource = 0;
        process_input.Transform.SourceRectangle = {0, 0,
                                                   static_cast<LONG>(kWidth),
                                                   static_cast<LONG>(kHeight)};
        process_input.Transform.DestinationRectangle = process_input.Transform.SourceRectangle;
        process_input.Transform.Orientation = D3D12_VIDEO_PROCESS_ORIENTATION_DEFAULT;
        process_input.RateInfo.OutputIndex = 0;
        process_input.RateInfo.InputFrameOrField = 0;
        D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS process_output = {};
        process_output.OutputStream[0].pTexture2D = slot.nv12.Get();
        process_output.OutputStream[0].Subresource = 0;
        process_output.TargetRectangle = process_input.Transform.DestinationRectangle;
        slot.process_list->ProcessFrames(process.processor.Get(), &process_output,
                                         1, &process_input);
        process_barrier.Transition.pResource = slot.texture.Get();
        process_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ;
        process_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        slot.process_list->ResourceBarrier(1, &process_barrier);
        process_barrier.Transition.pResource = slot.nv12.Get();
        process_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE;
        process_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        slot.process_list->ResourceBarrier(1, &process_barrier);
        if (!hr_ok(slot.process_list->Close(), "d3d12-video-process-list-close"))
            return false;
        ID3D12CommandList *process_lists[] = {slot.process_list.Get()};
        const UINT64 process_value = frame + 1;
        process_queue->ExecuteCommandLists(1, process_lists);
        if (!hr_ok(process_queue->Signal(process_fence.Get(), process_value),
                   "d3d12-video-process-signal") ||
            !register_event(process_fence.Get(), process_value, process_eventfd,
                            "d3d12-video-process-eventfd"))
            return false;
        const std::uint64_t process_submitted_ns = monotonic_ns();
        std::uint64_t process_done_ns = 0;
        if (!wait_eventfd(process_eventfd, "d3d12-video-process", &process_done_ns,
                          &timeouts))
            return false;
        if (process_done_ns >= process_submitted_ns)
            conversion_times.push_back(process_done_ns - process_submitted_ns);

        if (!hr_ok(slot.encode_allocator->Reset(),
                   "d3d12-video-encode-allocator-reset") ||
            !hr_ok(slot.encode_list->Reset(slot.encode_allocator.Get()),
                   "d3d12-video-encode-list-reset"))
            return false;
        D3D12_RESOURCE_BARRIER encode_barrier = {};
        encode_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        encode_barrier.Transition.pResource = slot.nv12.Get();
        encode_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        encode_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        encode_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
        slot.encode_list->ResourceBarrier(1, &encode_barrier);
        if (slot.output_initialized) {
            encode_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            encode_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
            encode_barrier.Transition.pResource = slot.bitstream.Get();
            slot.encode_list->ResourceBarrier(1, &encode_barrier);
            encode_barrier.Transition.pResource = slot.metadata_hw.Get();
            slot.encode_list->ResourceBarrier(1, &encode_barrier);
            encode_barrier.Transition.pResource = slot.metadata_resolved.Get();
            slot.encode_list->ResourceBarrier(1, &encode_barrier);
        }
        D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_DESC sequence = {};
        // The first frame establishes the sequence.  These flags describe a
        // reconfiguration relative to an already active sequence, so they
        // must remain clear for the initial EncodeFrame call.
        sequence.Flags = D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_NONE;
        sequence.RateControl = encoder.rate_control;
        sequence.PictureTargetResolution = {kWidth, kHeight};
        sequence.SelectedLayoutMode =
            D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
        sequence.CodecGopSequence = encoder.gop;
        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC picture_data = {};
        picture_data.FrameType = D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_I_FRAME;
        picture_data.PictureOrderCountNumber = static_cast<UINT>(frame);
        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_DESC picture = {};
        picture.PictureControlCodecData.DataSize = sizeof(picture_data);
        picture.PictureControlCodecData.pHEVCPicData = &picture_data;
        D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS input = {};
        input.SequenceControlDesc = sequence;
        input.PictureControlDesc = picture;
        input.pInputFrame = slot.nv12.Get();
        D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS output = {};
        output.Bitstream.pBuffer = slot.bitstream.Get();
        output.Bitstream.FrameStartOffset = 0;
        output.EncoderOutputMetadata.pBuffer = slot.metadata_hw.Get();
        output.EncoderOutputMetadata.Offset = 0;
        slot.encode_list->EncodeFrame(encoder.encoder.Get(), encoder.heap.Get(),
                                      &input, &output);
        D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS resolve_input = {};
        resolve_input.EncoderCodec = kEncodeCodec;
        resolve_input.EncoderProfile = encoder.profile;
        resolve_input.EncoderInputFormat = DXGI_FORMAT_NV12;
        resolve_input.EncodedPictureEffectiveResolution = {kWidth, kHeight};
        resolve_input.HWLayoutMetadata = output.EncoderOutputMetadata;
        D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS resolve_output = {};
        resolve_output.ResolvedLayoutMetadata.pBuffer = slot.metadata_resolved.Get();
        slot.encode_list->ResolveEncoderOutputMetadata(&resolve_input,
                                                       &resolve_output);
        encode_barrier.Transition.pResource = slot.nv12.Get();
        encode_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
        encode_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        slot.encode_list->ResourceBarrier(1, &encode_barrier);
        encode_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
        encode_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        encode_barrier.Transition.pResource = slot.bitstream.Get();
        slot.encode_list->ResourceBarrier(1, &encode_barrier);
        encode_barrier.Transition.pResource = slot.metadata_hw.Get();
        slot.encode_list->ResourceBarrier(1, &encode_barrier);
        encode_barrier.Transition.pResource = slot.metadata_resolved.Get();
        slot.encode_list->ResourceBarrier(1, &encode_barrier);
        if (!hr_ok(slot.encode_list->Close(), "d3d12-video-encode-list-close"))
            return false;
        ID3D12CommandList *encode_lists[] = {slot.encode_list.Get()};
        const std::uint64_t encode_submit_ns = monotonic_ns();
        encode_queue->Wait(process_fence.Get(), process_value);
        encode_queue->ExecuteCommandLists(1, encode_lists);
        const std::uint64_t encode_submit_done_ns = monotonic_ns();
        if (encode_submit_done_ns >= encode_submit_ns)
            encode_submit_times.push_back(encode_submit_done_ns - encode_submit_ns);
        const UINT64 encode_value = frame + 1;
        if (!hr_ok(encode_queue->Signal(encode_fence.Get(), encode_value),
                   "d3d12-video-encode-signal") ||
            !register_event(encode_fence.Get(), encode_value, slot.encode_fd,
                            "d3d12-video-encode-eventfd"))
            return false;
        slot.encode_submit_ns = encode_submit_ns;
        slot.pipeline_start_ns = pipeline_start_ns;

        if (!hr_ok(slot.copy_allocator->Reset(),
                   "d3d12-video-copy-allocator-reset") ||
            !hr_ok(slot.copy_list->Reset(slot.copy_allocator.Get(), nullptr),
                   "d3d12-video-copy-list-reset"))
            return false;
        D3D12_RESOURCE_BARRIER copy_barrier = {};
        copy_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copy_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copy_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        copy_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copy_barrier.Transition.pResource = slot.bitstream.Get();
        slot.copy_list->ResourceBarrier(1, &copy_barrier);
        copy_barrier.Transition.pResource = slot.metadata_resolved.Get();
        slot.copy_list->ResourceBarrier(1, &copy_barrier);
        slot.copy_list->CopyBufferRegion(slot.bitstream_readback.Get(), 0,
                                         slot.bitstream.Get(), 0,
                                         bitstream_size);
        slot.copy_list->CopyBufferRegion(slot.metadata_readback.Get(), 0,
                                         slot.metadata_resolved.Get(), 0,
                                         metadata_size);
        if (info.diagnostic != 0) {
            D3D12_RESOURCE_BARRIER input_barrier = {};
            input_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            input_barrier.Transition.pResource = slot.texture.Get();
            input_barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            input_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            input_barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_COPY_SOURCE;
            slot.copy_list->ResourceBarrier(1, &input_barrier);
            D3D12_TEXTURE_COPY_LOCATION input_source = {};
            input_source.pResource = slot.texture.Get();
            input_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            input_source.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION input_destination = {};
            input_destination.pResource = slot.input_readback.Get();
            input_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            input_destination.PlacedFootprint = slot.input_footprint;
            slot.copy_list->CopyTextureRegion(&input_destination, 0, 0, 0,
                                              &input_source, nullptr);
            input_barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_COPY_SOURCE;
            input_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            slot.copy_list->ResourceBarrier(1, &input_barrier);
        }
        copy_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copy_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        copy_barrier.Transition.pResource = slot.bitstream.Get();
        slot.copy_list->ResourceBarrier(1, &copy_barrier);
        copy_barrier.Transition.pResource = slot.metadata_resolved.Get();
        slot.copy_list->ResourceBarrier(1, &copy_barrier);
        if (!hr_ok(slot.copy_list->Close(), "d3d12-video-copy-list-close"))
            return false;
        ID3D12CommandList *copy_lists[] = {slot.copy_list.Get()};
        copy_queue->Wait(encode_fence.Get(), encode_value);
        copy_queue->ExecuteCommandLists(1, copy_lists);
        slot.copy_value = frame + 1;
        if (!hr_ok(copy_queue->Signal(copy_fence.Get(), slot.copy_value),
                   "d3d12-video-copy-signal") ||
            !register_event(copy_fence.Get(), slot.copy_value, slot.done_fd,
                            "consumer-done-eventfd") ||
            !register_event(copy_fence.Get(), slot.copy_value, slot.copy_fd,
                            "d3d12-video-copy-eventfd"))
            return false;
        slot.copy_pending = true;
        slot.output_initialized = true;
        slot.frame = frame;
        if (info.diagnostic != 0) {
            if (info.expected_valid != 0) {
                std::copy(std::begin(info.expected_bgra),
                          std::end(info.expected_bgra),
                          slot.expected_bgra.begin());
            } else {
                slot.expected_bgra = expected_bgra(frame);
            }
            slot.input_diagnostic_pending = true;
        }
        slot.nv12_first_use = false;
    }
    for (EncodeSlot &slot : slots) {
        if (!collect_encoded_slot(&slot, &stream, bitstream_size, metadata_size,
                                  &encoded_frames, &encoded_bytes,
                                  &encode_failures, &timeouts, &mismatches,
                                  &diagnostic_checks,
                                  &encode_complete_times,
                                  &total_pipeline_times,
                                  &slot_recycle_times))
            return false;
    }
    stream.close();
    const std::uint64_t consumer_end_ns = monotonic_ns();
    const double consumer_elapsed_seconds =
        static_cast<double>(consumer_end_ns - consumer_start_ns) / 1000000000.0;
    const double consumer_fps =
        consumer_elapsed_seconds > 0.0
            ? static_cast<double>(kFrameCount) / consumer_elapsed_seconds
            : 0.0;
    std::printf("bgra_to_nv12_us p50=%.3f p95=%.3f p99=%.3f samples=%zu\n",
                percentile_us(conversion_times, 0.50),
                percentile_us(conversion_times, 0.95),
                percentile_us(conversion_times, 0.99), conversion_times.size());
    std::printf("encode_submit_us p50=%.3f p95=%.3f p99=%.3f samples=%zu\n",
                percentile_us(encode_submit_times, 0.50),
                percentile_us(encode_submit_times, 0.95),
                percentile_us(encode_submit_times, 0.99),
                encode_submit_times.size());
    std::printf("encode_complete_us p50=%.3f p95=%.3f p99=%.3f samples=%zu\n",
                percentile_us(encode_complete_times, 0.50),
                percentile_us(encode_complete_times, 0.95),
                percentile_us(encode_complete_times, 0.99),
                encode_complete_times.size());
    std::printf("total_consumer_pipeline_us p50=%.3f p95=%.3f p99=%.3f samples=%zu\n",
                percentile_us(total_pipeline_times, 0.50),
                percentile_us(total_pipeline_times, 0.95),
                percentile_us(total_pipeline_times, 0.99),
                total_pipeline_times.size());
    std::printf("encoded_frames=%llu encoded_bytes=%llu encode_failures=%llu "
                "bitstream=%s\n",
                static_cast<unsigned long long>(encoded_frames),
                static_cast<unsigned long long>(encoded_bytes),
                static_cast<unsigned long long>(encode_failures), stream_path);
    std::printf("wake_latency_us p50=%.3f p95=%.3f p99=%.3f samples=%zu\n",
                percentile_us(wake_latencies, 0.50),
                percentile_us(wake_latencies, 0.95),
                percentile_us(wake_latencies, 0.99), wake_latencies.size());
    std::printf("mutter_publish_to_consumer_wake_us p50=%.3f p95=%.3f p99=%.3f samples=%zu\n",
                percentile_us(wake_latencies, 0.50),
                percentile_us(wake_latencies, 0.95),
                percentile_us(wake_latencies, 0.99), wake_latencies.size());
    std::printf("slot_recycle_us p50=%.3f p95=%.3f p99=%.3f samples=%zu\n",
                percentile_us(slot_recycle_times, 0.50),
                percentile_us(slot_recycle_times, 0.95),
                percentile_us(slot_recycle_times, 0.99),
                slot_recycle_times.size());
    std::printf("frames=%u fps=%.2f stale_frames=0 dropped_frames=0 "
                "repeated_frames=0 compositor_frame_misses=0\n",
                kFrameCount, consumer_fps);
    const bool fps_ok = consumer_fps >= 59.0 && consumer_fps <= 61.0;
    const bool ok = timeouts == 0 && mismatches == 0 && reopen_failures == 0 &&
                    encode_failures == 0 && encoded_frames == kFrameCount &&
                    diagnostic_checks == kFrameCount / kDiagnosticInterval &&
                    fps_ok;
    std::printf("%s stage=diagnostic-frame-sequence mismatches=%llu checks=%llu\n",
                mismatches == 0 ? "PASS" : "FAIL",
                static_cast<unsigned long long>(mismatches),
                static_cast<unsigned long long>(diagnostic_checks));
    if (bundle.synthetic_source == 0) {
        std::printf("%s stage=consumer-real-desktop-frame "
                    "diagnostic_points=3 stale_frames=0 mismatches=%llu\n",
                    mismatches == 0 ? "PASS" : "FAIL",
                    static_cast<unsigned long long>(mismatches));
        std::printf("%s stage=producer-eventfd-sync busy_poll=0 "
                    "set_event_on_completion=1 eventfd=1 poll=1\n",
                    timeouts == 0 ? "PASS" : "FAIL");
        std::printf("%s stage=4k60-sustained frames=%u fps=%.2f "
                    "timeouts=%llu mismatches=%llu encode_failures=%llu\n",
                    ok ? "PASS" : "FAIL", kFrameCount, consumer_fps,
                    static_cast<unsigned long long>(timeouts),
                    static_cast<unsigned long long>(mismatches),
                    static_cast<unsigned long long>(encode_failures));
        std::printf("%s stage=d3d12-hardware-encode frames=%llu\n",
                    encode_failures == 0 && encoded_frames == kFrameCount
                        ? "PASS"
                        : "FAIL",
                    static_cast<unsigned long long>(encoded_frames));
        std::printf("%s stage=throughput-zero-copy mmap_framebuffer=0 "
                    "cpu_memcpy_framebuffer=0 gpu_cpu_gpu=0\n",
                    ok ? "PASS" : "FAIL");
    }
    ConsumerResultMessage result = {
        kProtocolMagic, kConsumerResult, ok ? 0U : 1U, 0, kFrameCount,
        timeouts, mismatches, diagnostic_checks, reopen_failures};
    if (!send_packet(control_fd, &result, sizeof(result), {}))
        return false;
    close(process_eventfd);
    for (EncodeSlot &slot : slots) {
        close_fd(&slot.ready_fd);
        close_fd(&slot.done_fd);
        close_fd(&slot.encode_fd);
        close_fd(&slot.copy_fd);
    }
    return ok;
}

static void encode_exec_role(const char *self, const char *role, int control_fd)
{
    char fd_text[32] = {};
    std::snprintf(fd_text, sizeof(fd_text), "%d", control_fd);
    (void)set_fd_cloexec(control_fd, false);
    execl(self, self, role, fd_text, static_cast<char *>(nullptr));
    std::fprintf(stderr, "FAIL stage=exec-%s errno=%d (%s)\n", role, errno,
                 std::strerror(errno));
    _exit(127);
}

[[maybe_unused]] static int launch_encode_children(const char *self)
{
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0)
        return 1;
    const pid_t consumer_pid = fork();
    if (consumer_pid == 0) {
        close(sockets[0]);
        encode_exec_role(self, "--consumer", sockets[1]);
    }
    if (consumer_pid < 0)
        return 1;
    const pid_t producer_pid = fork();
    if (producer_pid == 0) {
        close(sockets[1]);
        encode_exec_role(self, "--producer", sockets[0]);
    }
    if (producer_pid < 0)
        return 1;
    close(sockets[0]);
    close(sockets[1]);
    std::printf("PASS stage=independent-processes producer_pid=%ld consumer_pid=%ld "
                "exec=1 inherited_d3d12_device=0\n",
                static_cast<long>(producer_pid),
                static_cast<long>(consumer_pid));
    int producer_status = 0;
    int consumer_status = 0;
    (void)waitpid(producer_pid, &producer_status, 0);
    (void)waitpid(consumer_pid, &consumer_status, 0);
    const bool producer_ok = WIFEXITED(producer_status) &&
                             WEXITSTATUS(producer_status) == 0;
    const bool consumer_ok = WIFEXITED(consumer_status) &&
                             WEXITSTATUS(consumer_status) == 0;
    if (!producer_ok || !consumer_ok) {
        std::fprintf(stderr,
                     "FAIL stage=child-exit producer_status=%d consumer_status=%d\n",
                     producer_status, consumer_status);
        return 1;
    }
    std::puts("PASS stage=4k60-sustained");
    std::puts("PASS stage=throughput-zero-copy mmap_framebuffer=0 "
              "cpu_memcpy_framebuffer=0 gpu_cpu_gpu=0");
    std::puts("PASS d3d12-shared-texture-hardware-encode-payload");
    return 0;
}

[[maybe_unused]] static int capability_main()
{
    DeviceContext context;
    if (!create_device("capability", &context))
        return 1;
    ComPtr<ID3D12VideoDevice3> video_device;
    const HRESULT interface_hr = context.device.As(&video_device);
    if (FAILED(interface_hr)) {
        std::fprintf(stderr,
                     "BLOCKED stage=d3d12-video-device-interface HRESULT=0x%08x\n",
                     static_cast<unsigned>(interface_hr));
        return 3;
    }
    std::puts("PASS stage=d3d12-video-device-interface version=3");
    if (!probe_codec(video_device.Get(), D3D12_VIDEO_ENCODER_CODEC_H264) ||
        !probe_codec(video_device.Get(), D3D12_VIDEO_ENCODER_CODEC_HEVC))
        return 1;
    std::puts("codec_matrix h264=1 hevc=1");
    EncoderSetup setup;
    if (!initialize_encoder(&context, &setup))
        return 3;
    std::puts("PASS d3d12-video-encode-capability codec=HEVC "
              "resolution=3840x2160 input=NV12 fps=60/1");
    return 0;
}

} // namespace

#ifndef ASB_D3D12_VIDEO_NO_MAIN
int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    (void)signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && std::strcmp(argv[1], "--capability") == 0)
        return capability_main();
    if (argc == 3 && std::strcmp(argv[1], "--producer") == 0)
        return producer_main(std::atoi(argv[2])) ? 0 : 1;
    if (argc == 3 && std::strcmp(argv[1], "--consumer") == 0)
        return encode_consumer_main(std::atoi(argv[2])) ? 0 : 1;
    if (argc == 1)
        return launch_encode_children(argv[0]);
    std::fprintf(stderr, "Usage: %s [--capability]\n", argv[0]);
    return 2;
}
#endif
