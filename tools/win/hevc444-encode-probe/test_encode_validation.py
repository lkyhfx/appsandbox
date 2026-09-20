"""CPU-only behavioral tests for the Windows encode probe gates."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class EncodeValidationTest(unittest.TestCase):
  def test_payload_sequence_and_state_gates(self):
    compiler = shutil.which("c++")
    if not compiler:
        self.skipTest("requires a C++ compiler")
    include_dir = Path(__file__).parent
    harness = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include "hevc444_encode_validation.h"

static hevc_access_unit_probe::Nal nal(unsigned type, std::uint8_t rbsp = 0xec) {
    hevc_access_unit_probe::Nal n;
    n.type = type;
    n.bytes = {0, 0, 0, 1, static_cast<std::uint8_t>(type << 1), 1, rbsp};
    return n;
}

int main() {
    using namespace appsandbox_hevc444_encode;
    std::vector<hevc_access_unit_probe::Nal> idr{nal(19)};
    auto payload = validate_encoder_payload(true, idr);
    assert(payload.valid() && payload.have_vcl && payload.have_idr);
    payload = validate_encoder_payload(true, {nal(21)});  // CRA, not IDR.
    assert(payload.have_vcl && !payload.have_idr && !payload.valid());
    assert(!validate_encoder_payload(true, {nal(19, 0x98)}).valid()); // PPS 1.
    assert(!validate_encoder_payload(true, {nal(19, 0x80)}).valid()); // truncated.
    assert(!validate_encoder_payload(true, {nal(19), nal(19, 0x80)}).valid());
    assert(!validate_encoder_payload(true, {nal(19, 0x98), nal(19)}).valid());
    assert(!validate_encoder_payload(true, {nal(19), nal(1)}).valid());
    assert(!validate_encoder_payload(false, idr).valid());

    assert(validate_main444_sequence_header(true, 4, 153, 3, 0, 0,
                                            3840, 2160, 3840, 2160,
                                            7, 7, 9, 9));
    assert(!validate_main444_sequence_header(true, 1, 153, 3, 0, 0,
                                             3840, 2160, 3840, 2160,
                                             7, 7, 9, 9));
    assert(!validate_main444_sequence_header(true, 4, 150, 3, 0, 0,
                                             3840, 2160, 3840, 2160,
                                             7, 7, 9, 9));
    assert(supports_hevc_level_51(8, 8));
    assert(!supports_hevc_level_51(7, 8));
    assert(std::string(combined_state(true, false, false)) == "BLOCKED_BY_LEVEL");
    assert(std::string(combined_state(false, true, true)) == "BLOCKED_BY_PROFILE");
    assert(std::string(combined_state(true, true, false)) == "UNSUPPORTED");
}
'''
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        source = root / "validation.cpp"
        executable = root / "validation-test"
        source.write_text(harness)
        subprocess.run([compiler, "-std=c++17", "-I", str(include_dir),
                        str(source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
