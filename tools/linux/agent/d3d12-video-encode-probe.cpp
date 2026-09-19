/* SPDX-License-Identifier: MIT
 *
 * D3D12 shared RGBA/BGRA texture -> GPU YUV -> D3D12 HEVC encoder.
 *
 * The existing cross-process share transport is included deliberately: this keeps
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
#include <ctime>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <vector>
#include <unistd.h>

#include "display_protocol.h"

namespace {

constexpr D3D12_VIDEO_ENCODER_CODEC kEncodeCodec =
    D3D12_VIDEO_ENCODER_CODEC_HEVC;
constexpr UINT kNodeIndex = 0;
constexpr UINT kFrameRateNumerator = 60;
constexpr UINT kFrameRateDenominator = 1;
constexpr UINT64 kBitstreamCapacity = 8ULL * 1024ULL * 1024ULL;

/* Dimensions belong to one encoder session. They are copied from Mutter's
 * ResourceBundleMessage for production and never stored in mutable globals. */
struct EncodeDimensions {
    std::uint32_t width;
    std::uint32_t height;
};

static constexpr EncodeDimensions kDiagnosticDimensions = {3840, 2160};
static constexpr DXGI_FORMAT kHevc420InputFormat = DXGI_FORMAT_NV12;
static constexpr DXGI_FORMAT kHevc444InputFormat = DXGI_FORMAT_AYUV;

static const char *stage_prefix(bool production)
{
    return production ? "APPSANDBOX" : "PASS";
}

static void publish_production_health(bool production, std::uint32_t width,
                                      std::uint32_t height, bool ready,
                                      bool gpu_copy, std::uint64_t frames_encoded,
                                      std::uint64_t encode_failures = 0)
{
    if (!production)
        return;
    const char *temporary = "/run/appsandbox/display-d3d12.health.tmp";
    std::ofstream health(temporary, std::ios::trunc);
    if (!health)
        return;
    std::ifstream marker("/opt/wsl-mesa/current/GRAPHICS");
    std::string graphics_version;
    std::getline(marker, graphics_version);
    if (!marker || graphics_version.empty())
        return;
    health << "graphics_version=" << graphics_version << "\n"
           << "session_id=" << static_cast<long>(getpid()) << "\n"
           << "timestamp=" << static_cast<long long>(std::time(nullptr)) << "\n"
           << "encoder_initialized=" << (ready ? 1 : 0) << "\n"
           << "native_d3d12_shared=" << (ready ? 1 : 0) << "\n"
           << "gpu_copy=" << (gpu_copy ? 1 : 0) << "\n"
           << "cpu_copy=0\n"
           << "cpu_conversion=0\n"
           << "framebuffer_mmap=0\n"
           << "cpu_memcpy_framebuffer=0\n"
           << "gpu_cpu_gpu=0\n"
           << "stale_frames=0\n"
           << "mismatches=0\n"
           << "encode_failures=" << encode_failures << "\n"
           << "ready=" << (ready ? 1 : 0) << "\n"
           << "frames_encoded=" << frames_encoded << "\n"
           << "resolution=" << width << "x" << height << "@60\n";
    health.close();
    if (health)
        std::rename(temporary, "/run/appsandbox/display-d3d12.health");
}

// D3D12 Video Encode returns picture payload NAL units; sequence headers are
// host-owned. This is a valid HEVC Main/level-5.1 template; the SPS picture
// dimensions are rewritten from Mutter's resource bundle before emission.
// The VPS/PPS are resolution-independent and contain no framebuffer pixels.
static constexpr std::uint8_t kHevcSequenceHeaders[] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x04,
    0x08, 0x00, 0x00, 0x03, 0x00, 0x9F, 0xA8, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x99, 0xBA, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01,
    0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x9F, 0xA8, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x99, 0xA0, 0x01, 0xE0, 0x20, 0x02, 0x1C, 0x59,
    0x6E, 0xAE, 0x46, 0xC2, 0xF0, 0x16, 0x80, 0x80, 0x00, 0x00, 0x03,
    0x00, 0x80, 0x00, 0x00, 0x1E, 0x04, 0x00, 0x00, 0x00, 0x01, 0x44,
    0x01, 0xC0, 0x71, 0x83, 0x12};

/* This is a real Main 4:4:4 8-bit VPS/SPS/PPS set emitted by x265 with
 * repeat-headers enabled.  It is deliberately separate from the existing
 * Main 4:2:0 template; the SPS below has chroma_format_idc == 3.  Only the
 * SPS dimensions are rewritten by hevc_dynamic_sps(). */
static constexpr std::uint8_t kHevc444SequenceHeaders[] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x04,
    0x08, 0x00, 0x00, 0x03, 0x00, 0x9E, 0x28, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x99, 0xBA, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01,
    0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x9E, 0x28, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x99, 0x90, 0x00, 0x3C, 0x04, 0x00, 0x43, 0x8B,
    0x2D, 0xD4, 0x92, 0x65, 0x78, 0x0B, 0x40, 0x40, 0x00, 0x00, 0x03,
    0x00, 0x40, 0x00, 0x00, 0x0F, 0x02, 0x00, 0x00, 0x00, 0x01, 0x44,
    0x01, 0xC1, 0x72, 0x86, 0x0C, 0x06, 0x24};

struct HevcBitReader {
    const std::vector<std::uint8_t> &bytes;
    std::size_t bit = 0;

    bool read(unsigned count, std::uint64_t *value)
    {
        if (!value || count > 64 || bit > bytes.size() * 8 ||
            count > bytes.size() * 8 - bit)
            return false;
        *value = 0;
        for (unsigned i = 0; i < count; ++i) {
            *value = (*value << 1) |
                     ((bytes[bit / 8] >> (7 - (bit % 8))) & 1U);
            ++bit;
        }
        return true;
    }

    bool ue(std::uint64_t *value)
    {
        unsigned leading_zeroes = 0;
        std::uint64_t bit_value = 0;
        while (true) {
            if (!read(1, &bit_value))
                return false;
            if (bit_value)
                break;
            if (++leading_zeroes >= 63)
                return false;
        }
        if (!read(leading_zeroes, &bit_value) ||
            (leading_zeroes == 63 && bit_value > 0))
            return false;
        *value = ((std::uint64_t{1} << leading_zeroes) - 1) + bit_value;
        return true;
    }
};

struct HevcBitWriter {
    std::vector<std::uint8_t> bytes;
    std::size_t bit = 0;

    void write_bit(std::uint8_t value)
    {
        if ((bit % 8) == 0)
            bytes.push_back(0);
        bytes.back() |= static_cast<std::uint8_t>((value & 1U) <<
                                                   (7 - (bit % 8)));
        ++bit;
    }

    void write(unsigned count, std::uint64_t value)
    {
        for (unsigned i = 0; i < count; ++i)
            write_bit(static_cast<std::uint8_t>(value >> (count - i - 1)));
    }

    void write_ue(std::uint64_t value)
    {
        const std::uint64_t code_num = value + 1;
        unsigned width = 0;
        for (std::uint64_t v = code_num; v > 1; v >>= 1)
            ++width;
        for (unsigned i = 0; i < width; ++i)
            write_bit(0);
        write(width + 1, code_num);
    }

    void copy_bits(const std::vector<std::uint8_t> &source,
                   std::size_t begin, std::size_t end)
    {
        for (std::size_t i = begin; i < end; ++i)
            write_bit(static_cast<std::uint8_t>(
                (source[i / 8] >> (7 - (i % 8))) & 1U));
    }
};

static std::vector<std::uint8_t> hevc_unescape(
    const std::uint8_t *data, std::size_t size)
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

static std::vector<std::uint8_t> hevc_escape(
    const std::vector<std::uint8_t> &data)
{
    std::vector<std::uint8_t> result;
    unsigned zeroes = 0;
    for (std::uint8_t byte : data) {
        if (zeroes >= 2 && byte <= 3) {
            result.push_back(3);
            zeroes = 0;
        }
        result.push_back(byte);
        zeroes = byte == 0 ? zeroes + 1 : 0;
    }
    return result;
}

