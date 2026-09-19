import importlib.util
import io
import json
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAKE_BUNDLE = ROOT / "make_bundle.py"


def tool(name):
    path = shutil.which(name)
    if not path:
        raise unittest.SkipTest(f"{name} is required for bundle behavior tests")
    return path


def run(*args, check=True):
    return subprocess.run(args, check=check, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


class SignedBundleBehaviorTests(unittest.TestCase):
    def make_bundle(self, root, extra_args=()):
        openssl = tool("openssl")
        zstd = tool("zstd")
        tool("tar")
        payload = root / "payload"
        payload.mkdir()
        (payload / "config").mkdir()
        (payload / "config" / "asb_drm.conf").write_text(
            "options asb_drm width=3840 height=2160 refresh=60\n"
        )
        graphics_root = root / "graphics-root" / "opt" / "wsl-mesa" / "lib"
        graphics_root.mkdir(parents=True)
        (graphics_root / "libEGL.so.1").write_bytes(b"mesa")
        graphics_tar = root / "graphics.tar"
        with tarfile.open(graphics_tar, "w") as archive:
            archive.add(root / "graphics-root" / "opt", arcname="opt")
        graphics_archive = root / "graphics.tar.zst"
        with graphics_archive.open("wb") as output:
            result = subprocess.run([zstd, "--quiet", "-c", str(graphics_tar)], stdout=output)
            self.assertEqual(result.returncode, 0)
        (payload / "graphics").mkdir()
        shutil.copyfile(graphics_archive, payload / "graphics" / "wsl-mesa.tar.zst")

        private = root / "signing-key.pem"
        public = root / "signing-key.pub.pem"
        run(openssl, "genpkey", "-algorithm", "ED25519", "-out", str(private))
        run(openssl, "pkey", "-in", str(private), "-pubout", "-out", str(public))
        output = root / "guest-runtime.tar.zst"
        run(
            sys.executable,
            str(MAKE_BUNDLE),
            "--payload",
            str(payload),
            "--version",
            "1.0.1",
            "--kind",
            "graphics",
            "--commit",
            "test-commit",
            "--graphics-version",
            "1.0.0",
            "--signing-key",
            str(private),
            "--output",
            str(output),
            *extra_args,
        )
        with output.open("rb") as bundle:
            outer = subprocess.run([zstd, "--quiet", "-d", "-c"], stdin=bundle, stdout=subprocess.PIPE, check=True)
        return output, public, outer.stdout

    def test_valid_signed_graphics_bundle_is_complete_and_verifiable(self):
        openssl = tool("openssl")
        with tempfile.TemporaryDirectory() as temp:
            bundle, public, tar_bytes = self.make_bundle(pathlib.Path(temp))
            with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:") as archive:
                manifest_bytes = archive.extractfile("manifest.json").read()
                signature = archive.extractfile("manifest.sig").read()
                manifest = json.loads(manifest_bytes)
                names = archive.getnames()
                graphics_member = archive.extractfile("payload/graphics/wsl-mesa.tar.zst").read()
            self.assertIn("payload/graphics/wsl-mesa.tar.zst", names)
            self.assertEqual(manifest["arch"], "amd64")
            self.assertEqual(manifest["os"], "ubuntu-26.04")
            self.assertEqual(manifest["kind"], "graphics")
            self.assertFalse(manifest["kernel_components_present"])
            verify = pathlib.Path(temp) / "manifest.json"
            sig = pathlib.Path(temp) / "manifest.sig"
            verify.write_bytes(manifest_bytes)
            sig.write_bytes(signature)
            result = run(
                openssl,
                "pkeyutl",
                "-verify",
                "-rawin",
                "-pubin",
                "-inkey",
                str(public),
                "-in",
                str(verify),
                "-sigfile",
                str(sig),
                check=False,
            )
            self.assertEqual(result.returncode, 0)
            graphics_path = pathlib.Path(temp) / "graphics-member.zst"
            graphics_path.write_bytes(graphics_member)
            mesa_tar = subprocess.run(
                [tool("zstd"), "--quiet", "-d", "-c", str(graphics_path)],
                stdout=subprocess.PIPE,
                check=True,
            ).stdout
            with tarfile.open(fileobj=io.BytesIO(mesa_tar), mode="r:") as mesa:
                self.assertIn("opt/wsl-mesa/lib/libEGL.so.1", mesa.getnames())

    def test_wrong_signature_is_rejected(self):
        openssl = tool("openssl")
        with tempfile.TemporaryDirectory() as temp:
            _, public, tar_bytes = self.make_bundle(pathlib.Path(temp))
            root = pathlib.Path(temp)
            with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:") as archive:
                manifest = archive.extractfile("manifest.json").read()
            manifest_path = root / "manifest.json"
            signature_path = root / "manifest.sig"
            manifest_path.write_bytes(manifest)
            signature_path.write_bytes(b"\0" * 64)
            result = run(
                openssl,
                "pkeyutl",
                "-verify",
                "-rawin",
                "-pubin",
                "-inkey",
                str(public),
                "-in",
                str(manifest_path),
                "-sigfile",
                str(signature_path),
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)

    def test_builder_rejects_kernel_payloads(self):
        spec = importlib.util.spec_from_file_location("make_bundle", MAKE_BUNDLE)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with self.assertRaises(ValueError):
            module.validate_runtime_path("modules/dxgkrnl.ko")
        with self.assertRaises(ValueError):
            module.validate_runtime_path("lib/asb_drm.ko")

    def test_builder_rejects_non_production_target_policy(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "payload").mkdir()
            (root / "payload" / "file").write_text("x")
            key = root / "key.pem"
            run(tool("openssl"), "genpkey", "-algorithm", "ED25519", "-out", str(key))
            result = run(
                sys.executable,
                str(MAKE_BUNDLE),
                "--payload",
                str(root / "payload"),
                "--version",
                "1.0.0",
                "--commit",
                "test",
                "--arch",
                "arm64",
                "--signing-key",
                str(key),
                "--output",
                str(root / "out.tar.zst"),
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)

    def test_builder_enforces_runtime_closure_and_graphics_isolation(self):
        spec = importlib.util.spec_from_file_location("make_bundle", MAKE_BUNDLE)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        core = set(module.REQUIRED_RUNTIME)
        module.validate_composition("runtime", core, "")
        with self.assertRaises(ValueError):
            module.validate_composition("runtime", core - {"bin/appsandbox-agent"}, "")
        with self.assertRaises(ValueError):
            module.validate_composition("runtime", core - {"systemd/appsandbox-agent.service"}, "")
        module.validate_composition("graphics", {"graphics/wsl-mesa.tar.zst", "config/asb_drm.conf"}, "1.0.0")
        with self.assertRaises(ValueError):
            module.validate_composition("graphics", {"graphics/wsl-mesa.tar.zst", "bin/appsandbox-agent"}, "1.0.0")


if __name__ == "__main__":
    unittest.main()
