import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


class DisplayProfileContractTests(unittest.TestCase):
    def read(self, *parts):
        return (ROOT.joinpath(*parts)).read_text(encoding="utf-8")

    def test_headless_put_rejects_non_integer_display_profile(self):
        source = self.read("src", "app_win", "headless.c")
        self.assertIn(
            'has_profile && !json_get_int(body, L"displayProfile", &display_profile)',
            source,
        )
        self.assertIn('"displayProfile must be an integer (0 or 1)"', source)

    def test_status_exposes_three_layer_runtime_state(self):
        source = self.read("src", "app_win", "headless.c")
        for field in (
            'displayProfile',
            'guestDisplayProfile',
            'activeDisplayProfile',
            'displayBackend',
            'displayProfileState',
            'rebootRequired',
            'displayProfileReason',
        ):
            self.assertIn(field, source)

    def test_guest_profile_is_exact_and_codec_aware(self):
        source = self.read("tools", "linux", "agent", "appsandbox-agent.c")
        self.assertIn('width == 3840 && height == 2160 && refresh == 60', source)
        self.assertIn('!strcmp(codec, "hevc444")', source)
        self.assertIn('width == 1920 && height == 1080 && refresh == 60', source)
        self.assertIn('!strcmp(codec, "hevc420")', source)
        self.assertIn("configured_profile=%s;active_profile=%s;reboot_required=%d", source)

    def test_host_selector_requires_actual_gpu_ayuv_decode(self):
        source = self.read("src", "backend_win", "vm_video_decode.c")
        for marker in (
            "MFTEnumEx",
            "IMFTransform_ProcessInput",
            "IMFTransform_ProcessOutput",
            "IMFDXGIBuffer",
            "DXGI_FORMAT_AYUV",
            "VideoProcessorBlt",
            "vm_video_decode_probe_profile",
        ):
            self.assertIn(marker, source)
        self.assertIn("profile == VM_VIDEO_HEVC444", source)

    def test_render_failure_is_cross_thread_fatal_and_reconnectable(self):
        source = self.read("src", "backend_win", "vm_display_idd.c")
        for marker in (
            "video_path_failed",
            "video_path_failure_hr",
            "recv_exact_display",
            "hevc444-video-processor-blt-failed",
            "reconnecting=1",
            "ASB_DISPLAY_BACKEND_RAW_ASFR",
        ):
            self.assertIn(marker, source)

    def test_reconcile_has_one_shot_reboot_guard(self):
        source = self.read("src", "backend_win", "vm_agent.c")
        for marker in (
            "display_profile_reboot_issued",
            "display_profile_reboot_generation",
            "profile-reconcile-failed-after-reboot",
            "guest-restart-required",
            "parse_display_profile_response",
        ):
            self.assertIn(marker, source)


if __name__ == "__main__":
    unittest.main()
