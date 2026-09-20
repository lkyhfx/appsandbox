#pragma once

/*
 * The Windows 10.0.26100 SDK shipped on the build host predates the public
 * HEVC1 additions in DirectX-Headers.  The D3D12 video ABI deliberately uses
 * pointers in its extensible unions, so the missing HEVC1 records can be
 * described locally without changing the size of any D3D12 feature query.
 *
 * These definitions mirror DirectX-Headers/include/directx/d3d12video.h:
 * D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC1 and
 * D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1.  The compatibility
 * overlay is only used for the Host probe; production does not infer a
 * capability from this header alone.
 */

#include <d3d12video.h>

#include <cstdint>
#include <cstring>

namespace appsandbox_d3d12_hevc1 {

using D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAGS1 =
    std::uint32_t;

struct D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC1 {
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAGS SupportFlags;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE MinLumaCodingUnitSize;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE MaxLumaCodingUnitSize;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE MinLumaTransformUnitSize;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE MaxLumaTransformUnitSize;
    UCHAR max_transform_hierarchy_depth_inter;
    UCHAR max_transform_hierarchy_depth_intra;
    UINT allowed_diff_cu_chroma_qp_offset_depth_values;
    UINT allowed_log2_sao_offset_scale_luma_values;
    UINT allowed_log2_sao_offset_scale_chroma_values;
    UINT allowed_log2_max_transform_skip_block_size_minus2_values;
    UINT allowed_chroma_qp_offset_list_len_minus1_values;
    UINT allowed_cb_qp_offset_list_values[6];
    UINT allowed_cr_qp_offset_list_values[6];
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAGS1 SupportFlags1;
};

struct D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT {
    UINT DataSize;
    union {
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC1 *pHEVCSupport1;
        void *pOpaque;
    };
};

struct D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1 {
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC_FLAGS Flags;
    D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC FrameType;
    UINT slice_pic_parameter_set_id;
    UINT PictureOrderCountNumber;
    UINT TemporalLayerIndex;
    UINT List0ReferenceFramesCount;
    UINT *pList0ReferenceFrames;
    UINT List1ReferenceFramesCount;
    UINT *pList1ReferenceFrames;
    UINT ReferenceFramesReconPictureDescriptorsCount;
    D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_HEVC *
        pReferenceFramesReconPictureDescriptors;
    UINT List0RefPicModificationsCount;
    UINT *pList0RefPicModifications;
    UINT List1RefPicModificationsCount;
    UINT *pList1RefPicModifications;
    UINT QPMapValuesCount;
    INT8 *pRateControlQPMap;
    UCHAR diff_cu_chroma_qp_offset_depth;
    UCHAR log2_sao_offset_scale_luma;
    UCHAR log2_sao_offset_scale_chroma;
    UCHAR log2_max_transform_skip_block_size_minus2;
    UCHAR chroma_qp_offset_list_len_minus1;
    CHAR cb_qp_offset_list[6];
    CHAR cr_qp_offset_list[6];
};

struct D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA {
    UINT DataSize;
    union {
        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1 *pHEVCPicData1;
        void *pOpaque;
    };
};

/* Values added to the HEVC configuration flags after the original SDK. */
constexpr std::uint32_t kSupportTransformSkipRotationRequired = 0x00000800u;
constexpr std::uint32_t kSupportTransformSkipContextRequired = 0x00002000u;
constexpr std::uint32_t kSupportImplicitRdpcmRequired = 0x00008000u;
constexpr std::uint32_t kSupportExplicitRdpcmRequired = 0x00020000u;
constexpr std::uint32_t kSupportExtendedPrecisionRequired = 0x00080000u;
constexpr std::uint32_t kSupportIntraSmoothingDisabledRequired = 0x00200000u;
constexpr std::uint32_t kSupportHighPrecisionOffsetsRequired = 0x00800000u;
constexpr std::uint32_t kSupportPersistentRiceRequired = 0x02000000u;
constexpr std::uint32_t kSupportCabacBypassAlignmentRequired = 0x08000000u;
constexpr std::uint32_t kSupportCrossComponentPredictionRequired = 0x20000000u;
constexpr std::uint32_t kSupportChromaQpOffsetListRequired = 0x80000000u;
constexpr std::uint32_t kSupport1SeparateColourPlaneRequired = 0x2u;
constexpr std::uint32_t kSupport1TemporalMvpRequired = 0x8u;
constexpr std::uint32_t kSupport1StrongIntraSmoothingRequired = 0x20u;

inline void install_hevc1_support(
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT *query,
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC1 *support)
{
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT compat = {};
    compat.DataSize = sizeof(*support);
    compat.pHEVCSupport1 = support;
    static_assert(sizeof(compat) == sizeof(query->CodecSupportLimits),
                  "D3D12 codec support union ABI changed");
    std::memcpy(&query->CodecSupportLimits, &compat, sizeof(compat));
}

inline void install_hevc1_picture_control(
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_DESC *picture,
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1 *data)
{
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA compat = {};
    compat.DataSize = sizeof(*data);
    compat.pHEVCPicData1 = data;
    static_assert(sizeof(compat) == sizeof(picture->PictureControlCodecData),
                  "D3D12 picture-control union ABI changed");
    std::memcpy(&picture->PictureControlCodecData, &compat, sizeof(compat));
}

} // namespace appsandbox_d3d12_hevc1
