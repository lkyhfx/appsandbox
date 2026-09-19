"""CPU-only regression checks for the encoder's HEVC parameter sets."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class HevcHeadersTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "requires a C++ compiler")
    def test_420_parameter_sets_preserve_template_and_rewrite_dimensions(self):
        source = Path(__file__).with_name("d3d12-video-encode-probe.cpp").read_text()
        begin = source.index("static constexpr std::uint8_t kHevcSequenceHeaders[]")
        end = source.index("static D3D12_VIDEO_ENCODER_PROFILE_DESC", begin)
        harness = r'''
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <cassert>
#include <cstdio>
'''
        harness += source[begin:end]
        harness += r'''
int main() {
    const std::vector<std::uint8_t> original(
        kHevcSequenceHeaders, kHevcSequenceHeaders + sizeof(kHevcSequenceHeaders));
    assert(dynamic_hevc_sequence_headers(3840, 2160) == original);
    for (const auto dimensions : {std::vector<unsigned>{1920,1080},
                                 std::vector<unsigned>{2560,1440}}) {
        const auto headers = dynamic_hevc_sequence_headers(dimensions[0], dimensions[1]);
        assert(!headers.empty());
        std::size_t start = 0;
        while (start + 6 < headers.size() && ((headers[start + 4] >> 1) & 63) != 33) {
            start += 4;
            while (start + 4 <= headers.size() &&
                   std::memcmp(headers.data() + start, "\0\0\0\1", 4) != 0) ++start;
        }
        assert(start + 6 < headers.size());
        auto rbsp = hevc_unescape(headers.data() + start + 6, headers.size() - start - 6);
        HevcBitReader r{rbsp};
        std::uint64_t v, layers, width, height;
        assert(r.read(4, &v) && r.read(3, &layers) && r.read(1, &v));
        assert(hevc_skip_profile_tier_level(&r, static_cast<unsigned>(layers)));
        assert(r.ue(&v) && r.ue(&v) && v == 1);
        assert(r.ue(&width) && r.ue(&height));
        assert(width == dimensions[0] && height == dimensions[1]);
        assert(headers.back() == original.back());
    }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cpp = root / "headers.cpp"
            cpp.write_text(harness)
            executable = root / "headers-test"
            subprocess.run(["c++", "-std=c++17", str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_dynamic_444_sps_pps_round_trip(self):
        helper = Path(__file__).with_name("hevc444_probe_codec.h")
        harness = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include "hevc444_probe_codec.h"

int main() {
    using namespace appsandbox_hevc444_probe;
    SequenceConfig config;
    config.width = 3840;
    config.height = 2160;
    config.configuration_flags =
        kConfigUseAsymmetricMotionPartition |
        kConfigEnableTransformSkipping | kConfigTransformSkipRotation |
        kConfigTransformSkipContext | kConfigImplicitRdpcm |
        kConfigExplicitRdpcm | kConfigExtendedPrecisionProcessing |
        kConfigIntraSmoothingDisabled | kConfigHighPrecisionOffsets |
        kConfigPersistentRiceAdaptation | kConfigCabacBypassAlignment |
        kConfigTemporalMvpEnabled | kConfigStrongIntraSmoothingEnabled |
        kConfigEnableSaoFilter;
    config.picture.flags = kPictureCrossComponentPrediction |
                           kPictureChromaQpOffsetList;
    config.picture.diff_cu_chroma_qp_offset_depth = 1;
    config.picture.log2_sao_offset_scale_luma = 2;
    config.picture.log2_sao_offset_scale_chroma = 3;
    config.picture.log2_max_transform_skip_block_size_minus2 = 1;
    config.picture.chroma_qp_offset_list_len_minus1 = 1;
    config.picture.cb_qp_offset_list[0] = -2;
    config.picture.cb_qp_offset_list[1] = 3;
    config.picture.cr_qp_offset_list[0] = 1;
    config.picture.cr_qp_offset_list[1] = -4;
    std::vector<std::uint8_t> headers;
    if (!build_hevc444_sequence_headers(config, &headers)) return 1;
    ParsedConfig parsed;
    if (!parse_hevc444_sequence_headers(headers, &parsed)) return 2;
    assert(parsed.profile_idc == 4);
    assert(parsed.chroma_format_idc == 3);
    assert(parsed.width == config.width && parsed.height == config.height);
    assert(parsed.bit_depth_luma_minus8 == 0 &&
           parsed.bit_depth_chroma_minus8 == 0);
    assert(parsed.min_luma_coding_unit_size == config.min_luma_coding_unit_size &&
           parsed.max_luma_coding_unit_size == config.max_luma_coding_unit_size &&
           parsed.min_luma_transform_unit_size == config.min_luma_transform_unit_size &&
           parsed.max_luma_transform_unit_size == config.max_luma_transform_unit_size);
    assert((parsed.configuration_flags & kConfigTransformSkipRotation) != 0);
    assert((parsed.configuration_flags & kConfigTemporalMvpEnabled) != 0);
    assert((parsed.picture_flags & kPictureCrossComponentPrediction) != 0);
    assert((parsed.picture_flags & kPictureChromaQpOffsetList) != 0);
    assert(parsed.diff_cu_chroma_qp_offset_depth == 1);
    assert(parsed.chroma_qp_offset_list_len_minus1 == 1);
    assert(parsed.cb_qp_offset_list[0] == -2 &&
           parsed.cb_qp_offset_list[1] == 3);
    assert(parsed.cr_qp_offset_list[0] == 1 &&
           parsed.cr_qp_offset_list[1] == -4);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "codec.cpp"
            cpp.write_text(harness)
            executable = Path(directory) / "codec-test"
            subprocess.run(["c++", "-std=c++17", "-I", str(helper.parent),
                            str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_required_flag_mapper_sets_configuration_and_picture_flags(self):
        helper = Path(__file__).with_name("hevc444_probe_codec.h")
        harness = r'''
#include <cassert>
#include <cstdint>
#include "hevc444_probe_codec.h"
int main() {
    using namespace appsandbox_hevc444_probe;
    std::uint32_t configuration = 0, picture = 0;
    const std::uint32_t support =
        kRequiredImplicitRdpcm | kRequiredCrossComponentPrediction |
        kRequiredChromaQpOffsetList;
    const std::uint32_t support1 =
        kRequiredTemporalMvp | kRequiredStrongIntraSmoothing;
    assert(apply_required_configuration_flags(support, support1,
                                              &configuration, &picture));
    assert((configuration & kConfigImplicitRdpcm) != 0);
    assert((configuration & kConfigTemporalMvpEnabled) != 0);
    assert((configuration & kConfigStrongIntraSmoothingEnabled) != 0);
    assert((picture & kPictureCrossComponentPrediction) != 0);
    assert((picture & kPictureChromaQpOffsetList) != 0);
    assert(!apply_required_configuration_flags(0, 1u << 6,
                                               &configuration, &picture));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "mapper.cpp"
            cpp.write_text(harness)
            executable = Path(directory) / "mapper-test"
            subprocess.run(["c++", "-std=c++17", "-I", str(helper.parent),
                            str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_picture_control_keeps_420_legacy_and_444_hevc1(self):
        source = Path(__file__).with_name("d3d12-video-encode-probe.cpp").read_text()
        self.assertIn("D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1", source)
        self.assertIn("picture.PictureControlCodecData.pHEVCPicData1", source)
        self.assertIn("picture.PictureControlCodecData.pHEVCPicData = &picture_data", source)
        branch = source[source.index("if (hevc444_session)") : source.index(
            "input.SequenceControlDesc", source.index("if (hevc444_session)")
        )]
        self.assertIn("pHEVCPicData1", branch)
        self.assertIn("else", branch)
        self.assertIn("pHEVCPicData =", branch)

    def test_hevc1_defaults_are_selected_from_driver_masks(self):
        source = Path(__file__).with_name("d3d12-video-encode-probe.cpp").read_text()
        self.assertIn("choose_hevc444_picture_defaults", source)
        for field in (
            "allowed_diff_cu_chroma_qp_offset_depth_values",
            "allowed_log2_sao_offset_scale_luma_values",
            "allowed_log2_sao_offset_scale_chroma_values",
            "allowed_log2_max_transform_skip_block_size_minus2_values",
            "allowed_chroma_qp_offset_list_len_minus1_values",
            "allowed_cb_qp_offset_list_values",
            "allowed_cr_qp_offset_list_values",
        ):
            self.assertIn(field, source)
        self.assertIn("reason=no-valid-configuration", source)

    def test_first_irap_access_unit_parser_keeps_all_slices(self):
        helper = Path(__file__).parents[2] / "win" / "hevc444-probe" / \
            "hevc_access_unit_probe.h"
        harness = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include "hevc_access_unit_probe.h"

static void nal(std::vector<std::uint8_t>* stream, unsigned type,
                std::uint8_t first, std::uint8_t marker) {
    stream->insert(stream->end(), {0, 0, 0, 1,
        static_cast<std::uint8_t>(type << 1), 1, first, marker});
}

int main() {
    std::vector<std::uint8_t> stream;
    nal(&stream, 32, 0, 1);
    nal(&stream, 33, 0, 2);
    nal(&stream, 34, 0, 3);
    nal(&stream, 35, 0, 4);       // AUD
    nal(&stream, 39, 0, 5);       // prefix SEI
    nal(&stream, 19, 0x80, 10);   // IDR slice #0, first_slice=1
    nal(&stream, 19, 0x00, 11);   // IDR slice #1, first_slice=0
    nal(&stream, 1, 0x80, 12);    // next picture, first_slice=1
    HevcAccessUnit au;
    assert(hevc_access_unit_probe::extract_first_irap_access_unit(stream, &au));
    assert(au.irap && au.idr && au.nal_count == 4);
    assert(au.bytes.size() < stream.size());
    assert(au.bytes.back() == 11);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "access-unit.cpp"
            cpp.write_text(harness)
            executable = Path(directory) / "access-unit-test"
            subprocess.run(["c++", "-std=c++17", "-I", str(helper.parent),
                            str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_sequence_header_is_not_bitstream_decode_evidence(self):
        source = Path(__file__).with_name("d3d12-video-encode-probe.cpp").read_text()
        self.assertIn("stage=hevc444-sequence-header", source)
        self.assertNotIn("stage=hevc444-bitstream", source)
        self.assertIn("guest_sequence_header_444", source)
        self.assertIn("guest_bitstream_generated", source)
        self.assertIn("guest_encode_failures", source)

    def test_probe_mode_cannot_switch_production_to_444(self):
        source = Path(__file__).with_name("d3d12-video-encode-probe.cpp").read_text()
        production = Path(__file__).with_name("display_d3d12_encoder.cpp").read_text()
        self.assertNotIn("ASB_D3D12_HEVC444", source)
        self.assertIn("encode_consumer_main(publisher, EncodeProbeMode::Hevc420)", production)
        self.assertIn("!production_session && mode == EncodeProbeMode::Hevc444", source)

    def test_capability_does_not_claim_sustained_4k60(self):
        source = Path(__file__).with_name("d3d12-video-encode-probe.cpp").read_text()
        capability = source[source.index("static int capability_444_main") :]
        self.assertIn("hevc444_4k60_config", capability)
        self.assertIn("hevc444_required_flags_applied", capability)
        self.assertIn("hevc444_4k60_sustained=not-run", capability)
        self.assertIn("sequence_header_runtime_config_match=not-run", capability)
        self.assertNotIn("hevc444_4k60=", capability)

    def test_host_requires_real_mf_decode_and_gpu_ayuv_surface(self):
        host = Path(__file__).parents[2] / "win" / "hevc444-probe" / "hevc444-probe.cpp"
        source = host.read_text()
        for token in (
            "ProcessInput",
            "ProcessOutput",
            "IMFDXGIBuffer",
            "DXGI_FORMAT_AYUV",
            "VideoProcessorBlt",
            "HEVC444_PATH=",
            "extract_first_irap_access_unit",
            "MFSampleExtension_CleanPoint",
            "guest_first_irap_nal_count",
        ):
            self.assertIn(token, source)
        self.assertIn("stream.first_irap.bytes", source)
        self.assertNotIn("std::memcpy(destination, stream.bytes.data()", source)
        self.assertIn("d3d12_actual_decode=not-tested", source)
        self.assertIn("const bool d3d12_path = false", source)


if __name__ == "__main__":
    unittest.main()
