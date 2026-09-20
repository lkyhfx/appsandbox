/* Build with: cl /nologo /utf-8 /I src/backend_win /I src/app_win
   tests/linux_tty_test.c /Fe:tests/linux_tty_test.exe */
#define ASB_BUILDING_DLL
#include "../src/backend_win/linux_tty.c"
#include <string.h>

static void check(BOOL value, const char *reason)
{
    if (!value) { fprintf(stderr, "FAIL: %s\n", reason); exit(1); }
}

static HANDLE server(const wchar_t *name)
{
    wchar_t path[512];
    swprintf_s(path, ARRAYSIZE(path), L"%s%s.com1", LINUX_TTY_PIPE_PREFIX, name);
    return CreateNamedPipeW(path, PIPE_ACCESS_OUTBOUND,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            1, 16384, 16384, 0, NULL);
}

static void wait_for_size(const wchar_t *path, DWORD expected)
{
    for (int i = 0; i < 100; i++) {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &data) &&
            data.nFileSizeLow >= expected) return;
        Sleep(20);
    }
    check(FALSE, "expected tty.log bytes");
}

static DWORD WINAPI race_start(LPVOID p)
{
    linux_tty_start((VmInstance *)p);
    return 0;
}

static DWORD WINAPI race_stop(LPVOID p)
{
    linux_tty_stop((VmInstance *)p);
    return 0;
}

int main(void)
{
    static VmInstance vm, moved;
    LinuxTtyCapture other = { 0 };
    VmInstance other_vm = { 0 };
    HANDLE pipe, thread, thread2, file;
    DWORD written, got, handles_before, handles_after;
    char raw[256];
    const char *first = "hello world\nx\xff\n";
    const char *second = "again\r\n";
    wchar_t exe[MAX_PATH], dir[MAX_PATH], vm_dir[MAX_PATH], path[MAX_PATH];
    wchar_t *slash;

    check(GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe)) > 0, "exe path");
    slash = wcsrchr(exe, L'\\');
    check(slash != NULL, "exe directory");
    *slash = L'\0';
    swprintf_s(dir, ARRAYSIZE(dir), L"%s\\tty-logs", exe);
    swprintf_s(vm.name, ARRAYSIZE(vm.name), L"tty_test_%lu", GetCurrentProcessId());
    swprintf_s(vm_dir, ARRAYSIZE(vm_dir), L"%s\\%s", dir, vm.name);
    swprintf_s(path, ARRAYSIZE(path), L"%s\\tty.log", vm_dir);
    wcscpy_s(vm.os_type, ARRAYSIZE(vm.os_type), L"Linux");
    swprintf_s(other_vm.name, ARRAYSIZE(other_vm.name), L"tty_other_%lu",
               GetCurrentProcessId());
    check(tty_prepare_paths(&other, &other_vm), "second VM path");
    check(wcscmp(other.tty_path, path) != 0, "VM logs are isolated");
    {
        wchar_t *last = wcsrchr(other.tty_path, L'\\');
        check(last != NULL, "second VM directory");
        *last = L'\0';
        RemoveDirectoryW(other.tty_path);
    }
    wcscpy_s(other_vm.name, ARRAYSIZE(other_vm.name), L"..\\escape");
    check(!tty_prepare_paths(&other, &other_vm), "VM name cannot escape exe directory");

    linux_tty_start(&vm);
    check(vm.linux_tty_capture != NULL, "capture starts before pipe exists");
    linux_tty_transfer(&vm, &moved);
    check(!vm.linux_tty_capture && moved.linux_tty_capture,
          "single capture owner after transfer");
    pipe = server(vm.name);
    check(pipe != INVALID_HANDLE_VALUE, "create pipe");
    check(ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED,
          "connect pipe");
    WriteFile(pipe, first, (DWORD)strlen(first), &written, NULL);
    wait_for_size(path, (DWORD)strlen(first));
    CloseHandle(pipe);
    pipe = server(vm.name);
    check(pipe != INVALID_HANDLE_VALUE, "recreate pipe");
    check(ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED,
          "reconnect pipe");
    WriteFile(pipe, second, (DWORD)strlen(second), &written, NULL);
    wait_for_size(path, (DWORD)(strlen(first) + strlen(second)));
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    check(file != INVALID_HANDLE_VALUE, "open raw log");
    check(ReadFile(file, raw, sizeof(raw), &got, NULL), "read raw log");
    check(got == strlen(first) + strlen(second) &&
          memcmp(raw, first, strlen(first)) == 0 &&
          memcmp(raw + strlen(first), second, strlen(second)) == 0,
          "raw bytes preserved across reconnect");
    CloseHandle(file);
    linux_tty_stop(&moved); /* ReadFile is pending. */
    linux_tty_stop(&moved);
    check(moved.linux_tty_capture == NULL, "double stop");
    CloseHandle(pipe);

    check(GetProcessHandleCount(GetCurrentProcess(), &handles_before), "handle count");
    for (int i = 0; i < 32; i++) {
        thread = CreateThread(NULL, 0, race_start, &vm, 0, NULL);
        check(thread != NULL, "start race thread");
        linux_tty_stop(&vm);
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        linux_tty_stop(&vm);
    }
    check(vm.linux_tty_capture == NULL, "start stop race cleanup");
    check(GetProcessHandleCount(GetCurrentProcess(), &handles_after) &&
          handles_after <= handles_before + 2, "no handle leak after races");
    linux_tty_start(&vm);
    thread = CreateThread(NULL, 0, race_stop, &vm, 0, NULL);
    thread2 = CreateThread(NULL, 0, race_stop, &vm, 0, NULL);
    check(thread && thread2, "two concurrent stop threads");
    WaitForSingleObject(thread, INFINITE);
    WaitForSingleObject(thread2, INFINITE);
    CloseHandle(thread);
    CloseHandle(thread2);
    check(vm.linux_tty_capture == NULL, "concurrent stops detach once");
    DeleteFileW(path);
    RemoveDirectoryW(vm_dir);
    RemoveDirectoryW(dir);
    puts("linux_tty_test: PASS");
    return 0;
}
