#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

struct HevcAccessUnit {
    std::vector<std::uint8_t> bytes;
    bool irap = false;
    bool idr = false;
    std::size_t nal_count = 0;
};

namespace hevc_access_unit_probe {

struct Nal {
    std::vector<std::uint8_t> bytes;
    unsigned type = 0;
};

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

inline bool split(const std::vector<std::uint8_t> &bytes,
                  std::vector<Nal> *out)
{
    if (!out)
        return false;
    out->clear();
    std::size_t start = 0;
    while (true) {
        std::size_t prefix_start = bytes.size();
        std::size_t prefix_size = 0;
        for (std::size_t i = start; i + 3 <= bytes.size(); ++i) {
            if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
                prefix_start = i;
                prefix_size = 3;
                break;
            }
            if (i + 4 <= bytes.size() && bytes[i] == 0 && bytes[i + 1] == 0 &&
                bytes[i + 2] == 0 && bytes[i + 3] == 1) {
                prefix_start = i;
                prefix_size = 4;
                break;
            }
        }
        if (prefix_start == bytes.size())
            break;
        const std::size_t nal_begin = prefix_start + prefix_size;
        if (nal_begin + 2 > bytes.size())
            return false;
        std::size_t next = nal_begin;
        std::size_t next_prefix = bytes.size();
        for (; next + 3 <= bytes.size(); ++next) {
            if (bytes[next] == 0 && bytes[next + 1] == 0 &&
                (bytes[next + 2] == 1 || (next + 3 < bytes.size() &&
                 bytes[next + 2] == 0 && bytes[next + 3] == 1))) {
                next_prefix = next;
                break;
            }
        }
        const std::size_t nal_end = next_prefix == bytes.size()
            ? bytes.size() : next_prefix;
        if (nal_end <= nal_begin)
            return false;
        Nal nal;
        nal.bytes.assign(bytes.begin() + prefix_start, bytes.begin() + nal_end);
        nal.type = (bytes[nal_begin] >> 1) & 0x3f;
        out->push_back(std::move(nal));
        if (next_prefix == bytes.size())
            break;
        start = next_prefix;
    }
    return !out->empty();
}

inline bool is_vcl(unsigned type) { return type <= 31; }
inline bool is_irap(unsigned type) { return type >= 16 && type <= 23; }
inline bool is_idr(unsigned type) { return type == 19 || type == 20; }

inline bool first_slice_segment_in_pic(const Nal &nal, bool *first)
{
    if (!first || !is_vcl(nal.type) || nal.bytes.size() < 3)
        return false;
    const std::size_t prefix = nal.bytes.size() >= 4 && nal.bytes[2] == 1 ? 3 : 4;
    if (nal.bytes.size() < prefix + 2)
        return false;
    const auto rbsp = unescape(nal.bytes.data() + prefix + 2,
                               nal.bytes.size() - prefix - 2);
    if (rbsp.empty())
        return false;
    *first = (rbsp[0] & 0x80) != 0;
    return true;
}

inline bool extract_first_irap_access_unit(
    const std::vector<std::uint8_t> &annex_b, HevcAccessUnit *out)
{
    if (!out)
        return false;
    *out = {};
    std::vector<Nal> nals;
    if (!split(annex_b, &nals))
        return false;

    std::vector<std::uint8_t> prefix;
    bool collecting = false;
    for (const Nal &nal : nals) {
        if (nal.type == 32 || nal.type == 33 || nal.type == 34)
            continue;
        if (!collecting) {
            if (nal.type == 35 || nal.type == 39) {
                prefix.insert(prefix.end(), nal.bytes.begin(), nal.bytes.end());
                continue;
            }
            if (!is_vcl(nal.type))
                continue;
            bool first = false;
            if (!first_slice_segment_in_pic(nal, &first))
                return false;
            if (!first)
                continue;
            if (!is_irap(nal.type)) {
                prefix.clear();
                continue;
            }
            out->bytes = prefix;
            out->bytes.insert(out->bytes.end(), nal.bytes.begin(), nal.bytes.end());
            out->irap = true;
            out->idr = is_idr(nal.type);
            out->nal_count = 1;
            std::vector<Nal> prefix_nals;
            if (!prefix.empty() && !split(prefix, &prefix_nals))
                return false;
            out->nal_count += prefix_nals.size();
            collecting = true;
            continue;
        }

        if (is_vcl(nal.type)) {
            bool first = false;
            if (!first_slice_segment_in_pic(nal, &first))
                return false;
            if (first)
                break;
            out->bytes.insert(out->bytes.end(), nal.bytes.begin(), nal.bytes.end());
            ++out->nal_count;
        } else if (nal.type == 40) {
            out->bytes.insert(out->bytes.end(), nal.bytes.begin(), nal.bytes.end());
            ++out->nal_count;
        }
    }
    return collecting && !out->bytes.empty() && out->nal_count != 0;
}

} // namespace hevc_access_unit_probe
