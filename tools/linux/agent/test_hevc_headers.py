"""CPU-only regression checks for the encoder's HEVC parameter sets."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class HevcHeadersTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "requires a C++ compiler")
    def test_parameter_sets_preserve_template_and_rewrite_dimensions(self):
        source = Path(__file__).with_name("d3d12-video-encode-probe.cpp").read_text()
        begin = source.index("static constexpr std::uint8_t kHevcSequenceHeaders[]")
        end = source.index("static D3D12_VIDEO_ENCODER_PROFILE_DESC", begin)
        harness = r'''
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <cassert>
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
    const auto headers444 = dynamic_hevc_sequence_headers(3840, 2160, true);
    assert(!headers444.empty());
    std::size_t sps444 = 0;
    while (sps444 + 6 < headers444.size() &&
           ((headers444[sps444 + 4] >> 1) & 63) != 33) {
        sps444 += 4;
        while (sps444 + 4 <= headers444.size() &&
               std::memcmp(headers444.data() + sps444, "\0\0\0\1", 4) != 0)
            ++sps444;
    }
    assert(sps444 + 6 < headers444.size());
    auto rbsp444 = hevc_unescape(headers444.data() + sps444 + 6,
                                 headers444.size() - sps444 - 6);
    HevcBitReader r444{rbsp444};
    std::uint64_t v444, layers444, chroma444, width444, height444;
    assert(r444.read(4, &v444) && r444.read(3, &layers444) &&
           r444.read(1, &v444));
    assert(hevc_skip_profile_tier_level(
        &r444, static_cast<unsigned>(layers444)));
    assert(r444.ue(&v444) && r444.ue(&chroma444) && chroma444 == 3);
    assert(r444.read(1, &v444) && r444.ue(&width444) &&
           r444.ue(&height444));
    assert(width444 == 3840 && height444 == 2160);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cpp = root / "headers.cpp"
            cpp.write_text(harness)
            executable = root / "headers-test"
            subprocess.run(["c++", "-std=c++17", str(cpp), "-o", str(executable)], check=True)
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
        self.assertIn("hevc444_4k60_sustained=not-run", capability)
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
        ):
            self.assertIn(token, source)
        self.assertIn("d3d12_actual_decode=not-tested", source)
        self.assertIn("const bool d3d12_path = false", source)


if __name__ == "__main__":
    unittest.main()
