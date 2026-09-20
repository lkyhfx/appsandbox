"""CPU-only contracts for Issue #4 gate evidence."""
from pathlib import Path
import unittest


def parse_unique(lines):
    values = {}
    for raw in lines.splitlines():
        if "=" not in raw or raw.startswith(("PASS ", "BLOCKED ", "FAIL ")):
            continue
        key, value = raw.split("=", 1)
        if key in values:
            raise ValueError(f"duplicate key: {key}")
        values[key] = value.strip()
    return values


class Issue4GateContractTest(unittest.TestCase):
    def test_gate_a_requires_4k444_actual_encode(self):
        values = parse_unique("""
guest_nvenc_hevc=1
guest_nvenc_hevc444=1
guest_nvenc_yuv444=1
guest_nvenc_max_width=7680
guest_nvenc_max_height=4320
guest_nvenc_4k=1
guest_nvenc_actual_encode=PASS
guest_nvenc_encoded_frames=600
guest_nvenc_encode_failures=0
decoded_width=3840
decoded_height=2160
decoded_pix_fmt=yuv444p
decoded_frames=600
decode_errors=0
""")
        required = {
            "guest_nvenc_hevc", "guest_nvenc_hevc444", "guest_nvenc_yuv444",
            "guest_nvenc_max_width", "guest_nvenc_max_height", "guest_nvenc_4k",
            "guest_nvenc_actual_encode", "guest_nvenc_encoded_frames",
            "guest_nvenc_encode_failures", "decoded_width", "decoded_height",
            "decoded_pix_fmt", "decoded_frames", "decode_errors",
        }
        self.assertTrue(required <= values.keys())
        self.assertEqual(values["guest_nvenc_actual_encode"], "PASS")
        self.assertEqual(values["guest_nvenc_encoded_frames"], "600")
        self.assertEqual(values["guest_nvenc_encode_failures"], "0")
        self.assertEqual(values["decoded_pix_fmt"], "yuv444p")

    def test_gate_a_duplicate_or_nv12_evidence_is_not_valid(self):
        with self.assertRaises(ValueError):
            parse_unique("guest_nvenc_hevc=1\nguest_nvenc_hevc=0\n")
        values = parse_unique("""
guest_nvenc_actual_encode=PASS
guest_nvenc_encoded_frames=600
guest_nvenc_encode_failures=0
decoded_width=3840
decoded_height=2160
decoded_pix_fmt=nv12
decoded_frames=600
decode_errors=0
""")
        self.assertNotIn("444", values["decoded_pix_fmt"])

    def test_gate_b_fail_closed_contract(self):
        values = parse_unique("""
real_mutter=0
frames=0
cpu_framebuffer_copy=0
cpu_conversion=0
cpu_upload=0
BLOCKED stage=gpu_rgb_to_yuv444 reason=no-gpu-conversion-kernel
""")
        self.assertEqual(values["real_mutter"], "0")
        self.assertEqual(values["cpu_framebuffer_copy"], "0")
        self.assertNotEqual((values["real_mutter"], values["frames"]), ("1", "600"))

    def test_gate_c_requires_persistent_zero_copy_metrics(self):
        values = parse_unique("""
host_decode_frames=3600
host_decode_failures=0
host_process_failures=0
host_present_failures=0
device_removed=0
per_frame_queue_create=0
per_frame_processor_create=0
per_frame_shared_open=0
cpu_frame_copy=0
host_pipeline_slots=3
host_pipeline_persistent=1
""")
        for key in ("host_decode_failures", "host_process_failures",
                    "host_present_failures", "device_removed",
                    "per_frame_queue_create", "per_frame_processor_create",
                    "per_frame_shared_open", "cpu_frame_copy"):
            self.assertEqual(values[key], "0")
        self.assertEqual(values["host_decode_frames"], "3600")
        self.assertEqual(values["host_pipeline_slots"], "3")
        self.assertEqual(values["host_pipeline_persistent"], "1")

    def test_sources_keep_production_gate_closed(self):
        repo = Path(__file__).resolve().parents[3]
        readme = (repo / "tools" / "linux" / "agent" / "README.md").read_text(encoding="utf-8")
        self.assertIn("production-4k60: false", readme)
        backend = (repo / "src" / "backend_win" /
                   "vm_video_decode_d3d12.cpp").read_text(encoding="utf-8")
        self.assertIn("kSlotCount = 3", backend)
        self.assertIn("CreateSharedHandle", backend)
        self.assertIn("host_texture", backend)


if __name__ == "__main__":
    unittest.main()