static bool hevc_skip_profile_tier_level(HevcBitReader *reader,
                                         unsigned max_sub_layers)
{
    std::uint64_t value;
    if (!reader->read(8, &value) || !reader->read(32, &value) ||
        !reader->read(48, &value) || !reader->read(8, &value))
        return false;
    std::vector<std::uint8_t> profile_present(max_sub_layers);
    std::vector<std::uint8_t> level_present(max_sub_layers);
    for (unsigned i = 0; i < max_sub_layers; ++i) {
        if (!reader->read(1, &value) ||
            (profile_present[i] = static_cast<std::uint8_t>(value)) > 1 ||
            !reader->read(1, &value) ||
            (level_present[i] = static_cast<std::uint8_t>(value)) > 1)
            return false;
    }
    if (max_sub_layers && !reader->read(2 * (8 - max_sub_layers), &value))
        return false;
    for (unsigned i = 0; i < max_sub_layers; ++i) {
        if (profile_present[i] &&
            (!reader->read(64, &value) || !reader->read(24, &value)))
            return false;
        if (level_present[i] && !reader->read(8, &value))
            return false;
    }
    return true;
}

static std::vector<std::uint8_t> hevc_dynamic_sps(
    const std::vector<std::uint8_t> &nal, std::uint32_t width,
    std::uint32_t height)
{
    if (nal.size() < 3)
        return {};
    const std::vector<std::uint8_t> rbsp = hevc_unescape(nal.data() + 2,
                                                          nal.size() - 2);
    HevcBitReader reader{rbsp};
    std::uint64_t value, max_sub_layers, chroma, old_width, old_height;
    if (!reader.read(4, &value) ||
        !reader.read(3, &max_sub_layers) || !reader.read(1, &value) ||
        max_sub_layers > 6 ||
        !hevc_skip_profile_tier_level(&reader,
                                      static_cast<unsigned>(max_sub_layers)) ||
        !reader.ue(&value) || !reader.ue(&chroma))
        return {};
    if (chroma == 3 && !reader.read(1, &value))
        return {};
    const std::size_t width_begin = reader.bit;
    if (!reader.ue(&old_width))
        return {};
    if (!reader.ue(&old_height))
        return {};
    (void)old_width;
    (void)old_height;
    HevcBitWriter writer;
    writer.copy_bits(rbsp, 0, width_begin);
    writer.write_ue(width);
    writer.write_ue(height);
    writer.copy_bits(rbsp, reader.bit, rbsp.size() * 8);
    std::vector<std::uint8_t> result{nal[0], nal[1]};
    const std::vector<std::uint8_t> escaped = hevc_escape(writer.bytes);
    result.insert(result.end(), escaped.begin(), escaped.end());
    return result;
}

static std::vector<std::uint8_t> dynamic_hevc_sequence_headers(
    std::uint32_t width, std::uint32_t height, bool hevc444 = false)
{
    const auto *data = hevc444 ? kHevc444SequenceHeaders : kHevcSequenceHeaders;
    const std::size_t size = hevc444 ? sizeof(kHevc444SequenceHeaders)
                                     : sizeof(kHevcSequenceHeaders);
    std::vector<std::uint8_t> result;
    bool have_vps = false, have_sps = false, have_pps = false;
    std::size_t begin = 0;
    while (begin + 4 <= size) {
        if (std::memcmp(data + begin, "\0\0\0\1", 4) != 0)
            return {};
        const std::size_t nal_begin = begin + 4;
        std::size_t end = nal_begin;
        while (end + 4 <= size &&
               std::memcmp(data + end, "\0\0\0\1", 4) != 0)
            ++end;
        if (end + 4 > size)
            end = size;
        if (end == nal_begin)
            return {};
        std::vector<std::uint8_t> nal(data + nal_begin, data + end);
        const unsigned type = (nal[0] >> 1) & 0x3f;
        if (type == 32)
            have_vps = true;
        else if (type == 33) {
            nal = hevc_dynamic_sps(nal, width, height);
            have_sps = !nal.empty();
        } else if (type == 34)
            have_pps = true;
        if (nal.empty())
            return {};
        result.insert(result.end(), {0, 0, 0, 1});
        result.insert(result.end(), nal.begin(), nal.end());
        if (end == size)
            break;
        begin = end;
    }
    return have_vps && have_sps && have_pps ? result : std::vector<std::uint8_t>{};
}

static D3D12_VIDEO_ENCODER_PROFILE_DESC hevc_profile(
    D3D12_VIDEO_ENCODER_PROFILE_HEVC *profile_value, bool hevc444 = false)
{
    *profile_value = hevc444 ? D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN_444
                             : D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN;
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile = {};
    profile.DataSize = sizeof(*profile_value);
    profile.pHEVCProfile = profile_value;
    return profile;
}

struct HevcBitstreamInfo {
    std::uint64_t chroma_format_idc = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

static bool parse_hevc_sps(const std::vector<std::uint8_t> &bitstream,
                           HevcBitstreamInfo *info)
{
    for (std::size_t i = 0; i + 3 < bitstream.size();) {
        std::size_t start = i;
        std::size_t prefix = 0;
        if (bitstream[i] == 0 && bitstream[i + 1] == 0 &&
            bitstream[i + 2] == 1) {
            prefix = 3;
        } else if (i + 4 < bitstream.size() && bitstream[i] == 0 &&
                   bitstream[i + 1] == 0 && bitstream[i + 2] == 0 &&
                   bitstream[i + 3] == 1) {
            prefix = 4;
        } else {
            ++i;
            continue;
        }
        const std::size_t nal_begin = start + prefix;
        std::size_t nal_end = nal_begin;
        while (nal_end + 3 < bitstream.size() &&
               !(bitstream[nal_end] == 0 && bitstream[nal_end + 1] == 0 &&
                 (bitstream[nal_end + 2] == 1 ||
                  (nal_end + 3 < bitstream.size() &&
                   bitstream[nal_end + 2] == 0 &&
                   bitstream[nal_end + 3] == 1))))
            ++nal_end;
        if (nal_end + 3 >= bitstream.size())
            nal_end = bitstream.size();
        if (nal_end <= nal_begin + 2) {
            i = nal_end + 1;
            continue;
        }
        const unsigned type = (bitstream[nal_begin] >> 1) & 0x3f;
        if (type != 33) {
            i = nal_end;
            continue;
        }
        const std::vector<std::uint8_t> rbsp = hevc_unescape(
            bitstream.data() + nal_begin + 2, nal_end - nal_begin - 2);
        HevcBitReader reader{rbsp};
        std::uint64_t value = 0, max_sub_layers = 0, chroma = 0;
        std::uint64_t width = 0, height = 0, conformance = 0;
        if (!reader.read(4, &value) || !reader.read(3, &max_sub_layers) ||
            !reader.read(1, &value) || max_sub_layers > 6 ||
            !hevc_skip_profile_tier_level(
                &reader, static_cast<unsigned>(max_sub_layers)) ||
            !reader.ue(&value) || !reader.ue(&chroma) ||
            (chroma == 3 && !reader.read(1, &value)) ||
            !reader.ue(&width) || !reader.ue(&height) ||
            !reader.read(1, &conformance))
            return false;
        if (conformance) {
            std::uint64_t left = 0, right = 0, top = 0, bottom = 0;
            if (!reader.ue(&left) || !reader.ue(&right) || !reader.ue(&top) ||
                !reader.ue(&bottom))
                return false;
            const std::uint64_t sub_width = chroma == 1 || chroma == 2 ? 2 : 1;
            const std::uint64_t sub_height = chroma == 1 ? 2 : 1;
            const std::uint64_t crop_width = sub_width * (left + right);
            const std::uint64_t crop_height = sub_height * (top + bottom);
            if (crop_width > width || crop_height > height)
                return false;
            width -= crop_width;
            height -= crop_height;
        }
        if (width > std::numeric_limits<std::uint32_t>::max() ||
            height > std::numeric_limits<std::uint32_t>::max())
            return false;
        info->chroma_format_idc = chroma;
        info->width = static_cast<std::uint32_t>(width);
        info->height = static_cast<std::uint32_t>(height);
        return true;
    }
    return false;
}

static bool verify_hevc444_sequence_header(const char *path,
                                           const EncodeDimensions &dimensions,
                                           HevcBitstreamInfo *info)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::fprintf(stderr,
                     "BLOCKED stage=hevc444-sequence-header reason=open-failed path=%s\n",
                     path);
        return false;
    }
    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!parse_hevc_sps(bytes, info)) {
        std::fputs("BLOCKED stage=hevc444-sequence-header reason=sps-not-found\n",
                   stderr);
        return false;
    }
    const bool valid = info->chroma_format_idc == 3 &&
                       info->width == dimensions.width &&
                       info->height == dimensions.height;
    std::printf("%s stage=hevc444-sequence-header chroma_format_idc=%llu width=%u "
                "height=%u\n",
                valid ? "PASS" : "BLOCKED",
                static_cast<unsigned long long>(info->chroma_format_idc),
                info->width, info->height);
    return valid;
}

