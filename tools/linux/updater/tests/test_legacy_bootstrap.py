import importlib.util
import os
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("legacy_bootstrap", ROOT / "offline_legacy_bootstrap.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class LegacyBootstrapTests(unittest.TestCase):
    def test_migrates_old_layout_and_installs_stable_links(self):
        if os.name == "nt":
            self.skipTest("Linux symlink semantics require a Linux test environment")
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "usr/local/bin").mkdir(parents=True)
            (root / "usr/local/bin/appsandbox-agent").write_bytes(b"old-agent")
            (root / "etc/systemd/system").mkdir(parents=True)
            (root / "etc/systemd/system/appsandbox-agent.service").write_text("[Unit]\n")
            (root / "opt/wsl-mesa/current").mkdir(parents=True)
            (root / "opt/wsl-mesa/current/GRAPHICS").write_text("1.2.3\n")
            (root / "opt/wsl-mesa/legacy.txt").write_text("old-mesa")
            updater = root / "updater"
            service = root / "updater.service"
            watch = root / "watch.service"
            updater.write_bytes(b"updater")
            service.write_text("[Service]\n")
            watch.write_text("[Service]\n")
            MODULE.migrate(root, updater, service, watch)
            release = root / "opt/appsandbox/guest/releases/legacy"
            self.assertEqual((release / "bin/appsandbox-agent").read_bytes(), b"old-agent")
            self.assertEqual((release / "RELEASE").read_text(),
                             "version=0.0.0\ngraphics_version=1.2.3\ncommit=legacy-bootstrap\n")
            self.assertEqual((release / "graphics/legacy-mesa/legacy.txt").read_text(), "old-mesa")
            self.assertTrue((root / "opt/appsandbox/guest/current").is_symlink())
            current = root / "opt/appsandbox/guest/current"
            bootstrap = root / "opt/appsandbox/guest/releases/bootstrap-0.0.0"
            self.assertEqual(os.readlink(root / "opt/appsandbox/guest/current"),
                             "releases/bootstrap-0.0.0")
            self.assertEqual(os.readlink(root / "opt/appsandbox/guest/previous"),
                             "releases/legacy-original")
            self.assertNotEqual((current / "bin/appsandbox-agent").read_bytes(),
                                b"old-agent")
            self.assertEqual((bootstrap / "libexec/appsandbox-guest-updater").read_bytes(),
                             b"updater")
            self.assertTrue((root / "usr/local/bin/appsandbox-agent").is_symlink())
            self.assertEqual(os.readlink(root / "usr/local/libexec/appsandbox-guest-updater"),
                             "/opt/appsandbox/guest/current/libexec/appsandbox-guest-updater")
            self.assertTrue((root / "etc/systemd/system/multi-user.target.wants/appsandbox-guest-updater.service").is_symlink())

    def test_failure_keeps_legacy_files_untouched(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "usr/local/bin").mkdir(parents=True)
            old = root / "usr/local/bin/appsandbox-agent"
            old.write_bytes(b"old-agent")
            with self.assertRaises(RuntimeError):
                MODULE.migrate(root, root / "missing", root / "service", root / "watch")
            self.assertEqual(old.read_bytes(), b"old-agent")
            self.assertFalse((root / "opt/appsandbox/guest/current").exists())


if __name__ == "__main__":
    unittest.main()
