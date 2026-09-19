import pathlib
import shutil
import subprocess
import tarfile
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "appsandbox-guest-updater.c").read_text()


class GraphicsLinkTests(unittest.TestCase):
    def test_actual_link_validator_accepts_mesa_and_rejects_escape(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        start = SOURCE.index("static int safe_component(")
        end = SOURCE.index("static int join_path(", start)
        with tempfile.TemporaryDirectory() as temp:
            source = pathlib.Path(temp) / "graphics_link_test.c"
            binary = pathlib.Path(temp) / "graphics_link_test"
            source.write_text(
                "#include <limits.h>\n#include <stddef.h>\n#include <string.h>\n"
                "#ifndef PATH_MAX\n#define PATH_MAX 4096\n#endif\n"
                "#ifndef NAME_MAX\n#define NAME_MAX 255\n#endif\n"
                + SOURCE[start:end]
                + "int main(int argc, char **argv) {\n"
                "  return argc == 3 && safe_graphics_link(argv[1], argv[2]) ? 0 : 1;\n}"
            )
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(source), "-o", str(binary)], check=True)
            allowed = (
                ("lib/x86_64-linux-gnu/dri/d3d12_dri.so", "libdril_dri.so"),
                ("lib/x86_64-linux-gnu/dri/d3d12_drv_video.so", "../libgallium-25.3.6.so"),
                ("lib/x86_64-linux-gnu/libEGL_mesa.so", "libEGL_mesa.so.0"),
            )
            rejected = (
                ("lib/x/link", "/etc/passwd"),
                ("lib/x/link", "../../../etc/passwd"),
                ("lib/x/link", "foo/../../../../etc/passwd"),
                ("lib/x/link", "foo//bar"),
                ("lib/x/link", "foo\\bar"),
                ("lib/x/link", ""),
            )
            for link, target in allowed:
                with self.subTest(link=link, target=target):
                    self.assertEqual(subprocess.run([binary, link, target]).returncode, 0)
            for link, target in rejected:
                with self.subTest(link=link, target=target):
                    self.assertNotEqual(subprocess.run([binary, link, target]).returncode, 0)

            artifact = ROOT.parent / "wsl-mesa/prebuilt/ubuntu-26.04-amd64/wsl-mesa.tar.zst"
            zstd = shutil.which("zstd")
            if artifact.is_file() and zstd:
                reader = subprocess.Popen([zstd, "-dc", str(artifact)],
                                          stdout=subprocess.PIPE,
                                          stderr=subprocess.DEVNULL)
                links = 0
                try:
                    with tarfile.open(fileobj=reader.stdout, mode="r|") as archive:
                        for entry in archive:
                            if not entry.issym():
                                continue
                            links += 1
                            self.assertTrue(entry.name.startswith("opt/wsl-mesa/"))
                            relative = entry.name[len("opt/wsl-mesa/"):]
                            with self.subTest(link=entry.name, target=entry.linkname):
                                self.assertEqual(
                                    subprocess.run([binary, relative, entry.linkname]).returncode, 0)
                    reader.stdout.read()
                    self.assertEqual(reader.wait(), 0)
                    self.assertGreater(links, 0)
                finally:
                    if reader.poll() is None:
                        reader.kill()
                        reader.wait()
                    reader.stdout.close()


if __name__ == "__main__":
    unittest.main()