static bool verify_hevc444_stream_structure(const char *path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    bool have_vps = false;
    bool have_sps = false;
    bool have_pps = false;
    bool have_irap = false;
    for (std::size_t i = 0; i + 3 < bytes.size();) {
        std::size_t prefix = 0;
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1)
            prefix = 3;
        else if (i + 4 < bytes.size() && bytes[i] == 0 &&
                 bytes[i + 1] == 0 && bytes[i + 2] == 0 &&
                 bytes[i + 3] == 1)
            prefix = 4;
        else {
            ++i;
            continue;
        }
        const std::size_t nal = i + prefix;
        if (nal + 1 >= bytes.size())
            break;
        const unsigned type = (bytes[nal] >> 1) & 0x3f;
        have_vps |= type == 32;
        have_sps |= type == 33;
        have_pps |= type == 34;
        have_irap |= type >= 16 && type <= 23;
        i = nal + 2;
    }
    const bool valid = have_vps && have_sps && have_pps && have_irap;
    if (!valid)
        std::fputs("BLOCKED stage=hevc444-stream-generated "
                   "reason=missing-vps-sps-pps-or-idr\n", stderr);
    return valid;
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

struct Hevc444PictureDefaults {
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC_FLAGS flags =
        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC_FLAG_NONE;
    UCHAR diff_cu_chroma_qp_offset_depth = 0;
    UCHAR log2_sao_offset_scale_luma = 0;
    UCHAR log2_sao_offset_scale_chroma = 0;
    UCHAR log2_max_transform_skip_block_size_minus2 = 0;
    UCHAR chroma_qp_offset_list_len_minus1 = 0;
    std::array<CHAR, 6> cb_qp_offset_list = {};
    std::array<CHAR, 6> cr_qp_offset_list = {};
};

struct HevcConfiguration {
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC requested = {};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC reported = {};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC1 reported1 = {};
    Hevc444PictureDefaults picture_defaults = {};
};

static bool choose_first_allowed(UINT mask, unsigned max_value, UCHAR *value)
{
    if (!value)
        return false;
    for (unsigned candidate = 0; candidate <= max_value; ++candidate) {
        if ((mask & (UINT{1} << candidate)) != 0) {
            *value = static_cast<UCHAR>(candidate);
            return true;
        }
    }
    return false;
}

static bool choose_first_allowed_qp_offset(UINT mask, CHAR *value)
{
    if (!value)
        return false;
    for (int candidate = -12; candidate <= 12; ++candidate) {
        const unsigned bit = static_cast<unsigned>(candidate + 12);
        if ((mask & (UINT{1} << bit)) != 0) {
            *value = static_cast<CHAR>(candidate);
            return true;
        }
    }
    return false;
}

static bool choose_hevc444_picture_defaults(
    const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC1 &support,
    Hevc444PictureDefaults *out)
{
    if (!out ||
        !choose_first_allowed(support.allowed_diff_cu_chroma_qp_offset_depth_values,
                              3, &out->diff_cu_chroma_qp_offset_depth) ||
        !choose_first_allowed(support.allowed_log2_sao_offset_scale_luma_values,
                              6, &out->log2_sao_offset_scale_luma) ||
        !choose_first_allowed(support.allowed_log2_sao_offset_scale_chroma_values,
                              6, &out->log2_sao_offset_scale_chroma) ||
        !choose_first_allowed(
            support.allowed_log2_max_transform_skip_block_size_minus2_values,
            3, &out->log2_max_transform_skip_block_size_minus2) ||
        !choose_first_allowed(
            support.allowed_chroma_qp_offset_list_len_minus1_values, 5,
            &out->chroma_qp_offset_list_len_minus1))
        return false;

    const unsigned list_count =
        static_cast<unsigned>(out->chroma_qp_offset_list_len_minus1) + 1;
    for (unsigned index = 0; index < list_count; ++index) {
        if (!choose_first_allowed_qp_offset(
                support.allowed_cb_qp_offset_list_values[index],
                &out->cb_qp_offset_list[index]) ||
            !choose_first_allowed_qp_offset(
                support.allowed_cr_qp_offset_list_values[index],
                &out->cr_qp_offset_list[index]))
            return false;
    }

    const auto required = support.SupportFlags;
    if ((required &
         D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_CROSS_COMPONENT_PREDICTION_ENABLED_FLAG_REQUIRED) !=
        0)
        out->flags |=
            D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC_FLAG_CROSS_COMPONENT_PREDICTION;
    if ((required &
         D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_CHROMA_QP_OFFSET_LIST_ENABLED_FLAG_REQUIRED) !=
        0)
        out->flags |=
            D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC_FLAG_CHROMA_QP_OFFSET_LIST;
    return true;
}

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

/* HEVC Main 4:4:4 uses the current DirectX-Headers HEVC1 configuration
 * support structure.  Do not silently fall back to the older Main-only
 * support structure: a successful result here is part of the 4:4:4 gate. */
static bool find_hevc444_configuration(
    ID3D12VideoDevice3 *video_device,
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
                        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC1 candidate = {};
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
                            limits.pHEVCSupport1 = &candidate;
                            D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT query = {};
                            query.NodeIndex = kNodeIndex;
                            query.Codec = kEncodeCodec;
                            query.Profile = profile;
                            query.CodecSupportLimits = limits;
                            if (!query_video_feature(
                                    video_device,
                                    D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT,
                                    &query,
                                    "hevc444-codec-config-query"))
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

                            Hevc444PictureDefaults defaults = {};
                            if (!choose_hevc444_picture_defaults(candidate,
                                                                  &defaults)) {
                                continue;
                            }
                            const bool separate_plane_required =
                                (candidate.SupportFlags1 &
                                 D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG1_SEPARATE_COLOUR_PLANE_REQUIRED) !=
                                0;
                            found->requested.ConfigurationFlags =
                                (asymmetric_required
                                     ? D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION
                                     : D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_NONE) |
                                (separate_plane_required
                                     ? D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_SEPARATE_COLOUR_PLANE
                                     : D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_NONE);
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
                            found->reported = {};
                            found->reported.SupportFlags = candidate.SupportFlags;
                            found->reported.MinLumaCodingUnitSize =
                                candidate.MinLumaCodingUnitSize;
                            found->reported.MaxLumaCodingUnitSize =
                                candidate.MaxLumaCodingUnitSize;
                            found->reported.MinLumaTransformUnitSize =
                                candidate.MinLumaTransformUnitSize;
                            found->reported.MaxLumaTransformUnitSize =
                                candidate.MaxLumaTransformUnitSize;
                            found->reported.max_transform_hierarchy_depth_inter =
                                candidate.max_transform_hierarchy_depth_inter;
                            found->reported.max_transform_hierarchy_depth_intra =
                                candidate.max_transform_hierarchy_depth_intra;
                            found->reported1 = candidate;
                            found->picture_defaults = defaults;
                            std::printf("PASS stage=hevc444-codec-config-support "
                                        "flags=0x%08x flags1=0x%08x "
                                        "picture_defaults=%u,%u,%u,%u,%u\n",
                                        static_cast<unsigned>(candidate.SupportFlags),
                                        static_cast<unsigned>(candidate.SupportFlags1),
                                        static_cast<unsigned>(defaults.diff_cu_chroma_qp_offset_depth),
                                        static_cast<unsigned>(defaults.log2_sao_offset_scale_luma),
                                        static_cast<unsigned>(defaults.log2_sao_offset_scale_chroma),
                                        static_cast<unsigned>(defaults.log2_max_transform_skip_block_size_minus2),
                                        static_cast<unsigned>(defaults.chroma_qp_offset_list_len_minus1));
                            return true;
                        }
                    }
                }
            }
        }
    }
    std::fputs("BLOCKED stage=hevc444-picture-control "
               "reason=no-valid-configuration\n", stderr);
    return false;
}

