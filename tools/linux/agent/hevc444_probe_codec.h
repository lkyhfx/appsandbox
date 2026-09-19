#pragma once

/*
 * Probe-only, dependency-free HEVC Main 4:4:4 parameter-set builder.
 *
 * The D3D12 probe adapts its native HEVC configuration structures to this
 * small value model. Keeping the bitstream code here makes it possible to
 * exercise the real SPS/PPS syntax in a CPU-only test without a D3D12
 * runtime, a GPU, or the WSL video libraries.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace appsandbox_hevc444_probe {

enum ConfigFlags : std::uint32_t {
    kConfigUseAsymmetricMotionPartition = 1u << 4,
    kConfigEnableTransformSkipping = 1u << 5,
    kConfigTransformSkipRotation = 1u << 7,
    kConfigTransformSkipContext = 1u << 8,
    kConfigImplicitRdpcm = 1u << 9,
    kConfigExplicitRdpcm = 1u << 10,
    kConfigExtendedPrecisionProcessing = 1u << 11,
    kConfigIntraSmoothingDisabled = 1u << 12,
    kConfigHighPrecisionOffsets = 1u << 13,
    kConfigPersistentRiceAdaptation = 1u << 14,
    kConfigCabacBypassAlignment = 1u << 15,
    kConfigSeparateColourPlane = 1u << 16,
    kConfigTemporalMvpEnabled = 1u << 17,
    kConfigStrongIntraSmoothingEnabled = 1u << 18,
    kConfigEnableSaoFilter = 1u << 2,
    kConfigUseConstrainedIntraprediction = 1u << 6,
};

enum PictureFlags : std::uint32_t {
    kPictureCrossComponentPrediction = 1u << 2,
    kPictureChromaQpOffsetList = 1u << 3,
};

enum RequiredSupportFlags : std::uint32_t {
    kRequiredAsymmetricMotionPartition = 1u << 5,
    kRequiredTransformSkipRotation = 1u << 11,
    kRequiredTransformSkipContext = 1u << 13,
    kRequiredImplicitRdpcm = 1u << 15,
    kRequiredExplicitRdpcm = 1u << 17,
    kRequiredExtendedPrecisionProcessing = 1u << 19,
    kRequiredIntraSmoothingDisabled = 1u << 21,
    kRequiredHighPrecisionOffsets = 1u << 23,
    kRequiredPersistentRiceAdaptation = 1u << 25,
    kRequiredCabacBypassAlignment = 1u << 27,
    kRequiredCrossComponentPrediction = 1u << 29,
    kRequiredChromaQpOffsetList = 1u << 31,
};

enum RequiredSupportFlags1 : std::uint32_t {
    kRequiredSeparateColourPlane = 1u << 1,
    kRequiredTemporalMvp = 1u << 3,
    kRequiredStrongIntraSmoothing = 1u << 5,
};

inline bool apply_required_configuration_flags(
    std::uint32_t support_flags, std::uint32_t support_flags1,
    std::uint32_t *configuration_flags, std::uint32_t *picture_flags)
{
    if (!configuration_flags || !picture_flags)
        return false;
    /* SupportFlags also contains the non-required *_SUPPORT bits. The
     * current DirectX-Headers SupportFlags1 values occupy the low six bits. */
    constexpr std::uint32_t supported_flags1 = 0x3fu;
    if ((support_flags1 & ~supported_flags1) != 0)
        return false;
    *configuration_flags = 0;
    *picture_flags = 0;
    if (support_flags & kRequiredAsymmetricMotionPartition)
        *configuration_flags |= kConfigUseAsymmetricMotionPartition;
    if (support_flags & kRequiredTransformSkipRotation)
        *configuration_flags |= kConfigTransformSkipRotation;
    if (support_flags & kRequiredTransformSkipContext)
        *configuration_flags |= kConfigTransformSkipContext;
    if (support_flags & kRequiredImplicitRdpcm)
        *configuration_flags |= kConfigImplicitRdpcm;
    if (support_flags & kRequiredExplicitRdpcm)
        *configuration_flags |= kConfigExplicitRdpcm;
    if (support_flags & kRequiredExtendedPrecisionProcessing)
        *configuration_flags |= kConfigExtendedPrecisionProcessing;
    if (support_flags & kRequiredIntraSmoothingDisabled)
        *configuration_flags |= kConfigIntraSmoothingDisabled;
    if (support_flags & kRequiredHighPrecisionOffsets)
        *configuration_flags |= kConfigHighPrecisionOffsets;
    if (support_flags & kRequiredPersistentRiceAdaptation)
        *configuration_flags |= kConfigPersistentRiceAdaptation;
    if (support_flags & kRequiredCabacBypassAlignment)
        *configuration_flags |= kConfigCabacBypassAlignment;
    if (support_flags & kRequiredCrossComponentPrediction)
        *picture_flags |= kPictureCrossComponentPrediction;
    if (support_flags & kRequiredChromaQpOffsetList)
        *picture_flags |= kPictureChromaQpOffsetList;
    if (support_flags1 & kRequiredSeparateColourPlane)
        *configuration_flags |= kConfigSeparateColourPlane;
    if (support_flags1 & kRequiredTemporalMvp)
        *configuration_flags |= kConfigTemporalMvpEnabled;
    if (support_flags1 & kRequiredStrongIntraSmoothing)
        *configuration_flags |= kConfigStrongIntraSmoothingEnabled;
    return true;
}

