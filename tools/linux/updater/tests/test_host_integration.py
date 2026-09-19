"""Compile and execute the Host's actual path, ServiceTable and file gates.

Run on Windows from an x64 Visual Studio developer prompt. Linux test runs
skip this suite; it exercises Win32 file sharing rather than mocking it.
"""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[4]


@unittest.skipUnless(os.name == "nt" and shutil.which("cl"), "requires Windows MSVC")
class HostIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        update = (ROOT / "src/backend_win/vm_guest_update.c").read_text(encoding="utf-8")
        hcs = (ROOT / "src/backend_win/hcs_vm.c").read_text(encoding="utf-8")
        path = update[update.index("static BOOL windows_path_to_wsl("):
                      update.index("static BOOL run_wsl_args(")]
        gate = update[update.index("static BOOL wait_for_vhdx_release("):
                      update.index("static BOOL restart_after_offline_migration(")]
        guids = hcs[hcs.index("void hcs_service_guid("):
                    hcs.index("/* HCS_E_OPERATION_TIMEOUT")]
        quoting = update[update.index("static BOOL append_quoted_arg("):
                         update.index("/* The helper is a release artifact")]
        verifier = update[update.index("static BOOL verify_manifest_with_pinned_helper("):
                          update.index("static BOOL bundle_preflight(")]
        table = hcs[hcs.index("    {", hcs.index("/* ServiceTable:")):
                    hcs.index("    /* Nested virtualization", hcs.index("/* ServiceTable:"))]
        cls.temp = tempfile.TemporaryDirectory()
        cls.exe = pathlib.Path(cls.temp.name) / "host_integration.exe"
        source = pathlib.Path(cls.temp.name) / "host_integration.c"
        source.write_text(r'''
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <assert.h>
#include <string.h>
#define ASB_BUNDLE_VERIFIER_SHA256 "test-pinned-hash"
#define PREFLIGHT_TIMEOUT_MS 30000UL
static wchar_t executable[MAX_PATH];
static BOOL get_linux_resource_path(const wchar_t *relative, wchar_t *out, size_t cap) {
    (void)relative; return wcscpy_s(out, cap, executable) == 0;
}
static BOOL verify_pinned_resource(const wchar_t *path, const char *sha) {
    (void)sha; return !wcscmp(path, executable);
}
static void ui_log(const wchar_t *fmt, ...) { (void)fmt; }
static BOOL fast_clock;
static DWORD fake_ticks;
static DWORD test_ticks(void) {
    if (fast_clock) { fake_ticks += 60000; return fake_ticks; }
    return GetTickCount();
}
#define GetTickCount test_ticks
''' + path + gate + guids + quoting + verifier + r'''
static void table_for(const wchar_t *os, wchar_t service_table[2048]) {
    struct { const wchar_t *os_type; } cfg = {os}, *config = &cfg;
''' + table + r'''
}
static DWORD WINAPI release_file(void *p) {
    Sleep(50); CloseHandle((HANDLE)p); return 0;
}
int main(int argc, char **argv) {
    wchar_t out[2048], file[MAX_PATH], temp[MAX_PATH];
    HANDLE held, thread;
    if (argc > 1) {
        return !(argc == 4 && !strcmp(argv[1], "--verify-bundle") &&
                 !strcmp(argv[2], "C:\\bundle with space.tar.zst") && strlen(argv[3]) == 64);
    }
    GetModuleFileNameW(NULL, executable, MAX_PATH);
    assert(verify_manifest_with_pinned_helper(L"C:\\bundle with space.tar.zst",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    puts("PASS actual CreateProcess verifier argv and path quoting");
    assert(windows_path_to_wsl(L"C:\\repo\\a b\\file.py", out, 2048));
    assert(!wcscmp(out, L"/mnt/c/repo/a b/file.py"));
    assert(windows_path_to_wsl(L"\\\\?\\D:\\guest\\update", out, 2048));
    assert(!wcscmp(out, L"/mnt/d/guest/update"));
    assert(windows_path_to_wsl(L"z:/", out, 8));
    assert(!wcscmp(out, L"/mnt/z/"));
    assert(!windows_path_to_wsl(L"C:relative", out, 2048));
    assert(!windows_path_to_wsl(L"\\\\server\\share", out, 2048));
    assert(!windows_path_to_wsl(L"", out, 2048));
    assert(!windows_path_to_wsl(NULL, out, 2048));
    assert(!windows_path_to_wsl(L"C:\\x", out, 8));
    assert(windows_path_to_wsl(L"C:\\x", out, 9));
    puts("PASS Windows-to-WSL paths and capacity boundaries");

    table_for(L"Linux", out);
    assert(wcsstr(out, L"00000009-facb-11e6-bd58-64006a7986d3"));
    assert(wcsstr(out, L"00000006-facb-11e6-bd58-64006a7986d3"));
    assert(!wcsstr(out, L"00000007-facb"));
    assert(!wcsstr(out, L"00000008-facb"));
    table_for(L"Windows", out);
    assert(wcsstr(out, L"a5b0cafe-0006-4000-8000-000000000001"));
    assert(!wcsstr(out, L"a5b0cafe-0009"));
    puts("PASS Linux update ServiceTable and Windows ports");

    GetTempPathW(MAX_PATH, temp);
    assert(GetTempFileNameW(temp, L"asb", 0, file));
    held = CreateFileW(file, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(held != INVALID_HANDLE_VALUE);
    fast_clock = TRUE;
    assert(!wait_for_vhdx_release(file)); /* Shared open is insufficient. */
    fast_clock = FALSE;
    thread = CreateThread(NULL, 0, release_file, held, 0, NULL);
    assert(thread);
    assert(wait_for_vhdx_release(file)); /* Waits until other handle closes. */
    WaitForSingleObject(thread, INFINITE); CloseHandle(thread);
    assert(DeleteFileW(file));
    assert(!wait_for_vhdx_release(file)); /* Missing file must fail closed. */
    puts("PASS exclusive VHDX release, retry, timeout and missing file");
    return 0;
}
''', encoding="utf-8")
        result = subprocess.run(["cl", "/nologo", "/utf-8", "/W4", str(source),
                                 "/Fe:" + str(cls.exe)], cwd=cls.temp.name,
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_host_integration(self):
        result = subprocess.run([str(self.exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout)


if __name__ == "__main__":
    unittest.main()
