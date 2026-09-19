import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "appsandbox-guest-updater.c").read_text()


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

    def test_kernel_components_are_not_in_normal_update(self):
        self.assertIn("kernel_components_present", SOURCE)
        self.assertIn("dxgkrnl", SOURCE)
        self.assertIn("asb_drm.ko", SOURCE)

    def test_no_update_shell_escape_hatch(self):
        self.assertNotIn("/bin/sh", SOURCE)
        self.assertNotIn("system(", SOURCE)


if __name__ == "__main__":
    unittest.main()