struct PictureDefaults {
    std::uint32_t flags = 0;
    std::uint8_t diff_cu_chroma_qp_offset_depth = 0;
    std::uint8_t log2_sao_offset_scale_luma = 0;
    std::uint8_t log2_sao_offset_scale_chroma = 0;
    std::uint8_t log2_max_transform_skip_block_size_minus2 = 0;
    std::uint8_t chroma_qp_offset_list_len_minus1 = 0;
    std::array<std::int8_t, 6> cb_qp_offset_list = {};
    std::array<std::int8_t, 6> cr_qp_offset_list = {};
};

struct SequenceConfig {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t profile = 5; /* D3D12 Main444 enum value. */
    std::uint32_t level = 8;   /* D3D12 HEVC 5.1 enum value. */
    std::uint32_t configuration_flags = 0;
    std::uint8_t min_luma_coding_unit_size = 0;
    std::uint8_t max_luma_coding_unit_size = 3;
    std::uint8_t min_luma_transform_unit_size = 0;
    std::uint8_t max_luma_transform_unit_size = 3;
    std::uint8_t max_transform_hierarchy_depth_inter = 0;
    std::uint8_t max_transform_hierarchy_depth_intra = 0;
    PictureDefaults picture;
    bool chroma_format_444 = true;
    std::uint8_t bit_depth_luma_minus8 = 0;
    std::uint8_t bit_depth_chroma_minus8 = 0;
};

struct ParsedConfig {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t profile_idc = 0;
    std::uint32_t level_idc = 0;
    std::uint32_t chroma_format_idc = 0;
    bool separate_colour_plane = false;
    std::uint8_t bit_depth_luma_minus8 = 0;
    std::uint8_t bit_depth_chroma_minus8 = 0;
    std::uint8_t min_luma_coding_unit_size = 0;
    std::uint8_t max_luma_coding_unit_size = 0;
    std::uint8_t min_luma_transform_unit_size = 0;
    std::uint8_t max_luma_transform_unit_size = 0;
    std::uint8_t max_transform_hierarchy_depth_inter = 0;
    std::uint8_t max_transform_hierarchy_depth_intra = 0;
    std::uint32_t configuration_flags = 0;
    std::uint32_t picture_flags = 0;
    std::uint8_t diff_cu_chroma_qp_offset_depth = 0;
    std::uint8_t log2_sao_offset_scale_luma = 0;
    std::uint8_t log2_sao_offset_scale_chroma = 0;
    std::uint8_t log2_max_transform_skip_block_size_minus2 = 0;
    std::uint8_t chroma_qp_offset_list_len_minus1 = 0;
    std::array<std::int8_t, 6> cb_qp_offset_list = {};
    std::array<std::int8_t, 6> cr_qp_offset_list = {};
};

namespace detail {

class BitWriter {
public:
    void bit(std::uint32_t value)
    {
        if ((bits_ & 7) == 0)
            bytes_.push_back(0);
        bytes_.back() |= static_cast<std::uint8_t>((value & 1u) <<
                                                    (7 - (bits_ & 7)));
        ++bits_;
    }

    void bits(unsigned count, std::uint64_t value)
    {
        for (unsigned i = 0; i < count; ++i)
            bit(static_cast<std::uint32_t>(value >> (count - i - 1)));
    }

    void ue(std::uint64_t value)
    {
        const std::uint64_t code_num = value + 1;
        unsigned width = 0;
        for (std::uint64_t v = code_num; v > 1; v >>= 1)
            ++width;
        for (unsigned i = 0; i < width; ++i)
            bit(0);
        bits(width + 1, code_num);
    }

