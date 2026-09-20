/* SPDX-License-Identifier: MIT
 * The initial D3D12 backend accepts the guest's canonical all-IDR stream.
 * Re-encoding parsed headers and comparing every byte deliberately rejects
 * syntax not represented by the value model (tiles, references, VUI, etc.).
 */
#pragma once
#include "../../tools/linux/agent/hevc444_probe_codec.h"
#include "../../tools/win/hevc444-probe/hevc_access_unit_probe.h"
#include "vm_hevc444_probe_sample.h"
#include <cstring>

namespace asb_hevc_decode {
inline bool configuration(const unsigned char *data, unsigned size,
    appsandbox_hevc444_probe::ParsedConfig *parsed, bool *bundled)
{
    using namespace appsandbox_hevc444_probe;
    if (!data || !parsed || !bundled || !size || size > 65536) return false;
    *bundled = size == ASB_HEVC444_PROBE_EXTRADATA_SIZE &&
        std::memcmp(data, asb_hevc444_probe_sample, size) == 0;
    *parsed = {};
    if (*bundled) return true;
    std::vector<std::uint8_t> headers(data, data + size), canonical;
    if (!parse_hevc444_sequence_headers(headers, parsed) ||
        parsed->width != 3840 || parsed->height != 2160 ||
        parsed->level_idc != 153 || parsed->bit_depth_luma_minus8 ||
        parsed->bit_depth_chroma_minus8 || parsed->separate_colour_plane)
        return false;
    SequenceConfig c;
    c.width = parsed->width; c.height = parsed->height;
    c.configuration_flags = parsed->configuration_flags;
    c.min_luma_coding_unit_size = parsed->min_luma_coding_unit_size;
    c.max_luma_coding_unit_size = parsed->max_luma_coding_unit_size;
    c.min_luma_transform_unit_size = parsed->min_luma_transform_unit_size;
    c.max_luma_transform_unit_size = parsed->max_luma_transform_unit_size;
    c.max_transform_hierarchy_depth_inter = parsed->max_transform_hierarchy_depth_inter;
    c.max_transform_hierarchy_depth_intra = parsed->max_transform_hierarchy_depth_intra;
    c.picture.flags = parsed->picture_flags;
    c.picture.diff_cu_chroma_qp_offset_depth = parsed->diff_cu_chroma_qp_offset_depth;
    c.picture.log2_sao_offset_scale_luma = parsed->log2_sao_offset_scale_luma;
    c.picture.log2_sao_offset_scale_chroma = parsed->log2_sao_offset_scale_chroma;
    c.picture.log2_max_transform_skip_block_size_minus2 = parsed->log2_max_transform_skip_block_size_minus2;
    c.picture.chroma_qp_offset_list_len_minus1 = parsed->chroma_qp_offset_list_len_minus1;
    c.picture.cb_qp_offset_list = parsed->cb_qp_offset_list;
    c.picture.cr_qp_offset_list = parsed->cr_qp_offset_list;
    if (c.min_luma_coding_unit_size > c.max_luma_coding_unit_size ||
        c.max_luma_coding_unit_size > 3 || c.max_luma_transform_unit_size > 3 ||
        c.min_luma_transform_unit_size > c.max_luma_transform_unit_size)
        return false;
    if (c.height % (1u << (c.min_luma_coding_unit_size + 3))) return false;
    return build_hevc444_sequence_headers(c, &canonical) && canonical == headers;
}

inline bool idr_slice(const unsigned char *data, unsigned size,
                      std::vector<std::uint8_t> *compressed)
{
    using namespace hevc_access_unit_probe;
    if (!data || !size || size > 32u * 1024 * 1024 || !compressed) return false;
    std::vector<Nal> nals;
    if (size < 4 || data[0] || data[1] ||
        !(data[2] == 1 || (data[2] == 0 && data[3] == 1)) ||
        !split(std::vector<std::uint8_t>(data, data + size), &nals)) return false;
    compressed->clear();
    for (const auto &nal : nals) {
        const size_t prefix = nal.bytes[2] == 1 ? 3 : 4;
        if (nal.bytes.size() < prefix + 3 ||
            (nal.bytes[prefix] & 0x81) || nal.bytes[prefix + 1] != 1)
            return false; // forbidden bit, layer id, temporal_id_plus1
        if (!is_vcl(nal.type)) {
            if (nal.type != 35 && nal.type != 39 && nal.type != 40) return false;
            continue;
        }
        if (!is_idr(nal.type) || !compressed->empty()) return false;
        const auto rbsp = unescape(nal.bytes.data() + prefix + 2,
                                   nal.bytes.size() - prefix - 2);
        appsandbox_hevc444_probe::detail::BitReader bits(rbsp);
        std::uint64_t first, no_prior, pps, type;
        if (!bits.bit(&first) || !first || !bits.bit(&no_prior) ||
            !bits.ue(&pps) || pps != 0 || !bits.ue(&type) || type != 2)
            return false;
        *compressed = {0, 0, 1};
        compressed->insert(compressed->end(), nal.bytes.begin() + prefix, nal.bytes.end());
    }
    if (compressed->empty()) return false;
    compressed->resize((compressed->size() + 127) & ~size_t(127), 0);
    return true;
}
}
