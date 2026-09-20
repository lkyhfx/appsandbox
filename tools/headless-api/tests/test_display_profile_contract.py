import unittest
from pathlib import Path
import subprocess


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
        compat = self.read("tools", "win", "hevc444-encode-probe",
                           "d3d12video_hevc1_compat.h")
        for marker in (
            "CreateVideoEncoder",
            "CreateVideoEncoderHeap",
            "EncodeFrame",
            "ResolveEncoderOutputMetadata",
            "MinSupportedLevel.pHEVCLevelSetting",
            "MaxSupportedLevel.pHEVCLevelSetting",
            "D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC1",
            "D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1",
            "encode_queue->Wait(copy_fence.Get(), 1)",
            "EncodeErrorFlags",
            "EncodedBitstreamWrittenBytesCount",
            "parse_hevc444_sequence_headers",
            "host_d3d12_hevc444_actual_encode",
            "host_d3d12_hevc444_4k60",
        ):
            self.assertIn(marker, source)
        self.assertIn("install_hevc1_support", compat)
        self.assertIn("install_hevc1_picture_control", compat)
        self.assertIn("pHEVCSupport1", compat)
        self.assertIn("pHEVCPicData1", compat)
        self.assertRegex(
            source,
            r"(?s)D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC1\s+limits"
            r".*?install_hevc1_support\(&query, &limits\)",
        )
        self.assertRegex(
            source,
            r"(?s)StateBefore = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE"
            r".*?StateAfter = D3D12_RESOURCE_STATE_COMMON",
        )
        self.assertIn("D3D12_VIDEO_ENCODER_CODEC_HEVC", source)
        self.assertIn("DXGI_FORMAT_AYUV", source)
        self.assertIn("D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_IDR_FRAME", source)
        self.assertIn("host_d3d12_hevc444_encoder_payload", source)
        self.assertIn("D3D12_FEATURE_VIDEO_ENCODER_SUPPORT", source)
        self.assertIn("TargetFrameRate", source)
        self.assertIn("D3D12_VIDEO_ENCODER_VALIDATION_FLAG_NONE", source)

    def test_native_d3d12_decode_probe_submits_and_verifies_gpu_output(self):
        source = self.read("tools", "win", "hevc444-probe",
                           "hevc444-probe.cpp")
        for marker in (
            "D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS",
            "D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL",
            "DXVA_PicParams_HEVC_RangeExt",
            "ID3D12VideoDecodeCommandList",
            "DecodeFrame",
            "D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS",
            "d3d12_decode_status",
            "CopyTextureRegion",
            "d3d12_decoded_ayuv_checksum",
            "d3d12_decoded_gpu_surface",
            "d3d12_decode_ayuv_to_rgb",
            "d3d12_decode_cpu_frame_copy",
        ):
            self.assertIn(marker, source)
        self.assertNotIn("d3d12_actual_decode=not-tested", source)

    def test_native_d3d12_decode_runtime_is_fail_closed(self):
        exe = (ROOT / "tools" / "win" / "hevc444-probe" / "bin" /
               "Release" / "appsandbox-hevc444-probe.exe")
        sample = ROOT / "tools" / "win" / "hevc444-probe" / \
            "hevc444-host-probe-sample.hevc"
        if not exe.exists():
            self.skipTest("Windows D3D12 decode probe has not been built")
        result = subprocess.run([str(exe), str(sample)], capture_output=True,
                                text=True, timeout=60, check=False)
        output = result.stdout + result.stderr
        self.assertIn("d3d12_actual_decode=", output)
        self.assertIn("d3d12_decode_cpu_frame_copy=0", output)
        if "d3d12_actual_decode=PASS" in output:
            self.assertIn("d3d12_decoded_format=AYUV", output)
            self.assertIn("d3d12_decoded_gpu_surface=1", output)
            self.assertIn("d3d12_decode_ayuv_to_rgb=1", output)
        else:
            self.assertIn("d3d12_actual_decode=BLOCKED", output)

    def test_native_encode_probe_runtime_is_fail_closed(self):
        exe = (ROOT / "tools" / "win" / "hevc444-encode-probe" / "bin" /
               "Release" / "appsandbox-hevc444-encode-probe.exe")
        if not exe.exists():
            self.skipTest("Windows hardware probe has not been built")
        result = subprocess.run([str(exe)], capture_output=True, text=True,
                                timeout=30, check=False)
        output = result.stdout + result.stderr
        self.assertIn("host_d3d12_hevc444_4k60=", output)
        self.assertIn("host_d3d12_hevc444_actual_encode=", output)
        if "host_d3d12_hevc444_actual_encode=PASS" in output:
            self.assertIn("host_d3d12_hevc444_encode_error_flags=0x0", output)
            self.assertRegex(output, r"host_d3d12_hevc444_encoded_bytes=[1-9][0-9]*")
            self.assertIn("host_d3d12_hevc444_sequence_header_444=1", output)
            self.assertIn("host_d3d12_hevc444_irap=1", output)
        else:
            self.assertIn("host_d3d12_hevc444_actual_encode=BLOCKED", output)

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