    void se(std::int64_t value)
    {
        const std::uint64_t mapped = value <= 0
            ? static_cast<std::uint64_t>(-value) * 2
            : static_cast<std::uint64_t>(value) * 2 - 1;
        ue(mapped);
    }

    void rbsp_trailing_bits()
    {
        bit(1);
        while ((bits_ & 7) != 0)
            bit(0);
    }

    const std::vector<std::uint8_t> &bytes() const { return bytes_; }
private:
    std::vector<std::uint8_t> bytes_;
    std::size_t bits_ = 0;
};

class BitReader {
public:
    explicit BitReader(const std::vector<std::uint8_t> &bytes)
        : bytes_(bytes) {}

    bool bits(unsigned count, std::uint64_t *value)
    {
        if (!value || count > 64 || bit_ > bytes_.size() * 8 ||
            count > bytes_.size() * 8 - bit_)
            return false;
        *value = 0;
        for (unsigned i = 0; i < count; ++i) {
            *value = (*value << 1) |
                     ((bytes_[bit_ / 8] >> (7 - (bit_ & 7))) & 1u);
            ++bit_;
        }
        return true;
    }

    bool bit(std::uint64_t *value) { return bits(1, value); }

    bool ue(std::uint64_t *value)
    {
        unsigned zeros = 0;
        std::uint64_t current = 0;
        for (;;) {
            if (!bit(&current))
                return false;
            if (current != 0)
                break;
            if (++zeros >= 63)
                return false;
        }
        if (!bits(zeros, &current))
            return false;
        *value = ((std::uint64_t{1} << zeros) - 1) + current;
        return true;
    }