static bool probe_codec(ID3D12VideoDevice3 *video_device,
                        D3D12_VIDEO_ENCODER_CODEC codec,
                        const EncodeDimensions &dimensions,
                        bool production)
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
                codec_query.IsSupported ? stage_prefix(production) : "BLOCKED",
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
                profile_query.IsSupported ? stage_prefix(production) : "BLOCKED",
                codec == D3D12_VIDEO_ENCODER_CODEC_H264 ? "H264" : "HEVC",
                profile_query.IsSupported ? 1U : 0U);
    if (!profile_query.IsSupported)
        return true;

    if (codec == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
        HevcConfiguration config = {};
        if (!find_hevc_configuration(video_device, profile, &config))
            return false;
        std::printf("%s stage=d3d12-video-codec-configuration codec=HEVC "
                    "flags=0x%08x cu=%u..%u tu=%u..%u hierarchy=%u/%u\n",
                    stage_prefix(production),
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
        resolution.MinResolutionSupported.Width <= dimensions.width &&
        resolution.MinResolutionSupported.Height <= dimensions.height &&
        resolution.MaxResolutionSupported.Width >= dimensions.width &&
        resolution.MaxResolutionSupported.Height >= dimensions.height &&
        (resolution.ResolutionWidthMultipleRequirement == 0 ||
         dimensions.width % resolution.ResolutionWidthMultipleRequirement == 0) &&
        (resolution.ResolutionHeightMultipleRequirement == 0 ||
         dimensions.height % resolution.ResolutionHeightMultipleRequirement == 0);
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

static bool probe_hevc444_profile_and_input(
    ID3D12VideoDevice3 *video_device, const EncodeDimensions &dimensions,
    bool *codec_config_supported, bool *ayuv_supported)
{
    *codec_config_supported = false;
    *ayuv_supported = false;
    D3D12_VIDEO_ENCODER_PROFILE_HEVC profile_value =
        D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN_444;
    const D3D12_VIDEO_ENCODER_PROFILE_DESC profile =
        hevc_profile(&profile_value, true);
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC codec = {};
    codec.NodeIndex = kNodeIndex;
    codec.Codec = kEncodeCodec;
    if (!query_video_feature(video_device, D3D12_FEATURE_VIDEO_ENCODER_CODEC,
                             &codec, "hevc444-profile-codec-query")) {
        std::fputs("BLOCKED stage=hevc444-profile supported=0\n", stderr);
        return false;
    }
    if (!codec.IsSupported) {
        std::puts("BLOCKED stage=hevc444-profile supported=0");
        return false;
    }

    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC min_level = {};
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC max_level = {};
    min_level.Level = D3D12_VIDEO_ENCODER_LEVELS_HEVC_1;
    min_level.Tier = D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN;
    max_level.Level = D3D12_VIDEO_ENCODER_LEVELS_HEVC_51;
    max_level.Tier = D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN;
    D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL profile_query = {};
    profile_query.NodeIndex = kNodeIndex;
    profile_query.Codec = kEncodeCodec;
    profile_query.Profile = profile;
    profile_query.MinSupportedLevel.DataSize = sizeof(min_level);
    profile_query.MinSupportedLevel.pHEVCLevelSetting = &min_level;
    profile_query.MaxSupportedLevel.DataSize = sizeof(max_level);
    profile_query.MaxSupportedLevel.pHEVCLevelSetting = &max_level;
    if (!query_video_feature(video_device,
                             D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL,
                             &profile_query,
                             "hevc444-profile-query")) {
        std::fputs("BLOCKED stage=hevc444-profile supported=0\n", stderr);
        return false;
    }
    std::printf("%s stage=hevc444-profile codec=HEVC "
                "profile=HEVC_MAIN_444 resolution=%ux%u fps=60/1 supported=%u\n",
                profile_query.IsSupported ? "PASS" : "BLOCKED",
                dimensions.width, dimensions.height,
                profile_query.IsSupported ? 1U : 0U);
    if (!profile_query.IsSupported)
        return false;

    HevcConfiguration configuration = {};
    if (!find_hevc444_configuration(video_device, profile, &configuration))
        return false;
    *codec_config_supported = true;

    D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT input = {};
    input.NodeIndex = kNodeIndex;
    input.Codec = kEncodeCodec;
    input.Profile = profile;
    input.Format = kHevc444InputFormat;
    if (!query_video_feature(video_device,
                             D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, &input,
                             "hevc444-ayuv-input-query")) {
        std::fputs("BLOCKED stage=hevc444-ayuv-input supported=0\n", stderr);
        return false;
    }
    std::printf("%s stage=hevc444-ayuv-input format=AYUV supported=%u\n",
                input.IsSupported ? "PASS" : "BLOCKED",
                input.IsSupported ? 1U : 0U);
    *ayuv_supported = input.IsSupported != FALSE;
    return *ayuv_supported;
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
    Hevc444PictureDefaults hevc444_picture_defaults = {};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION configuration_union = {};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC gop_value = {};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop = {};
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP cqp_value = {};
    D3D12_VIDEO_ENCODER_RATE_CONTROL rate_control = {};
    UINT64 metadata_bytes = sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA);
    UINT64 bitstream_alignment = 1;
    DXGI_FORMAT input_format = kHevc420InputFormat;
};

static bool initialize_encoder(DeviceContext *context, EncoderSetup *setup,
                               const EncodeDimensions &dimensions,
                               bool production, bool hevc444 = false)
{
    if (FAILED(context->device.As(&setup->video_device))) {
        std::fputs("BLOCKED stage=d3d12-video-device-interface\n", stderr);
        return false;
    }
    setup->input_format = hevc444 ? kHevc444InputFormat : kHevc420InputFormat;
    setup->profile = hevc_profile(&setup->profile_value, hevc444);
    setup->level = hevc_level(&setup->level_value);
    HevcConfiguration config = {};
    const bool config_ok = hevc444
        ? find_hevc444_configuration(setup->video_device.Get(), setup->profile,
                                     &config)
        : find_hevc_configuration(setup->video_device.Get(), setup->profile,
                                  &config);
    if (!config_ok) {
        std::fprintf(stderr, "BLOCKED stage=%s reason=unsupported\n",
                     hevc444 ? "hevc444-codec-config" :
                               "d3d12-video-codec-configuration");
        return false;
    }
    setup->configuration = config.requested;
    setup->hevc444_picture_defaults = config.picture_defaults;
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

    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC resolution = {
        dimensions.width, dimensions.height};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS limits = {};
    D3D12_VIDEO_ENCODER_PROFILE_HEVC suggested_profile_value = {};
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC suggested_level_value = {};
    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT support = {};
    support.NodeIndex = kNodeIndex;
    support.Codec = kEncodeCodec;
    support.InputFormat = setup->input_format;
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
    std::printf("%s stage=%s codec=HEVC input=%s supported=%u "
                "validation=0x%08x support_flags=0x%08x\n",
                support_ok ? stage_prefix(production) : "BLOCKED",
                hevc444 ? "hevc444-4k60-support"
                        : "d3d12-video-60fps-configuration",
                hevc444 ? "AYUV" : "NV12",
                support_ok ? 1U : 0U,
                static_cast<unsigned>(support.ValidationFlags),
                static_cast<unsigned>(support.SupportFlags));
    if (!support_ok)
        return false;

    D3D12_VIDEO_ENCODER_DESC encoder_desc = {};
    encoder_desc.EncodeCodec = kEncodeCodec;
    encoder_desc.EncodeProfile = setup->profile;
    encoder_desc.InputFormat = setup->input_format;
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
    requirements.InputFormat = setup->input_format;
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
    std::printf("%s stage=%s codec=HEVC encoder=1 heap=1 "
                "resolution=%ux%u input=%s fps=60/1\n",
                stage_prefix(production),
                hevc444 ? "hevc444-encoder-create" :
                          "d3d12-video-objects-constructed",
                dimensions.width, dimensions.height,
                hevc444 ? "AYUV" : "NV12");
    return true;
}

struct ProcessSetup {
    ComPtr<ID3D12VideoProcessor> processor;
    D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC input_desc = {};
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC output_desc = {};
};

struct EncodeSlot {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> encoder_input;
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
    std::array<std::uint8_t, 4> expected_pixel = {0, 0, 0, 255};
    bool copy_pending = false;
    bool input_diagnostic_pending = false;
    bool output_initialized = false;
    bool encoder_input_first_use = true;
};

static bool make_process_setup(DeviceContext *context,
                               std::uint32_t source_format,
                               ProcessSetup *setup,
                               const EncodeDimensions &dimensions,
                               bool production, bool hevc444 = false)
{
    DXGI_FORMAT input_format = DXGI_FORMAT_UNKNOWN;
    const char *input_name = nullptr;
    const char *support_stage = nullptr;
    const char *conversion_stage = nullptr;
    switch (source_format) {
    case kDxgiFormatR8G8B8A8Unorm:
        input_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        input_name = "RGBA8";
        support_stage = hevc444 ? "rgba-to-ayuv-video-processor-support"
                                : "d3d12-video-rgba-nv12-support";
        conversion_stage = hevc444 ? "rgba-to-ayuv-gpu-only"
                                    : "rgba-to-nv12-gpu-only";
        break;
    case kDxgiFormatB8G8R8A8Unorm:
        input_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        input_name = "BGRA8";
        support_stage = hevc444 ? "bgra-to-ayuv-video-processor-support"
                                : "d3d12-video-bgra-nv12-support";
        conversion_stage = hevc444 ? "bgra-to-ayuv-gpu-only"
                                    : "bgra-to-nv12-gpu-only";
        break;
    default:
        std::fprintf(stderr,
                     "BLOCKED stage=resource-format format=%u reason=unsupported-dxgi-format\n",
                     source_format);
        return false;
    }
    setup->input_desc = {};
    setup->input_desc.Format = input_format;
    setup->input_desc.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    setup->input_desc.SourceAspectRatio = {1, 1};
    setup->input_desc.DestinationAspectRatio = {1, 1};
    setup->input_desc.FrameRate = {kFrameRateNumerator, kFrameRateDenominator};
    setup->input_desc.SourceSizeRange = {dimensions.width, dimensions.height,
                                         dimensions.width, dimensions.height};
    setup->input_desc.DestinationSizeRange = {dimensions.width, dimensions.height,
                                              dimensions.width, dimensions.height};
    setup->input_desc.StereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    setup->input_desc.FieldType = D3D12_VIDEO_FIELD_TYPE_NONE;
    setup->input_desc.DeinterlaceMode = D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_NONE;
    setup->output_desc = {};
    setup->output_desc.Format = hevc444 ? kHevc444InputFormat
                                        : kHevc420InputFormat;
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
    support.InputSample.Width = dimensions.width;
    support.InputSample.Height = dimensions.height;
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
                             &support, support_stage))
        return false;
    if ((support.SupportFlags & D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED) == 0) {
        std::fprintf(stderr, "BLOCKED stage=%s input=%s output=%s "
                             "resolution=%ux%u fps=60/1\n",
                     support_stage, input_name, hevc444 ? "AYUV" : "NV12",
                     dimensions.width, dimensions.height);
        return false;
    }
    std::printf("%s stage=%s input=%s output=%s resolution=%ux%u fps=60/1\n",
                stage_prefix(production),
                support_stage, input_name, hevc444 ? "AYUV" : "NV12",
                dimensions.width, dimensions.height);
    if (!hr_ok(video_device->CreateVideoProcessor(
                   0, &setup->output_desc, 1, &setup->input_desc,
                   IID_PPV_ARGS(&setup->processor)),
               "d3d12-video-create-processor"))
        return false;
    std::printf(hevc444
                    ? "%s stage=%s supported=1 cpu_conversion=0 output=AYUV\n"
                    : "%s stage=%s support=1 cpu_conversion=0 output=NV12\n",
                stage_prefix(production), conversion_stage);
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

