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

    def test_host_hello_is_gated_by_cached_actual_probe_not_registration(self):
        source = self.read("src", "backend_win", "vm_display_idd.c")
        hello = source[source.index("requested_hevc444 ="):
                       source.index("d->cursor_visible = TRUE")]
        self.assertIn("idd_probe_hevc444_capability(d, FALSE)", hello)
        self.assertNotIn("vm_video_decode_supported_profile(d->device, VM_VIDEO_HEVC444)", hello)
        self.assertNotIn("production-4k60-v1", hello)
        self.assertIn("hevc444_capability", hello)

    def test_selector_identity_is_reused_or_reprobed(self):
        header = self.read("src", "backend_win", "vm_video_decode.h")
        source = self.read("src", "backend_win", "vm_video_decode.c")
        idd = self.read("src", "backend_win", "vm_display_idd.c")
        for field in ("decoder_clsid", "decoder_name", "decoder_identity_valid",
                      "actual_decode", "gpu_surface"):
            self.assertIn(field, header)
        self.assertIn("vm_video_decoder_create_with_capability", source)
        self.assertIn("MFT_TRANSFORM_CLSID_Attribute", source)
        self.assertIn("decoder_identity_valid", source)
        self.assertIn("&d->hevc444_capability", idd)
        self.assertIn("idd_probe_hevc444_capability(d, TRUE)", idd)

    def test_ayuv_probe_uses_immediate_context_video_context(self):
        source = self.read("src", "backend_win", "vm_video_decode.c")
        self.assertIn("ID3D11Device_GetImmediateContext", source)
        self.assertIn("ID3D11DeviceContext_QueryInterface", source)
        self.assertNotIn(
            "ID3D11Device_QueryInterface(device, &IID_ID3D11VideoContext",
            source,
        )
        self.assertIn("VideoProcessorBlt", source)

    def test_bundled_probe_sample_has_vps_sps_pps_and_irap(self):
        source = self.read("src", "backend_win", "vm_hevc444_probe_sample.h")
        values = []
        in_array = False
        for line in source.splitlines():
            if "asb_hevc444_probe_sample[]" in line:
                in_array = True
                continue
            if in_array and line.strip() == "};":
                break
            if in_array:
                values.extend(int(token.strip().rstrip(","), 16)
                              for token in line.strip().split(",") if token.strip())
        self.assertGreater(len(values), 1000)
        starts = []
        i = 0
        while i + 3 < len(values):
            if values[i:i + 4] == [0, 0, 0, 1]:
                starts.append(i)
                i += 4
            elif values[i:i + 3] == [0, 0, 1]:
                starts.append(i)
                i += 3
            else:
                i += 1
        types = []
        for index, start in enumerate(starts):
            prefix = 4 if values[start:start + 4] == [0, 0, 0, 1] else 3
            nal = start + prefix
            types.append((values[nal] >> 1) & 0x3f)
        self.assertTrue({32, 33, 34}.issubset(types))
        self.assertTrue(any(16 <= nal_type <= 23 for nal_type in types))
        self.assertIn("ASB_HEVC444_PROBE_EXTRADATA_SIZE 84u", source)
        self.assertIn("ASB_HEVC444_PROBE_ACCESS_UNIT_OFFSET 84u", source)

    def test_native_encode_probe_has_real_submission_gate(self):
        source = self.read("tools", "win", "hevc444-encode-probe",
                           "hevc444-encode-probe.cpp")
        for marker in (
            "CreateVideoEncoder",
            "CreateVideoEncoderHeap",
            "EncodeFrame",
            "ResolveEncoderOutputMetadata",
            "host_d3d12_hevc444_actual_encode",
            "host_d3d12_hevc444_4k60",
        ):
            self.assertIn(marker, source)
        self.assertIn("D3D12_VIDEO_ENCODER_CODEC_HEVC", source)
        self.assertIn("DXGI_FORMAT_AYUV", source)

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