    bool se(std::int64_t *value)
    {
        std::uint64_t code = 0;
        if (!ue(&code))
            return false;
        *value = (code & 1) ? static_cast<std::int64_t>((code + 1) / 2)
                            : -static_cast<std::int64_t>(code / 2);
        return true;
    }

private:
    const std::vector<std::uint8_t> &bytes_;
    std::size_t bit_ = 0;
};

inline std::vector<std::uint8_t> escape(const std::vector<std::uint8_t> &rbsp)
{
    std::vector<std::uint8_t> result;
    unsigned zeroes = 0;
    for (std::uint8_t value : rbsp) {
        if (zeroes >= 2 && value <= 3) {
            result.push_back(3);
            zeroes = 0;
        }
        result.push_back(value);
        zeroes = value == 0 ? zeroes + 1 : 0;
    }
    return result;
}

inline std::vector<std::uint8_t> unescape(const std::uint8_t *data,
                                          std::size_t size)
{
    std::vector<std::uint8_t> result;
    unsigned zeroes = 0;
    for (std::size_t i = 0; i < size; ++i) {
        if (zeroes >= 2 && data[i] == 3) {
            zeroes = 0;
            continue;
        }
        result.push_back(data[i]);
        zeroes = data[i] == 0 ? zeroes + 1 : 0;
    }
    return result;
}

inline std::vector<std::uint8_t> nal(unsigned type,
                                     const std::vector<std::uint8_t> &rbsp)
{
    std::vector<std::uint8_t> result = {0, 0, 0, 1,
                                        static_cast<std::uint8_t>(type << 1),
                                        1};
    const auto escaped = escape(rbsp);
    result.insert(result.end(), escaped.begin(), escaped.end());
    return result;
}

inline void profile_tier_level(BitWriter *writer, const SequenceConfig &config)
{
    const unsigned profile_idc = config.profile == 5 ? 4 :
        (config.profile == 6 ? 5 : (config.profile == 7 ? 6 : 4));
    const unsigned level_idc[] = {30, 60, 63, 90, 93, 120, 123, 150,
                                  153, 156, 180, 183, 186};
    writer->bits(2, 0);                         // profile_space
    writer->bit(0);                             // tier_flag (Main tier)
    writer->bits(5, profile_idc);               // profile_idc
    writer->bits(32, 0);                        // compatibility flags
    writer->bits(48, 0);                        // constraint flags
    writer->bits(8, config.level < 13 ? level_idc[config.level] : 153);
}

inline std::vector<std::uint8_t> make_vps(const SequenceConfig &config)
{
    BitWriter writer;
    writer.bits(4, 0);   // vps_video_parameter_set_id
    writer.bit(1);       // vps_base_layer_internal_flag
    writer.bit(1);       // vps_base_layer_available_flag
    writer.bits(6, 0);   // vps_max_layers_minus1
    writer.bits(3, 0);   // vps_max_sub_layers_minus1
    writer.bit(1);       // vps_temporal_id_nesting_flag
    writer.bits(16, 0xffff);
    profile_tier_level(&writer, config);
    writer.rbsp_trailing_bits();
    return nal(32, writer.bytes());
}

inline std::vector<std::uint8_t> make_sps(const SequenceConfig &config)
{
    if (!config.width || !config.height || !config.chroma_format_444 ||
        config.min_luma_coding_unit_size > config.max_luma_coding_unit_size ||
        config.min_luma_transform_unit_size > config.max_luma_transform_unit_size ||
        config.max_luma_coding_unit_size > 3 ||
        config.max_luma_transform_unit_size > 3 ||
        config.bit_depth_luma_minus8 > 4 || config.bit_depth_chroma_minus8 > 4)
        return {};

    BitWriter writer;
    writer.bits(4, 0);   // sps_video_parameter_set_id
    writer.bits(3, 0);   // sps_max_sub_layers_minus1
    writer.bit(1);       // sps_temporal_id_nesting_flag
    profile_tier_level(&writer, config);
    writer.ue(0);        // sps_seq_parameter_set_id
    writer.ue(3);        // chroma_format_idc
    writer.bit((config.configuration_flags & kConfigSeparateColourPlane) != 0);
    writer.ue(config.width);
    writer.ue(config.height);
    writer.bit(0);       // conformance_window_flag
    writer.ue(config.bit_depth_luma_minus8);
    writer.ue(config.bit_depth_chroma_minus8);
    writer.ue(4);        // log2_max_pic_order_cnt_lsb_minus4
    writer.bit(0);       // sps_sub_layer_ordering_info_present_flag
    writer.ue(4);        // sps_max_dec_pic_buffering_minus1
    writer.ue(0);        // sps_max_num_reorder_pics
    writer.ue(0);        // sps_max_latency_increase_plus1
    writer.ue(config.min_luma_coding_unit_size);
    writer.ue(config.max_luma_coding_unit_size -
              config.min_luma_coding_unit_size);
    writer.ue(config.min_luma_transform_unit_size);
    writer.ue(config.max_luma_transform_unit_size -
              config.min_luma_transform_unit_size);
    writer.ue(config.max_transform_hierarchy_depth_inter);
    writer.ue(config.max_transform_hierarchy_depth_intra);
    writer.bit(0);       // scaling_list_enabled_flag
    writer.bit((config.configuration_flags &
                kConfigUseAsymmetricMotionPartition) != 0);
    writer.bit((config.configuration_flags & kConfigEnableSaoFilter) != 0);
    writer.bit(0);       // pcm_enabled_flag
    writer.ue(0);        // num_short_term_ref_pic_sets
    writer.bit(0);       // long_term_ref_pics_present_flag
    writer.bit((config.configuration_flags & kConfigTemporalMvpEnabled) != 0);
    writer.bit((config.configuration_flags &
                kConfigStrongIntraSmoothingEnabled) != 0);
    writer.bit(0);       // vui_parameters_present_flag
    const bool range_extension = config.profile >= 5 ||
        (config.configuration_flags &
         (kConfigTransformSkipRotation | kConfigTransformSkipContext |
          kConfigImplicitRdpcm | kConfigExplicitRdpcm |
          kConfigExtendedPrecisionProcessing | kConfigIntraSmoothingDisabled |
          kConfigHighPrecisionOffsets | kConfigPersistentRiceAdaptation |
          kConfigCabacBypassAlignment)) != 0;
    writer.bit(range_extension); // sps_extension_present_flag
    if (range_extension) {
        writer.bit(1);       // sps_range_extension_flag
        writer.bit(0);       // sps_multilayer_extension_flag
        writer.bit(0);       // sps_3d_extension_flag
        writer.bit(0);       // sps_scc_extension_flag
        writer.bits(4, 0);   // sps_extension_4bits
        writer.bit((config.configuration_flags & kConfigTransformSkipRotation) != 0);
        writer.bit((config.configuration_flags & kConfigTransformSkipContext) != 0);
        writer.bit((config.configuration_flags & kConfigImplicitRdpcm) != 0);
        writer.bit((config.configuration_flags & kConfigExplicitRdpcm) != 0);
        writer.bit((config.configuration_flags & kConfigExtendedPrecisionProcessing) != 0);
        writer.bit((config.configuration_flags & kConfigIntraSmoothingDisabled) != 0);
        writer.bit((config.configuration_flags & kConfigHighPrecisionOffsets) != 0);
        writer.bit((config.configuration_flags & kConfigPersistentRiceAdaptation) != 0);
        writer.bit((config.configuration_flags & kConfigCabacBypassAlignment) != 0);
    }
    writer.rbsp_trailing_bits();
    return nal(33, writer.bytes());
}

inline std::vector<std::uint8_t> make_pps(const SequenceConfig &config)
{
    const std::uint32_t flags = config.configuration_flags;
    const std::uint32_t picture_flags = config.picture.flags;
    const bool range_extension =
        (config.profile >= 5) ||
        (flags & (kConfigTransformSkipRotation | kConfigTransformSkipContext |
                  kConfigImplicitRdpcm | kConfigExplicitRdpcm |
                  kConfigExtendedPrecisionProcessing |
                  kConfigIntraSmoothingDisabled | kConfigHighPrecisionOffsets |
                  kConfigPersistentRiceAdaptation |
                  kConfigCabacBypassAlignment)) != 0 ||
        (picture_flags & (kPictureCrossComponentPrediction |
                          kPictureChromaQpOffsetList)) != 0;
    if (config.picture.chroma_qp_offset_list_len_minus1 > 5)
        return {};

    BitWriter writer;
    writer.ue(0);        // pps_pic_parameter_set_id
    writer.ue(0);        // pps_seq_parameter_set_id
    writer.bit(0);       // dependent_slice_segments_enabled_flag
    writer.bit(0);       // output_flag_present_flag
    writer.bits(3, 0);   // num_extra_slice_header_bits
    writer.bit(0);       // sign_data_hiding_enabled_flag
    writer.bit(0);       // cabac_init_present_flag
    writer.ue(0);        // num_ref_idx_l0_default_active_minus1
    writer.ue(0);        // num_ref_idx_l1_default_active_minus1
    writer.se(0);        // init_qp_minus26
    writer.bit((flags & kConfigUseConstrainedIntraprediction) != 0);
    writer.bit((flags & kConfigEnableTransformSkipping) != 0);
    writer.bit(1);       // cu_qp_delta_enabled_flag
    writer.ue(0);        // diff_cu_qp_delta_depth
    writer.se(0);        // pps_cb_qp_offset
    writer.se(0);        // pps_cr_qp_offset
    writer.bit((picture_flags & kPictureChromaQpOffsetList) != 0);
    writer.bit(0);       // weighted_pred_flag
    writer.bit(0);       // weighted_bipred_flag
    writer.bit(0);       // transquant_bypass_enabled_flag
    writer.bit(0);       // tiles_enabled_flag
    writer.bit(0);       // entropy_coding_sync_enabled_flag
    writer.bit(1);       // pps_loop_filter_across_slices_enabled_flag
    writer.bit(0);       // deblocking_filter_control_present_flag
    writer.bit(0);       // pps_scaling_list_data_present_flag
    writer.bit(0);       // lists_modification_present_flag
    writer.ue(0);        // log2_parallel_merge_level_minus2
    writer.bit(0);       // slice_segment_header_extension_present_flag
    writer.bit(range_extension); // pps_extension_present_flag
    if (range_extension) {
        writer.bit(1);       // pps_range_extension_flag
        writer.bit(0);       // pps_multilayer_extension_flag
        writer.bit(0);       // pps_3d_extension_flag
        writer.bit(0);       // pps_scc_extension_flag
        writer.bits(4, 0);   // pps_extension_4bits
        if (flags & kConfigEnableTransformSkipping)
            writer.ue(config.picture.log2_max_transform_skip_block_size_minus2);
        writer.bit((picture_flags & kPictureCrossComponentPrediction) != 0);
        writer.bit((picture_flags & kPictureChromaQpOffsetList) != 0);
        if (picture_flags & kPictureChromaQpOffsetList) {
            writer.ue(config.picture.diff_cu_chroma_qp_offset_depth);
            writer.ue(config.picture.chroma_qp_offset_list_len_minus1);
            const unsigned count = config.picture.chroma_qp_offset_list_len_minus1 + 1;
            for (unsigned i = 0; i < count; ++i) {
                writer.se(config.picture.cb_qp_offset_list[i]);
                writer.se(config.picture.cr_qp_offset_list[i]);
            }
        }
        writer.ue(config.picture.log2_sao_offset_scale_luma);
        writer.ue(config.picture.log2_sao_offset_scale_chroma);
    }
    writer.rbsp_trailing_bits();
    return nal(34, writer.bytes());
}

inline bool next_nal(const std::vector<std::uint8_t> &annex_b,
                     std::size_t *cursor, unsigned *type,
                     std::vector<std::uint8_t> *nal_bytes)
{
    if (!cursor || !type || !nal_bytes)
        return false;
    std::size_t start = *cursor;
    while (start + 3 <= annex_b.size() &&
           !(annex_b[start] == 0 && annex_b[start + 1] == 0 &&
             (annex_b[start + 2] == 1 || (start + 3 < annex_b.size() &&
              annex_b[start + 2] == 0 && annex_b[start + 3] == 1))))
        ++start;
    if (start + 3 > annex_b.size())
        return false;
    const std::size_t prefix = annex_b[start + 2] == 1 ? 3 : 4;
    const std::size_t begin = start + prefix;
    if (begin + 2 > annex_b.size())
        return false;
    std::size_t end = begin;
    bool found_next = false;
    while (end + 3 <= annex_b.size() &&
           !(annex_b[end] == 0 && annex_b[end + 1] == 0 &&
             (annex_b[end + 2] == 1 || (end + 3 < annex_b.size() &&
              annex_b[end + 2] == 0 && annex_b[end + 3] == 1))))
        ++end;
    if (end + 3 <= annex_b.size())
        found_next = true;
    if (!found_next)
        end = annex_b.size();
    if (end == begin)
        return false;
    *type = (annex_b[begin] >> 1) & 0x3f;
    nal_bytes->assign(annex_b.begin() + start, annex_b.begin() + end);
    *cursor = end;
    return true;
}

inline bool parse_sps(const std::vector<std::uint8_t> &nal_bytes,
                      ParsedConfig *out)
{
    if (!out || nal_bytes.size() < 6)
        return false;
    const std::size_t prefix = nal_bytes[2] == 1 ? 3 : 4;
    if (nal_bytes.size() < prefix + 2)
        return false;
    const auto rbsp = unescape(nal_bytes.data() + prefix + 2,
                               nal_bytes.size() - prefix - 2);
    BitReader reader(rbsp);
    std::uint64_t value = 0, layers = 0, chroma = 0, width = 0, height = 0;
    std::uint64_t profile_idc = 0, level_idc = 0;
    if (!reader.bits(4, &value) || !reader.bits(3, &layers) ||
        !reader.bit(&value) || !reader.bits(2, &value) || !reader.bit(&value) ||
        !reader.bits(5, &profile_idc) || !reader.bits(32, &value) ||
        !reader.bits(48, &value) || !reader.bits(8, &level_idc))
        return false;
    if (layers != 0)
        return false;
    std::uint64_t separate_plane = 0;
    if (!reader.ue(&value) || !reader.ue(&chroma) || chroma != 3 ||
        !reader.bit(&separate_plane) || !reader.ue(&width) || !reader.ue(&height) ||
        !reader.bit(&value) || !reader.ue(&value))
        return false;
    out->profile_idc = static_cast<std::uint32_t>(profile_idc);
    out->level_idc = static_cast<std::uint32_t>(level_idc);
    out->width = static_cast<std::uint32_t>(width);
    out->height = static_cast<std::uint32_t>(height);
    out->chroma_format_idc = static_cast<std::uint32_t>(chroma);
    out->separate_colour_plane = separate_plane != 0;
    if (out->separate_colour_plane)
        out->configuration_flags |= kConfigSeparateColourPlane;
    out->bit_depth_luma_minus8 = static_cast<std::uint8_t>(value);
    if (!reader.ue(&value))
        return false;
    out->bit_depth_chroma_minus8 = static_cast<std::uint8_t>(value);
    if (!reader.ue(&value) || !reader.bit(&value))
        return false;
    /* sps_sub_layer_ordering_info_present_flag is zero in the builder. */
    for (std::uint64_t i = 0; i <= layers; ++i)
        if (!reader.ue(&value) || !reader.ue(&value) || !reader.ue(&value))
            return false;
    std::uint64_t min_cu = 0, diff_cu = 0, min_tu = 0, diff_tu = 0;
    std::uint64_t max_hierarchy_inter = 0, max_hierarchy_intra = 0;
    if (!reader.ue(&min_cu) || !reader.ue(&diff_cu) || !reader.ue(&min_tu) ||
        !reader.ue(&diff_tu) || !reader.ue(&max_hierarchy_inter) ||
        !reader.ue(&max_hierarchy_intra))
        return false;
    if (min_cu > 3 || diff_cu > 3 || min_tu > 3 || diff_tu > 3)
        return false;
    if (max_hierarchy_inter > 7 || max_hierarchy_intra > 7)
        return false;
    out->min_luma_coding_unit_size = static_cast<std::uint8_t>(min_cu);
    out->max_luma_coding_unit_size =
        static_cast<std::uint8_t>(min_cu + diff_cu);
    out->min_luma_transform_unit_size = static_cast<std::uint8_t>(min_tu);
    out->max_luma_transform_unit_size =
        static_cast<std::uint8_t>(min_tu + diff_tu);
    out->max_transform_hierarchy_depth_inter =
        static_cast<std::uint8_t>(max_hierarchy_inter);
    out->max_transform_hierarchy_depth_intra =
        static_cast<std::uint8_t>(max_hierarchy_intra);
    if (!reader.bit(&value) || value != 0) {
        return false; // scaling_list_enabled_flag
    }
    if (!reader.bit(&value))
        return false; // amp_enabled_flag
    if (value)
        out->configuration_flags |= kConfigUseAsymmetricMotionPartition;
    if (!reader.bit(&value))
        return false; // sample_adaptive_offset_enabled_flag
    if (value)
        out->configuration_flags |= kConfigEnableSaoFilter;
    if (!reader.bit(&value) || value != 0 || !reader.ue(&value) || value != 0)
        return false; // pcm flag and num_short_term_ref_pic_sets
    if (!reader.bit(&value) || value != 0)
        return false; // long_term_ref_pics_present_flag
    if (!reader.bit(&value))
        return false; // sps_temporal_mvp_enabled_flag
    if (value)
        out->configuration_flags |= kConfigTemporalMvpEnabled;
    if (!reader.bit(&value))
        return false; // strong_intra_smoothing_enabled_flag
    if (value)
        out->configuration_flags |= kConfigStrongIntraSmoothingEnabled;
    if (!reader.bit(&value) || value != 0 || !reader.bit(&value))
        return false; // VUI and sps_extension_present_flag
    if (value) {
        std::uint64_t extension_flags = 0;
        if (!reader.bit(&extension_flags) || !reader.bit(&value) ||
            !reader.bit(&value) || !reader.bit(&value) ||
            !reader.bits(4, &value))
            return false;
        if (extension_flags) {
            const std::uint32_t flags[] = {
                kConfigTransformSkipRotation, kConfigTransformSkipContext,
                kConfigImplicitRdpcm, kConfigExplicitRdpcm,
                kConfigExtendedPrecisionProcessing, kConfigIntraSmoothingDisabled,
                kConfigHighPrecisionOffsets, kConfigPersistentRiceAdaptation,
                kConfigCabacBypassAlignment};
            for (std::uint32_t flag : flags) {
                if (!reader.bit(&value))
                    return false;
                if (value)
                    out->configuration_flags |= flag;
            }
        }
    }
    (void)min_cu;
    (void)diff_cu;
    (void)min_tu;
    (void)diff_tu;
    return true;
}

inline bool parse_pps(const std::vector<std::uint8_t> &nal_bytes,
                      ParsedConfig *out)
{
    if (!out || nal_bytes.size() < 6)
        return false;
    const std::size_t prefix = nal_bytes[2] == 1 ? 3 : 4;
    if (nal_bytes.size() < prefix + 2)
        return false;
    const auto rbsp = unescape(nal_bytes.data() + prefix + 2,
                               nal_bytes.size() - prefix - 2);
    BitReader reader(rbsp);
    std::uint64_t value = 0;
    if (!reader.ue(&value) || !reader.ue(&value) || !reader.bit(&value) ||
        !reader.bit(&value) || !reader.bits(3, &value) || !reader.bit(&value) ||
        !reader.bit(&value) || !reader.ue(&value) || !reader.ue(&value))
        return false;
    std::int64_t signed_value = 0;
    if (!reader.se(&signed_value) || !reader.bit(&value))
        return false;
    if (value)
        out->configuration_flags |= kConfigUseConstrainedIntraprediction;
    std::uint64_t transform_skip_value = 0;
    if (!reader.bit(&transform_skip_value))
        return false;
    const bool transform_skip = transform_skip_value != 0;
    if (transform_skip)
        out->configuration_flags |= kConfigEnableTransformSkipping;
    std::uint64_t cu_qp_delta = 0;
    if (!reader.bit(&cu_qp_delta))
        return false;
    if (cu_qp_delta && !reader.ue(&value))
        return false;
    if (!reader.se(&signed_value) || !reader.se(&signed_value) ||
        !reader.bit(&value))
        return false;
    if (!reader.bit(&value) || !reader.bit(&value) || !reader.bit(&value) ||
        !reader.bit(&value) || !reader.bit(&value) || !reader.bit(&value) ||
        !reader.bit(&value) || !reader.bit(&value) || !reader.bit(&value) ||
        !reader.ue(&value) || !reader.bit(&value) ||
        !reader.bit(&value))
        return false;
    const bool extension_present = value != 0;
    if (!extension_present)
        return true;
    std::uint64_t range_extension_value = 0;
    if (!reader.bit(&range_extension_value) || !reader.bit(&value) ||
        !reader.bit(&value) || !reader.bit(&value) || !reader.bits(4, &value))
        return false;
    const bool range_extension = range_extension_value != 0;
    if (!range_extension)
        return true;
    if (transform_skip) {
        if (!reader.ue(&value))
            return false;
        out->log2_max_transform_skip_block_size_minus2 =
            static_cast<std::uint8_t>(value);
    }
    if (!reader.bit(&value))
        return false;
    if (value)
        out->picture_flags |= kPictureCrossComponentPrediction;
    if (!reader.bit(&value))
        return false;
    if (value) {
        out->picture_flags |= kPictureChromaQpOffsetList;
        if (!reader.ue(&value))
            return false;
        out->diff_cu_chroma_qp_offset_depth = static_cast<std::uint8_t>(value);
        if (!reader.ue(&value))
            return false;
        out->chroma_qp_offset_list_len_minus1 = static_cast<std::uint8_t>(value);
        if (value > 5)
            return false;
        for (unsigned i = 0; i <= value; ++i) {
            std::int64_t cb = 0, cr = 0;
            if (!reader.se(&cb) || !reader.se(&cr))
                return false;
            out->cb_qp_offset_list[i] = static_cast<std::int8_t>(cb);
            out->cr_qp_offset_list[i] = static_cast<std::int8_t>(cr);
        }
    }
    if (!reader.ue(&value) || !reader.ue(&value))
        return false;
    return true;
}

inline bool parse_hevc444_sequence_headers(
    const std::vector<std::uint8_t> &annex_b, ParsedConfig *out)
{
    if (!out)
        return false;
    *out = {};
    std::size_t cursor = 0;
    bool have_vps = false, have_sps = false, have_pps = false;
    while (cursor < annex_b.size()) {
        unsigned type = 0;
        std::vector<std::uint8_t> nal_bytes;
        if (!next_nal(annex_b, &cursor, &type, &nal_bytes))
            return false;
        if (type == 32) {
            have_vps = true;
        } else if (type == 33) {
            if (!parse_sps(nal_bytes, out))
                return false;
            have_sps = true;
        } else if (type == 34) {
            if (!parse_pps(nal_bytes, out))
                return false;
            have_pps = true;
        }
    }
    return have_vps && have_sps && have_pps && out->profile_idc == 4 &&
           out->chroma_format_idc == 3;
}

inline bool build_hevc444_sequence_headers(const SequenceConfig &config,
                                           std::vector<std::uint8_t> *out)
{
    if (!out || !config.chroma_format_444 || !config.width || !config.height)
        return false;
    constexpr std::uint32_t supported_configuration_flags =
        kConfigUseAsymmetricMotionPartition | kConfigEnableTransformSkipping |
        kConfigTransformSkipRotation | kConfigTransformSkipContext |
        kConfigImplicitRdpcm | kConfigExplicitRdpcm |
        kConfigExtendedPrecisionProcessing | kConfigIntraSmoothingDisabled |
        kConfigHighPrecisionOffsets | kConfigPersistentRiceAdaptation |
        kConfigCabacBypassAlignment | kConfigSeparateColourPlane |
        kConfigTemporalMvpEnabled | kConfigStrongIntraSmoothingEnabled |
        kConfigEnableSaoFilter | kConfigUseConstrainedIntraprediction;
    constexpr std::uint32_t supported_picture_flags =
        kPictureCrossComponentPrediction | kPictureChromaQpOffsetList;
    if ((config.configuration_flags & ~supported_configuration_flags) != 0 ||
        (config.picture.flags & ~supported_picture_flags) != 0)
        return false;
    *out = make_vps(config);
    const auto sps = make_sps(config);
    const auto pps = make_pps(config);
    if (sps.empty() || pps.empty()) {
        out->clear();
        return false;
    }
    out->insert(out->end(), sps.begin(), sps.end());
    out->insert(out->end(), pps.begin(), pps.end());
    return true;
}

} // namespace detail

inline bool build_hevc444_sequence_headers(const SequenceConfig &config,
                                           std::vector<std::uint8_t> *out)
{
    return detail::build_hevc444_sequence_headers(config, out);
}

inline bool parse_hevc444_sequence_headers(
    const std::vector<std::uint8_t> &annex_b, ParsedConfig *out)
{
    return detail::parse_hevc444_sequence_headers(annex_b, out);
}

} // namespace appsandbox_hevc444_probe
