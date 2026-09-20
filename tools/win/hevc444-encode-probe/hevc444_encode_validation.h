#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../hevc444-probe/hevc_access_unit_probe.h"
#include "../../linux/agent/hevc444_probe_codec.h"

namespace appsandbox_hevc444_encode {

struct PayloadValidation {
    bool split_ok = false;
    bool have_vcl = false;
    bool have_idr = false;
    bool valid_nal_headers = false;
    bool first_slice = false;
    bool pps_id_zero = false;
    bool intra_slice = false;
    bool mixed_vcl = false;
    bool all_nal_headers_valid = false;
    unsigned vcl_count = 0;

    bool valid() const {
        return split_ok && have_vcl && have_idr && valid_nal_headers &&
               all_nal_headers_valid && vcl_count == 1 && first_slice &&
               pps_id_zero && intra_slice && !mixed_vcl;
    }
};

inline PayloadValidation validate_encoder_payload(
    bool split_ok, const std::vector<hevc_access_unit_probe::Nal> &nals)
{
    PayloadValidation result;
    result.split_ok = split_ok;
    result.all_nal_headers_valid = split_ok && !nals.empty();
    for (const auto &nal : nals) {
        const std::size_t prefix = nal.bytes.size() >= 4 &&
                nal.bytes[2] == 1 ? 3 : 4;
        const bool header_ok = nal.bytes.size() >= prefix + 2 &&
            (nal.bytes[prefix] & 0x80) == 0 &&
            (nal.bytes[prefix] & 1) == 0 &&
            nal.bytes[prefix + 1] == 1 &&
            ((nal.bytes[prefix] >> 1) & 0x3f) == nal.type;
        result.all_nal_headers_valid &= header_ok;
        if (!header_ok)
            continue;
        result.have_vcl |= hevc_access_unit_probe::is_vcl(nal.type);
        result.have_idr |= hevc_access_unit_probe::is_idr(nal.type);
        result.mixed_vcl |= hevc_access_unit_probe::is_vcl(nal.type) &&
                            !hevc_access_unit_probe::is_idr(nal.type);
        if (!hevc_access_unit_probe::is_vcl(nal.type))
            continue;
        ++result.vcl_count;
        if (nal.bytes.size() < prefix + 3 ||
            (nal.bytes[prefix] & 0x80) != 0 ||
            (nal.bytes[prefix + 1] & 7) == 0)
            continue;
        result.valid_nal_headers = true;
        const auto rbsp = appsandbox_hevc444_probe::detail::unescape(
            nal.bytes.data() + prefix + 2,
            nal.bytes.size() - prefix - 2);
        appsandbox_hevc444_probe::detail::BitReader reader(rbsp);
        std::uint64_t first = 0;
        if (!reader.bit(&first))
            continue;
        if (!first)
            continue;
        result.first_slice = true;
        std::uint64_t no_output = 0, pps = 0, slice_type = 0;
        if (!reader.bit(&no_output) || !reader.ue(&pps) ||
            !reader.ue(&slice_type))
            continue;
        result.pps_id_zero |= pps == 0;
        result.intra_slice |= slice_type == 2;
    }
    return result;
}

inline bool validate_main444_sequence_header(
    bool parsed, std::uint32_t profile_idc, std::uint32_t level_idc,
    std::uint64_t chroma_format_idc, std::uint8_t bit_depth_luma_minus8,
    std::uint8_t bit_depth_chroma_minus8, std::uint32_t width,
    std::uint32_t height, std::uint32_t expected_width,
    std::uint32_t expected_height, std::uint32_t configuration_flags,
    std::uint32_t expected_configuration_flags, std::uint32_t picture_flags,
    std::uint32_t expected_picture_flags)
{
    return parsed && profile_idc == 4 && level_idc == 153 &&
           chroma_format_idc == 3 && bit_depth_luma_minus8 == 0 &&
           bit_depth_chroma_minus8 == 0 && width == expected_width &&
           height == expected_height && configuration_flags ==
           expected_configuration_flags && picture_flags == expected_picture_flags;
}

inline bool supports_hevc_level_51(unsigned max_level, unsigned level_51)
{
    return max_level >= level_51;
}

inline const char *combined_state(bool profile_ok, bool level_51_supported,
                                  bool combined_ok)
{
    if (!profile_ok)
        return "BLOCKED_BY_PROFILE";
    if (!level_51_supported)
        return "BLOCKED_BY_LEVEL";
    return combined_ok ? "PASS" : "UNSUPPORTED";
}

inline bool four_k60_ready(bool codec_ok, bool profile_ok, bool ayuv_ok,
                           bool config_ok, bool resource_ok,
                           bool level_51_supported, bool combined_ok)
{
    return codec_ok && profile_ok && ayuv_ok && config_ok && resource_ok &&
           level_51_supported && combined_ok;
}

} // namespace appsandbox_hevc444_encode
