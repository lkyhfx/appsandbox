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
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cpp = root / "headers.cpp"
            cpp.write_text(harness)
            executable = root / "headers-test"
            subprocess.run(["c++", "-std=c++17", str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
