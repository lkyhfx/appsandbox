"""Executable regression checks for the host decode backend policy."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


class BackendPolicyTest(unittest.TestCase):
    def test_policy_contract(self):
        repo = Path(__file__).resolve().parents[3]
        header = repo / "src" / "backend_win" / "vm_video_decode_policy.h"
        self.assertTrue(header.is_file())

        harness = r'''
#include <cassert>
#include "vm_video_decode_policy.h"

int main() {
    // A failed decode, an unchanged output, and a CPU copy are all blocked.
    assert(!vm_video_decode_evidence_ready(0, 1, 1, 1, 0));
    assert(!vm_video_decode_evidence_ready(1, 0, 1, 1, 0));
    assert(!vm_video_decode_evidence_ready(1, 1, 0, 1, 0));
    assert(!vm_video_decode_evidence_ready(1, 1, 1, 0, 0));
    assert(!vm_video_decode_evidence_ready(1, 1, 1, 1, 1));

    // Only the complete GPU decode -> written output -> RGB chain is ready.
    assert(vm_video_decode_evidence_ready(1, 1, 1, 1, 0));

    // MF/D3D11 is preferred when both implementations are proven.
    assert(vm_video_decode_select_backend(1, 1) == VM_VIDEO_DECODE_BACKEND_MF_D3D11);
    // D3D12 is the fallback when MF is unavailable but D3D12 is proven.
    assert(vm_video_decode_select_backend(0, 1) == VM_VIDEO_DECODE_BACKEND_D3D12);
    // Capability-only results must not select a backend.
    assert(vm_video_decode_select_backend(0, 0) == VM_VIDEO_DECODE_BACKEND_NONE);
}
'''

        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "backend_policy_test.cpp"
            executable = work / ("backend_policy_test.exe" if os.name == "nt" else "backend_policy_test")
            source.write_text(harness, encoding="utf-8")

            compiler = shutil.which("cl")
            if compiler:
                command = [compiler, "/nologo", "/EHsc", "/std:c++17",
                           f"/I{header.parent}", str(source), f"/Fe:{executable}"]
            else:
                compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
                if not compiler:
                    self.skipTest("requires cl, c++, g++, or clang++")
                command = [compiler, "-std=c++17", "-I", str(header.parent),
                           str(source), "-o", str(executable)]

            subprocess.run(command, cwd=work, check=True)
            subprocess.run([str(executable)], cwd=work, check=True)

    def test_hevc444_decode_config_contract(self):
        repo = Path(__file__).resolve().parents[3]
        header = repo / "src" / "backend_win" / "vm_hevc444_decode_config.h"
        self.assertTrue(header.is_file())
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "vm_hevc444_decode_config.h"

using namespace appsandbox_hevc444_probe;
using namespace asb_hevc_decode;
using namespace hevc_access_unit_probe;

static std::vector<std::uint8_t> headers_from_sample() {
    return std::vector<std::uint8_t>(
        asb_hevc444_probe_sample,
        asb_hevc444_probe_sample + ASB_HEVC444_PROBE_EXTRADATA_SIZE);
}

static std::vector<std::uint8_t> au_from_sample() {
    return std::vector<std::uint8_t>(
        asb_hevc444_probe_sample + ASB_HEVC444_PROBE_ACCESS_UNIT_OFFSET,
        asb_hevc444_probe_sample + ASB_HEVC444_PROBE_SAMPLE_SIZE);
}

static void set_bit(std::vector<std::uint8_t> *bytes, std::size_t bit,
                    std::uint8_t value) {
    (*bytes)[bit / 8] = static_cast<std::uint8_t>(
        ((*bytes)[bit / 8] & ~(1u << (7 - (bit & 7)))) |
        ((value & 1u) << (7 - (bit & 7))));
}

int main() {
    auto headers = headers_from_sample();
    ParsedConfig parsed = {};
    bool bundled = false;
    assert(configuration(headers.data(), static_cast<unsigned>(headers.size()),
                         &parsed, &bundled));
    assert(bundled);

    // The guest canonical stream is 3840x2160, Main 4:4:4, 8-bit, level 5.1.
    // Build its default value model explicitly; the bundled sample is accepted
    // as an opaque known-good fixture and is intentionally not reparsed here.
    SequenceConfig canonical = {};
    canonical.width = 3840;
    canonical.height = 2160;
    canonical.configuration_flags =
        kConfigUseAsymmetricMotionPartition |
        kConfigEnableTransformSkipping | kConfigTransformSkipRotation |
        kConfigTransformSkipContext | kConfigImplicitRdpcm |
        kConfigExplicitRdpcm | kConfigExtendedPrecisionProcessing |
        kConfigIntraSmoothingDisabled | kConfigHighPrecisionOffsets |
        kConfigPersistentRiceAdaptation | kConfigCabacBypassAlignment |
        kConfigTemporalMvpEnabled | kConfigStrongIntraSmoothingEnabled |
        kConfigEnableSaoFilter;
    canonical.picture.flags = kPictureCrossComponentPrediction |
                              kPictureChromaQpOffsetList;
    canonical.picture.diff_cu_chroma_qp_offset_depth = 1;
    canonical.picture.log2_sao_offset_scale_luma = 2;
    canonical.picture.log2_sao_offset_scale_chroma = 3;
    canonical.picture.log2_max_transform_skip_block_size_minus2 = 1;
    canonical.picture.chroma_qp_offset_list_len_minus1 = 1;
    canonical.picture.cb_qp_offset_list[0] = -2;
    canonical.picture.cb_qp_offset_list[1] = 3;
    canonical.picture.cr_qp_offset_list[0] = 1;
    canonical.picture.cr_qp_offset_list[1] = -4;
    std::vector<std::uint8_t> rebuilt;
    assert(build_hevc444_sequence_headers(canonical, &rebuilt));
    assert(parse_hevc444_sequence_headers(rebuilt, &parsed));
    assert(parsed.width == 3840 && parsed.height == 2160);
    assert(parsed.profile_idc == 4 && parsed.level_idc == 153);
    assert(parsed.chroma_format_idc == 3);
    assert(parsed.bit_depth_luma_minus8 == 0 &&
           parsed.bit_depth_chroma_minus8 == 0);
    assert((parsed.configuration_flags & kConfigEnableSaoFilter) != 0);
    assert((parsed.configuration_flags & kConfigTemporalMvpEnabled) != 0);
    assert((parsed.configuration_flags & kConfigStrongIntraSmoothingEnabled) != 0);
    assert((parsed.picture_flags & kPictureChromaQpOffsetList) != 0);
    // A canonical re-encode is accepted; truncation and byte changes fail.
    assert(configuration(rebuilt.data(), static_cast<unsigned>(rebuilt.size()),
                         &parsed, &bundled));
    assert(!bundled);
    auto truncated = rebuilt;
    truncated.pop_back();
    assert(!configuration(truncated.data(), static_cast<unsigned>(truncated.size()),
                          &parsed, &bundled));
    auto changed = rebuilt;
    changed[changed.size() - 1] ^= 1;
    assert(!configuration(changed.data(), static_cast<unsigned>(changed.size()),
                          &parsed, &bundled));

    // The default value model (only dimensions set) is also canonical.
    SequenceConfig defaults = {};
    defaults.width = 3840;
    defaults.height = 2160;
    std::vector<std::uint8_t> default_headers;
    assert(build_hevc444_sequence_headers(defaults, &default_headers));
    assert(configuration(default_headers.data(),
                         static_cast<unsigned>(default_headers.size()),
                         &parsed, &bundled));
    assert(!bundled);

    auto au = au_from_sample();
    std::vector<std::uint8_t> compressed;
    assert(idr_slice(au.data(), static_cast<unsigned>(au.size()), &compressed));

    std::vector<Nal> nals;
    assert(split(au, &nals));
    std::vector<std::uint8_t> vcl;
    for (const Nal &nal : nals) {
        if (is_vcl(nal.type)) {
            vcl = nal.bytes;
            break;
        }
    }
    assert(!vcl.empty());
    const std::size_t prefix = vcl[2] == 1 ? 3 : 4;

    // Wrong VCL type, a second VCL picture, invalid PPS, and temporal id 0 fail.
    auto non_idr = vcl;
    non_idr[prefix] = static_cast<std::uint8_t>((1u << 1) | (non_idr[prefix] & 1u));
    assert(!idr_slice(non_idr.data(), static_cast<unsigned>(non_idr.size()), &compressed));
    auto second_slice = vcl;
    second_slice.insert(second_slice.end(), vcl.begin(), vcl.end());
    assert(!idr_slice(second_slice.data(), static_cast<unsigned>(second_slice.size()), &compressed));
    auto wrong_pps = vcl;
    // First VCL RBSP: first_slice_segment_in_pic_flag, no_output_of_prior_pics,
    // then PPS ue(v). Change ue(0)=1 to ue(1)=010 while retaining first bits.
    set_bit(&wrong_pps, 8 * (prefix + 2) + 2, 0);
    set_bit(&wrong_pps, 8 * (prefix + 2) + 3, 1);
    set_bit(&wrong_pps, 8 * (prefix + 2) + 4, 0);
    assert(!idr_slice(wrong_pps.data(), static_cast<unsigned>(wrong_pps.size()), &compressed));
    auto bad_temporal_id = vcl;
    bad_temporal_id[prefix + 1] &= 0xf8;
    assert(!idr_slice(bad_temporal_id.data(), static_cast<unsigned>(bad_temporal_id.size()), &compressed));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "hevc444_decode_config_test.cpp"
            executable = work / ("hevc444_decode_config_test.exe" if os.name == "nt" else "hevc444_decode_config_test")
            source.write_text(harness, encoding="utf-8")
            compiler = shutil.which("cl")
            if compiler:
                command = [compiler, "/nologo", "/EHsc", "/std:c++17",
                           f"/I{repo / 'src' / 'backend_win'}", str(source), f"/Fe:{executable}"]
            else:
                compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
                if not compiler:
                    self.skipTest("requires cl, c++, g++, or clang++")
                command = [compiler, "-std=c++17", "-I", str(repo / "src" / "backend_win"),
                           str(source), "-o", str(executable)]
            subprocess.run(command, cwd=work, check=True)
            subprocess.run([str(executable)], cwd=work, check=True)


if __name__ == "__main__":
    unittest.main()
