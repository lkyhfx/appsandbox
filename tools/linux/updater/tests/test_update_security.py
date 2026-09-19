import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "appsandbox-guest-updater.c").read_text()
MAKEFILE = (ROOT / "Makefile").read_text()


class UpdateSecurityContractTests(unittest.TestCase):
    def test_bounded_transport_and_exclusive_staging(self):
        self.assertIn("UPDATE_MAX_BUNDLE", SOURCE)
        self.assertIn("O_CREAT | O_EXCL", SOURCE)
        self.assertIn("O_NOFOLLOW", SOURCE)
        self.assertIn("fsync(out)", SOURCE)

    def test_archive_paths_and_special_files_are_rejected(self):
        self.assertIn("safe_relpath", SOURCE)
        self.assertIn("type != '0'", SOURCE)
        self.assertIn("goto fail", SOURCE)
        self.assertIn("tar_path", SOURCE)

    def test_signature_and_compatibility_are_mandatory(self):
        self.assertIn("verify_signature", SOURCE)
        self.assertIn("host_protocol_min", SOURCE)
        self.assertIn("updater_min_version", SOURCE)
        self.assertIn("downgrade_rejected", SOURCE)

    def test_release_key_is_explicit_and_test_key_is_rejected(self):
        self.assertIn("PUBLIC_KEY_HEX must be provided", MAKEFILE)
        self.assertIn("PUBLIC_KEY_HEX must be exactly 64 hexadecimal characters", MAKEFILE)
        self.assertIn("PUBLIC_KEY_HEX must not be all zero", MAKEFILE)
        self.assertIn("PUBLIC_KEY_HEX is a known RFC/test key", MAKEFILE)
        self.assertIn("ASB_UPDATE_PUBLIC_KEY_HEX must be supplied", SOURCE)
        self.assertNotIn("#define ASB_UPDATE_PUBLIC_KEY_HEX \\", SOURCE)

    def test_graphics_moves_before_install_and_stable_links_use_current(self):
        self.assertIn('MOVE_PAYLOAD_DIR("graphics")', SOURCE)
        self.assertIn('"/opt/wsl-mesa/current"', SOURCE)
        self.assertIn('UPDATE_ROOT "/current/bin/%s"', SOURCE)
        self.assertIn('read_current_version', SOURCE)

    def test_provisioned_baseline_is_semver_valid(self):
        vhdx = (ROOT.parents[2] / "tools" / "iso-patch" / "ubuntu_vhdx.c").read_text(
            encoding="utf-8", errors="replace")
        self.assertIn("ASB_BASE_VERSION=0.0.0", vhdx)
        self.assertIn("graphics_version=0.0.0", vhdx)
        self.assertNotIn("version=initial\\\\ngraphics_version=legacy-provisioned", vhdx)
        self.assertIn("appsandbox-guest-update-watch.service", vhdx)

    def test_release_build_emits_prefetch_release_layout(self):
        linux_make = (ROOT.parents[2] / "tools" / "linux" / "Makefile").read_text(
            encoding="utf-8", errors="replace")
        self.assertIn("$(DISTDIR)/updater/appsandbox-guest-updater.sha256", linux_make)
        self.assertIn("$(DISTDIR)/systemd", linux_make)
        self.assertIn("$(DISTDIR)/agent-src", linux_make)
        self.assertIn("$(DISTDIR)/dxgkrnl-src", linux_make)

    def test_release_build_builds_and_pins_host_verifier(self):
        linux_make = (ROOT.parents[2] / "tools" / "linux" / "Makefile").read_text(
            encoding="utf-8", errors="replace")
        updater_make = (ROOT / "Makefile").read_text(encoding="utf-8", errors="replace")
        release = (ROOT.parents[2] / "tools" / "sign" / "make-release.ps1").read_text(
            encoding="utf-8", errors="replace")
        self.assertIn("host-verifier", linux_make)
        self.assertIn("appsandbox-guest-bundle-verifier.exe.sha256", linux_make)
        self.assertIn("GOOS=$(HOST_GOOS)", updater_make)
        self.assertIn("AsbBundleVerifierSha256", release)
        self.assertIn("Guest Update release tree is incomplete", release)

    def test_activation_metadata_is_durable_before_live_switch(self):
        self.assertIn('strcpy(s.state, "activating")', SOURCE)
        self.assertIn('old_runtime_target', SOURCE)
        self.assertIn('new_runtime_target', SOURCE)
        self.assertIn('old_graphics_target', SOURCE)
        self.assertIn('new_graphics_target', SOURCE)
        self.assertIn('old_config_backup', SOURCE)
        self.assertIn('s.graphics_changed', SOURCE)

    def test_async_apply_and_legacy_migration_exist(self):
        self.assertIn('strcpy(s.state, "apply_requested")', SOURCE)
        self.assertIn('puts("accepted")', SOURCE)
        self.assertIn('--migrate', SOURCE)
        self.assertIn('/usr/local/libexec/%s', SOURCE)

    def test_kernel_components_are_not_in_normal_update(self):
        self.assertIn("kernel_components_present", SOURCE)
        self.assertIn("dxgkrnl", SOURCE)
        self.assertIn("asb_drm.ko", SOURCE)

    def test_no_update_shell_escape_hatch(self):
        self.assertNotIn("/bin/sh", SOURCE)
        self.assertNotIn("system(", SOURCE)


if __name__ == "__main__":
    unittest.main()