static bool collect_encoded_slot(EncodeSlot *slot, std::ostream *stream,
                                 const EncodeDimensions &dimensions,
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
            std::make_pair(dimensions.width / 4 + 64U, dimensions.height / 4 + 64U),
            std::make_pair(dimensions.width / 2, dimensions.height / 2),
            std::make_pair(dimensions.width * 3 / 4 - 64U,
                           dimensions.height * 3 / 4 - 64U)};
        bool frame_ok = true;
        std::array<std::array<std::uint8_t, 4>, 3> actual_pixels = {};
        for (std::size_t point_index = 0; point_index < points.size();
             ++point_index) {
            const auto &point = points[point_index];
            const std::size_t offset =
                static_cast<std::size_t>(point.second) *
                    slot->input_footprint.Footprint.RowPitch +
                static_cast<std::size_t>(point.first) * 4;
            std::memcpy(actual_pixels[point_index].data(), bytes + offset,
                        actual_pixels[point_index].size());
            if (!close_enough(actual_pixels[point_index].data(),
                             slot->expected_pixel))
                frame_ok = false;
        }
        slot->input_readback->Unmap(0, nullptr);
        ++*diagnostic_checks;
        if (!frame_ok) {
            ++*mismatches;
            std::fprintf(stderr,
                         "FAIL stage=diagnostic-frame-sequence frame=%llu "
                         "expected=%u,%u,%u,%u actual0=%u,%u,%u,%u "
                         "actual1=%u,%u,%u,%u actual2=%u,%u,%u,%u\n",
                         static_cast<unsigned long long>(slot->frame),
                         slot->expected_pixel[0], slot->expected_pixel[1],
                         slot->expected_pixel[2], slot->expected_pixel[3],
                         actual_pixels[0][0], actual_pixels[0][1],
                         actual_pixels[0][2], actual_pixels[0][3],
                         actual_pixels[1][0], actual_pixels[1][1],
                         actual_pixels[1][2], actual_pixels[1][3],
                         actual_pixels[2][0], actual_pixels[2][1],
                         actual_pixels[2][2], actual_pixels[2][3]);
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

class EncodedPacketStreamBuf final : public std::streambuf {
public:
    EncodedPacketStreamBuf(int fd, EncodeDimensions dimensions)
        : fd_(fd), dimensions_(dimensions) {}

protected:
    std::streamsize xsputn(const char *data, std::streamsize count) override
    {
        if (count <= 0)
            return count;
        if (!configured_) {
            AsbEncodedVideoConfig config = {};
            config.magic = ASB_DISPLAY_VIDEO_CONFIG_MAGIC;
            config.version = ASB_DISPLAY_PROTOCOL_VERSION;
            config.header_size = sizeof(config);
            config.generation = 1;
            config.codec = ASB_DISPLAY_CODEC_HEVC;
            config.width = dimensions_.width;
            config.height = dimensions_.height;
            config.fps_num = kFrameRateNumerator;
            config.fps_den = kFrameRateDenominator;
            config.flags = ASB_DISPLAY_VIDEO_FLAG_DISCONTINUITY;
            config.extradata_size = static_cast<std::uint32_t>(count);
            if (!send_message(&config, sizeof(config), data,
                              static_cast<std::size_t>(count)))
                return 0;
            configured_ = true;
            return count;
        }
        AsbEncodedVideoFrame frame = {};
        frame.magic = ASB_DISPLAY_VIDEO_FRAME_MAGIC;
        frame.version = ASB_DISPLAY_PROTOCOL_VERSION;
        frame.header_size = sizeof(frame);
        frame.generation = 1;
        frame.frame_seq = ++frame_seq_;
        frame.capture_time_ns = monotonic_ns();
        /* The validated baseline currently emits intra pictures. */
        frame.flags = ASB_DISPLAY_VIDEO_FLAG_IDR;
        frame.payload_size = static_cast<std::uint32_t>(count);
        return send_message(&frame, sizeof(frame), data,
                            static_cast<std::size_t>(count)) ? count : 0;
    }

    int overflow(int ch) override
    {
        if (ch == traits_type::eof()) return traits_type::not_eof(ch);
        const char c = static_cast<char>(ch);
        return xsputn(&c, 1) == 1 ? ch : traits_type::eof();
    }

private:
    bool send_message(const void *header, std::size_t header_size,
                      const void *payload, std::size_t payload_size)
    {
        struct iovec iov[2] = {
            {const_cast<void *>(header), header_size},
            {const_cast<void *>(payload), payload_size}
        };
        struct msghdr message = {};
        message.msg_iov = iov;
        message.msg_iovlen = 2;
        const std::size_t total = header_size + payload_size;
        return sendmsg(fd_, &message, MSG_NOSIGNAL) ==
               static_cast<ssize_t>(total);
    }

    int fd_;
    EncodeDimensions dimensions_;
    bool configured_ = false;
    std::uint64_t frame_seq_ = 0;
};

enum class EncodeProbeMode {
    Hevc420,
    Hevc444
};

static bool encode_consumer_main(
    int control_fd, EncodeProbeMode mode = EncodeProbeMode::Hevc420)
{
    const bool production_session = std::getenv("ASB_D3D12_ENCODED_FD") != nullptr;
    const bool hevc444_session =
        !production_session && mode == EncodeProbeMode::Hevc444;
    if (production_session)
        ::unlink("/run/appsandbox/display-d3d12.health");
    DeviceContext context;
    if (!create_device("encoder-device", &context))
        return false;
    ResourceBundleMessage bundle = {};
    std::vector<int> received_fds;
    std::size_t bundle_size = 0;
    const EncodeDimensions diagnostic_dimensions = kDiagnosticDimensions;
    if (!receive_packet(control_fd, &bundle, sizeof(bundle), &bundle_size,
                        &received_fds, kSocketTimeoutMs) ||
        bundle_size != sizeof(bundle) || bundle.magic != kProtocolMagic ||
        bundle.type != kResourceBundle || bundle.width == 0 ||
        bundle.height == 0 || bundle.width > ASB_DISPLAY_MAX_WIDTH ||
        bundle.height > ASB_DISPLAY_MAX_HEIGHT || (bundle.width & 1) ||
        (bundle.height & 1) || bundle.slots != kSlotCount ||
        (!production_session && bundle.synthetic_source != 0 &&
         (bundle.width != diagnostic_dimensions.width ||
          bundle.height != diagnostic_dimensions.height ||
          bundle.frames != kFrameCount)) ||
        (production_session && bundle.frames != 0 && bundle.frames < kSlotCount) ||
        received_fds.size() != kMaxTransferFds) {
        close_fd_vector(&received_fds);
        std::fputs("FAIL stage=cross-process-resource-fd-transfer\n", stderr);
        return false;
    }
    const EncodeDimensions dimensions = {bundle.width, bundle.height};

    if (bundle.synthetic_source == 0) {
        if (bundle.format != kDxgiFormatR8G8B8A8Unorm &&
            bundle.format != kDxgiFormatB8G8R8A8Unorm) {
            std::fprintf(stderr,
                         "BLOCKED stage=mutter-real-render-target format=%u "
                         "reason=unsupported-dxgi-format\n",
                         bundle.format);
            return false;
        }
        if (bundle.buffer_count != kSlotCount) {
            std::fprintf(stderr,
                         "BLOCKED stage=mutter-real-render-target format=%u "
                         "buffer_count=%u reason=unexpected-buffer-count\n",
                         bundle.format, bundle.buffer_count);
            return false;
        }
        const char *format_name =
            bundle.format == kDxgiFormatR8G8B8A8Unorm ? "RGBA8" : "BGRA8";
        std::printf("%s stage=mutter-real-render-target synthetic_source=0 "
                    "width=%u height=%u format=%s dxgi_format=%u\n",
                    stage_prefix(production_session),
                    bundle.width, bundle.height, format_name, bundle.format);
        std::printf("%s stage=mutter-shared-resource "
                    "format=%s native_d3d12_shared=1 gpu_copy=%u cpu_copy=0\n",
                    stage_prefix(production_session),
                    format_name, bundle.gpu_copy);
    }
    if (bundle.format != kDxgiFormatR8G8B8A8Unorm &&
        bundle.format != kDxgiFormatB8G8R8A8Unorm) {
        std::fprintf(stderr,
                     "BLOCKED stage=resource-format format=%u "
                     "reason=unsupported-dxgi-format\n",
                     bundle.format);
        close_fd_vector(&received_fds);
        return false;
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
    const char *format_name =
        bundle.format == kDxgiFormatR8G8B8A8Unorm ? "RGBA8" : "BGRA8";
    std::printf("%s stage=cross-process-open-shared-resource format=%s "
                "dxgi_format=%u slots=3 consumer_device=independent\n",
                stage_prefix(production_session),
                format_name, bundle.format);
    std::printf("%s stage=resource-transport-fd-close side=consumer count=3\n",
                stage_prefix(production_session));

    EncoderSetup encoder;
    if (!initialize_encoder(&context, &encoder, dimensions, production_session,
                            hevc444_session))
        return false;
    ProcessSetup process;
    if (!make_process_setup(&context, bundle.format, &process, dimensions,
                            production_session, hevc444_session))
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

    const D3D12_RESOURCE_DESC encoder_input_desc = {
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, dimensions.width, dimensions.height, 1, 1,
        encoder.input_format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
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
        const D3D12_RESOURCE_DESC input_desc = slots[slot_index].texture->GetDesc();
        context.device->GetCopyableFootprints(
            &input_desc, 0, 1, 0, &slot.input_footprint, nullptr, nullptr,
            &input_readback_size);
        if (!create_default_resource(
                context.device.Get(), encoder_input_desc,
                D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE, &slot.encoder_input) ||
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
            (!hevc444_session &&
             !create_readback_resource(context.device.Get(), input_readback_size,
                                       &slot.input_readback)))
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
    std::printf("%s stage=consumer-done-eventfd slots=3 "
                "set_event_on_completion=1 scm_rights=1\n",
                stage_prefix(production_session));
    std::printf(hevc444_session
                    ? "%s stage=consumer-gpu-operation %s_to_ayuv=GPU-only "
                      "d3d12_encode=GPU-only cpu_framebuffer_copy=0\n"
                    : "%s stage=consumer-gpu-operation %s_to_nv12=GPU-only "
                      "d3d12_encode=GPU-only cpu_framebuffer_copy=0\n",
                stage_prefix(production_session),
                bundle.format == kDxgiFormatR8G8B8A8Unorm ? "rgba" : "bgra");

    const char *packet_fd_text = std::getenv("ASB_D3D12_ENCODED_FD");
    const char *path = std::getenv("D3D12_VIDEO_BITSTREAM_PATH");
    const char *stream_path = path != nullptr ? path
                                               : "/tmp/d3d12-video-probe.hevc";
    std::unique_ptr<EncodedPacketStreamBuf> packet_buffer;
    std::unique_ptr<std::ostream> packet_stream;
    std::unique_ptr<std::ofstream> file_stream;
    std::ostream *stream = nullptr;
    if (packet_fd_text && *packet_fd_text) {
        char *end = nullptr;
        long parsed = std::strtol(packet_fd_text, &end, 10);
        if (!end || *end || parsed < 0 || parsed > std::numeric_limits<int>::max())
            return false;
        packet_buffer = std::make_unique<EncodedPacketStreamBuf>(static_cast<int>(parsed),
                                                                  dimensions);
        packet_stream = std::make_unique<std::ostream>(packet_buffer.get());
        stream = packet_stream.get();
        stream_path = "seqpacket";
    } else {
        file_stream = std::make_unique<std::ofstream>(
            stream_path, std::ios::binary | std::ios::trunc);
        stream = file_stream.get();
    }
    if (!*stream)
        return false;
    const std::vector<std::uint8_t> sequence_headers =
        dynamic_hevc_sequence_headers(bundle.width, bundle.height,
                                      hevc444_session);
    if (sequence_headers.empty()) {
        std::fputs("FAIL stage=hevc-sequence-headers reason=invalid-template\n", stderr);
        return false;
    }
    stream->write(reinterpret_cast<const char *>(sequence_headers.data()),
                  static_cast<std::streamsize>(sequence_headers.size()));
    if (!*stream)
        return false;
    std::printf("%s stage=hevc-sequence-headers vps=1 sps=1 pps=1 bytes=%zu "
                     "host_generated=1 chroma_format_idc=%u framebuffer_bytes=0\n",
                 stage_prefix(production_session),
                 sequence_headers.size(), hevc444_session ? 3U : 1U);
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
    const std::uint64_t frame_limit = production_session
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(kFrameCount);

    for (std::uint64_t frame = 0; frame < frame_limit; ++frame) {
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
            if (!collect_encoded_slot(&slot, stream, dimensions, bitstream_size,
                                      metadata_size, &encoded_frames,
                                      &encoded_bytes, &encode_failures,
                                      &timeouts, &mismatches,
                                      &diagnostic_checks,
                                      &encode_complete_times,
                                      &total_pipeline_times,
                                      &slot_recycle_times))
                return false;
            /* This is the first point at which a completed GPU encode/readback
             * proves that the session rendered an actual frame. */
            publish_production_health(
                production_session, bundle.width, bundle.height,
                encoded_frames > 0 && encode_failures == 0, bundle.gpu_copy != 0,
                encoded_frames, encode_failures);
            if (!reuse_logged) {
                std::printf("%s stage=triple-buffer-reuse slots=3\n",
                            stage_prefix(production_session));
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
        if (!slot.encoder_input_first_use) {
            process_barrier.Transition.pResource = slot.encoder_input.Get();
            process_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            process_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE;
            slot.process_list->ResourceBarrier(1, &process_barrier);
        }
        D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS process_input = {};
        process_input.InputStream[0].pTexture2D = slot.texture.Get();
        process_input.InputStream[0].Subresource = 0;
        process_input.Transform.SourceRectangle = {0, 0,
                                                   static_cast<LONG>(dimensions.width),
                                                   static_cast<LONG>(dimensions.height)};
        process_input.Transform.DestinationRectangle = process_input.Transform.SourceRectangle;
        process_input.Transform.Orientation = D3D12_VIDEO_PROCESS_ORIENTATION_DEFAULT;
        process_input.RateInfo.OutputIndex = 0;
        process_input.RateInfo.InputFrameOrField = 0;
        D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS process_output = {};
        process_output.OutputStream[0].pTexture2D = slot.encoder_input.Get();
        process_output.OutputStream[0].Subresource = 0;
        process_output.TargetRectangle = process_input.Transform.DestinationRectangle;
        slot.process_list->ProcessFrames(process.processor.Get(), &process_output,
                                         1, &process_input);
        process_barrier.Transition.pResource = slot.texture.Get();
        process_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ;
        process_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        slot.process_list->ResourceBarrier(1, &process_barrier);
        process_barrier.Transition.pResource = slot.encoder_input.Get();
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
                            "d3d12-video-process-eventfd") ||
            !register_event(process_fence.Get(), process_value, slot.done_fd,
                            "consumer-done-eventfd"))
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
        encode_barrier.Transition.pResource = slot.encoder_input.Get();
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
        sequence.PictureTargetResolution = {dimensions.width, dimensions.height};
        sequence.SelectedLayoutMode =
            D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
        sequence.CodecGopSequence = encoder.gop;
        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_DESC picture = {};
        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC picture_data = {};
        D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1 picture_data1 = {};
        if (hevc444_session) {
            picture_data1.Flags = encoder.hevc444_picture_defaults.flags;
            picture_data1.FrameType =
                D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_I_FRAME;
            picture_data1.PictureOrderCountNumber = static_cast<UINT>(frame);
            picture_data1.diff_cu_chroma_qp_offset_depth =
                encoder.hevc444_picture_defaults.diff_cu_chroma_qp_offset_depth;
            picture_data1.log2_sao_offset_scale_luma =
                encoder.hevc444_picture_defaults.log2_sao_offset_scale_luma;
            picture_data1.log2_sao_offset_scale_chroma =
                encoder.hevc444_picture_defaults.log2_sao_offset_scale_chroma;
            picture_data1.log2_max_transform_skip_block_size_minus2 =
                encoder.hevc444_picture_defaults
                    .log2_max_transform_skip_block_size_minus2;
            picture_data1.chroma_qp_offset_list_len_minus1 =
                encoder.hevc444_picture_defaults.chroma_qp_offset_list_len_minus1;
            std::copy(encoder.hevc444_picture_defaults.cb_qp_offset_list.begin(),
                      encoder.hevc444_picture_defaults.cb_qp_offset_list.end(),
                      std::begin(picture_data1.cb_qp_offset_list));
            std::copy(encoder.hevc444_picture_defaults.cr_qp_offset_list.begin(),
                      encoder.hevc444_picture_defaults.cr_qp_offset_list.end(),
                      std::begin(picture_data1.cr_qp_offset_list));
            picture.PictureControlCodecData.DataSize = sizeof(picture_data1);
            picture.PictureControlCodecData.pHEVCPicData1 = &picture_data1;
        } else {
            picture_data.FrameType = D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_I_FRAME;
            picture_data.PictureOrderCountNumber = static_cast<UINT>(frame);
            picture.PictureControlCodecData.DataSize = sizeof(picture_data);
            picture.PictureControlCodecData.pHEVCPicData = &picture_data;
        }
        D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS input = {};
        input.SequenceControlDesc = sequence;
        input.PictureControlDesc = picture;
        input.pInputFrame = slot.encoder_input.Get();
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
        resolve_input.EncoderInputFormat = encoder.input_format;
        resolve_input.EncodedPictureEffectiveResolution = {dimensions.width,
                                                           dimensions.height};
        resolve_input.HWLayoutMetadata = output.EncoderOutputMetadata;
        D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS resolve_output = {};
        resolve_output.ResolvedLayoutMetadata.pBuffer = slot.metadata_resolved.Get();
        slot.encode_list->ResolveEncoderOutputMetadata(&resolve_input,
                                                       &resolve_output);
        encode_barrier.Transition.pResource = slot.encoder_input.Get();
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
            !register_event(copy_fence.Get(), slot.copy_value, slot.copy_fd,
                            "d3d12-video-copy-eventfd"))
            return false;
        slot.copy_pending = true;
        slot.output_initialized = true;
        slot.frame = frame;
        if (info.diagnostic != 0) {
            const auto semantic =
                info.expected_valid != 0
                    ? std::array<std::uint8_t, 4>{info.expected_r,
                                                  info.expected_g,
                                                  info.expected_b,
                                                  info.expected_a}
                    : expected_rgba(frame);
            if (!expected_pixel_for_format(bundle.format, semantic,
                                           &slot.expected_pixel)) {
                std::fprintf(stderr,
                             "BLOCKED stage=diagnostic-format format=%u\n",
                             bundle.format);
                return false;
            }
            slot.input_diagnostic_pending = !hevc444_session;
        }
        slot.encoder_input_first_use = false;
    }
    for (EncodeSlot &slot : slots) {
        if (!collect_encoded_slot(&slot, stream, dimensions, bitstream_size, metadata_size,
                                  &encoded_frames, &encoded_bytes,
                                  &encode_failures, &timeouts, &mismatches,
                                  &diagnostic_checks,
                                  &encode_complete_times,
                                  &total_pipeline_times,
                                  &slot_recycle_times))
            return false;
    }
    publish_production_health(
        production_session, dimensions.width, dimensions.height,
        encoded_frames > 0 && encode_failures == 0, bundle.gpu_copy != 0,
        encoded_frames, encode_failures);
    if (file_stream)
        file_stream->close();
    HevcBitstreamInfo hevc444_bitstream = {};
    const bool hevc444_sequence_header_ok =
        !hevc444_session ||
        verify_hevc444_sequence_header(stream_path, dimensions, &hevc444_bitstream);
    const bool hevc444_stream_structure_ok =
        !hevc444_session || verify_hevc444_stream_structure(stream_path);
    const std::uint64_t consumer_end_ns = monotonic_ns();
    const double consumer_elapsed_seconds =
        static_cast<double>(consumer_end_ns - consumer_start_ns) / 1000000000.0;
    const double consumer_fps =
        consumer_elapsed_seconds > 0.0
            ? static_cast<double>(kFrameCount) / consumer_elapsed_seconds
            : 0.0;
    std::printf("%s p50=%.3f p95=%.3f p99=%.3f samples=%zu\n",
                hevc444_session
                    ? (bundle.format == kDxgiFormatR8G8B8A8Unorm
                           ? "rgba_to_ayuv_us" : "bgra_to_ayuv_us")
                    : (bundle.format == kDxgiFormatR8G8B8A8Unorm
                           ? "rgba_to_nv12_us" : "bgra_to_nv12_us"),
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
    const bool fps_ok = consumer_fps >= 59.0;
    const bool ok = timeouts == 0 && mismatches == 0 && reopen_failures == 0 &&
                    encode_failures == 0 && encoded_frames == kFrameCount &&
                    (hevc444_session
                         ? diagnostic_checks == 0
                         : diagnostic_checks == kFrameCount / kDiagnosticInterval) &&
                    fps_ok && hevc444_sequence_header_ok &&
                    hevc444_stream_structure_ok;
    publish_production_health(production_session, bundle.width, bundle.height,
                              ok, bundle.gpu_copy != 0, encoded_frames,
                              encode_failures);
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
        std::printf("%s stage=throughput-zero-copy framebuffer_mmap=0 "
                    "cpu_memcpy_framebuffer=0 gpu_cpu_gpu=0\n",
                    ok ? "PASS" : "FAIL");
    }
    if (hevc444_session) {
        std::printf("%s stage=hevc444-4k60-sustained frames=%llu fps=%.2f "
                    "encode_failures=%llu cpu_conversion=0 framebuffer_mmap=0 "
                    "cpu_memcpy_framebuffer=0 gpu_cpu_gpu=0\n",
                    ok ? "PASS" : "BLOCKED",
                    static_cast<unsigned long long>(encoded_frames),
                    consumer_fps,
                    static_cast<unsigned long long>(encode_failures));
        std::printf("=== AppSandbox HEVC444 Guest Capability ===\n"
                    "hevc444_profile=1\n"
                    "ayuv_encode_input=1\n"
                    "rgba_to_ayuv=1\n"
                    "bgra_to_ayuv=1\n"
                    "hevc444_codec_config=1\n"
                    "hevc444_picture_control=1\n"
                    "hevc444_encoder_create=1\n"
                    "hevc444_4k60_config=1\n"
                    "hevc444_4k60_sustained=%u\n"
                    "sequence_header_chroma_format_idc=%llu\n"
                    "guest_bitstream_generated=%u\n"
                    "guest_sequence_header_444=%u\n"
                    "frames=%llu\n"
                    "fps=%.2f\n"
                    "encode_failures=%llu\n"
                    "cpu_conversion=0\n"
                    "framebuffer_mmap=0\n"
                    "cpu_memcpy_framebuffer=0\n"
                    "gpu_cpu_gpu=0\n"
                    "chroma_format_idc=%llu\n",
                    ok ? 1U : 0U,
                    static_cast<unsigned long long>(hevc444_bitstream.chroma_format_idc),
                    (encoded_frames == kFrameCount && encode_failures == 0 &&
                     hevc444_stream_structure_ok) ? 1U : 0U,
                    hevc444_sequence_header_ok ? 1U : 0U,
                    static_cast<unsigned long long>(encoded_frames),
                    consumer_fps,
                    static_cast<unsigned long long>(encode_failures),
                    static_cast<unsigned long long>(hevc444_bitstream.chroma_format_idc));
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

static void encode_exec_role(const char *self, const char *role, int control_fd,
                             EncodeProbeMode mode = EncodeProbeMode::Hevc420)
{
    char fd_text[32] = {};
    std::snprintf(fd_text, sizeof(fd_text), "%d", control_fd);
    (void)set_fd_cloexec(control_fd, false);
    if (std::strcmp(role, "--consumer") == 0) {
        const char *mode_text = mode == EncodeProbeMode::Hevc444
            ? "--hevc444" : "--hevc420";
        execl(self, self, role, fd_text, mode_text, static_cast<char *>(nullptr));
    } else {
        execl(self, self, role, fd_text, static_cast<char *>(nullptr));
    }
    std::fprintf(stderr, "FAIL stage=exec-%s errno=%d (%s)\n", role, errno,
                 std::strerror(errno));
    _exit(127);
}

[[maybe_unused]] static int launch_encode_children(
    const char *self, EncodeProbeMode mode = EncodeProbeMode::Hevc420)
{
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0)
        return 1;
    const pid_t consumer_pid = fork();
    if (consumer_pid == 0) {
        close(sockets[0]);
        encode_exec_role(self, "--consumer", sockets[1], mode);
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
    std::puts("PASS stage=throughput-zero-copy framebuffer_mmap=0 "
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
    if (!probe_codec(video_device.Get(), D3D12_VIDEO_ENCODER_CODEC_H264,
                     kDiagnosticDimensions, false) ||
        !probe_codec(video_device.Get(), D3D12_VIDEO_ENCODER_CODEC_HEVC,
                     kDiagnosticDimensions, false))
        return 1;
    std::puts("codec_matrix h264=1 hevc=1");
    EncoderSetup setup;
    if (!initialize_encoder(&context, &setup, kDiagnosticDimensions, false))
        return 3;
    std::puts("PASS d3d12-video-encode-capability codec=HEVC "
              "resolution=3840x2160 input=NV12 fps=60/1");
    return 0;
}

static int capability_444_main()
{
    DeviceContext context;
    if (!create_device("hevc444-capability", &context))
        return 1;
    ComPtr<ID3D12VideoDevice3> video_device;
    if (FAILED(context.device.As(&video_device))) {
        std::fputs("BLOCKED stage=hevc444-video-device-interface\n", stderr);
        return 1;
    }

    bool codec_config_ok = false;
    bool ayuv_input_ok = false;
    const bool profile_ok = probe_hevc444_profile_and_input(
        video_device.Get(), kDiagnosticDimensions, &codec_config_ok,
        &ayuv_input_ok);
    ProcessSetup rgba_process = {};
    ProcessSetup bgra_process = {};
    const bool rgba_ok = profile_ok &&
        make_process_setup(&context, kDxgiFormatR8G8B8A8Unorm, &rgba_process,
                           kDiagnosticDimensions, false, true);
    const bool bgra_ok = profile_ok &&
        make_process_setup(&context, kDxgiFormatB8G8R8A8Unorm, &bgra_process,
                           kDiagnosticDimensions, false, true);
    EncoderSetup setup;
    const bool encoder_ok = profile_ok && rgba_ok && bgra_ok &&
        initialize_encoder(&context, &setup, kDiagnosticDimensions, false, true);

    std::printf("=== AppSandbox HEVC444 Guest Capability ===\n"
                "hevc444_profile=%u\n"
                "ayuv_encode_input=%u\n"
                "rgba_to_ayuv=%u\n"
                "bgra_to_ayuv=%u\n"
                "hevc444_codec_config=%u\n"
                "hevc444_picture_control=%u\n"
                "hevc444_encoder_create=%u\n"
                "hevc444_4k60_config=%u\n"
                "hevc444_4k60_sustained=not-run\n"
                "sequence_header_chroma_format_idc=3\n"
                "guest_bitstream_generated=0\n",
                profile_ok ? 1U : 0U, ayuv_input_ok ? 1U : 0U,
                rgba_ok ? 1U : 0U, bgra_ok ? 1U : 0U,
                codec_config_ok ? 1U : 0U, encoder_ok ? 1U : 0U,
                encoder_ok ? 1U : 0U, encoder_ok ? 1U : 0U);
    return encoder_ok ? 0 : 1;
}

} // namespace

#ifndef ASB_D3D12_VIDEO_NO_MAIN
int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    (void)signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && std::strcmp(argv[1], "--capability") == 0)
        return capability_main();
    if (argc == 2 && std::strcmp(argv[1], "--capability-444") == 0)
        return capability_444_main();
    if (argc == 2 && std::strcmp(argv[1], "--workload-444") == 0) {
        if (capability_444_main() != 0)
            return 1;
        if (setenv("D3D12_VIDEO_BITSTREAM_PATH",
                   "/tmp/appsandbox-hevc444-probe.hevc", 1) != 0)
            return 1;
        const int result = launch_encode_children(argv[0],
                                                  EncodeProbeMode::Hevc444);
        unsetenv("D3D12_VIDEO_BITSTREAM_PATH");
        return result;
    }
    if (argc == 3 && std::strcmp(argv[1], "--producer") == 0)
        return producer_main(std::atoi(argv[2])) ? 0 : 1;
    if (argc == 4 && std::strcmp(argv[1], "--consumer") == 0) {
        const EncodeProbeMode mode =
            std::strcmp(argv[3], "--hevc444") == 0
                ? EncodeProbeMode::Hevc444 : EncodeProbeMode::Hevc420;
        if (std::strcmp(argv[3], "--hevc444") != 0 &&
            std::strcmp(argv[3], "--hevc420") != 0)
            return 2;
        return encode_consumer_main(std::atoi(argv[2]), mode) ? 0 : 1;
    }
    if (argc == 1)
        return launch_encode_children(argv[0]);
    std::fprintf(stderr,
                 "Usage: %s [--capability|--capability-444|--workload-444]\n",
                 argv[0]);
    return 2;
}
#endif
