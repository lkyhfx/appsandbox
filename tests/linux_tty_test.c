/* Build with: cl /nologo /utf-8 /I src/backend_win /I src/app_win
   tests/linux_tty_test.c /Fe:tests/linux_tty_test.exe */
#define ASB_BUILDING_DLL
#include "../src/backend_win/linux_tty.c"
#include <stdarg.h>
#include <string.h>

static SRWLOCK log_lock = SRWLOCK_INIT;
static wchar_t logs[64][1024];
static int log_count;

void ui_log(const wchar_t *fmt, ...)
{
    wchar_t line[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(line, ARRAYSIZE(line), fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&log_lock);
    if (log_count < ARRAYSIZE(logs))
        wcscpy_s(logs[log_count++], ARRAYSIZE(logs[0]), line);
    ReleaseSRWLockExclusive(&log_lock);
}

static void check(BOOL value, const char *reason)
{
    if (!value) { fprintf(stderr, "FAIL: %s\n", reason); exit(1); }
}

static BOOL logged(const wchar_t *needle)
{
    int i;
    BOOL found = FALSE;
    AcquireSRWLockShared(&log_lock);
    for (i = 0; i < log_count; i++)
        if (wcsstr(logs[i], needle)) { found = TRUE; break; }
    ReleaseSRWLockShared(&log_lock);
    return found;
}

static void clear_logs(void)
{
    AcquireSRWLockExclusive(&log_lock);
    log_count = 0;
    ReleaseSRWLockExclusive(&log_lock);
}

static HANDLE server(const wchar_t *name)
{
    wchar_t path[512];
    swprintf_s(path, ARRAYSIZE(path), L"%s%s.com1", LINUX_TTY_PIPE_PREFIX, name);
    return CreateNamedPipeW(path, PIPE_ACCESS_OUTBOUND,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            1, 16384, 16384, 0, NULL);
}

static void wait_for_log(const wchar_t *needle)
{
    int i;
    for (i = 0; i < 100 && !logged(needle); i++) Sleep(20);
    check(logged(needle), "expected log line");
}

static DWORD WINAPI race_start(LPVOID p)
{
    linux_tty_start((VmInstance *)p);
    return 0;
}

int main(void)
{
    LinuxTtyCapture parser = { 0 };
    static VmInstance vm, moved;
    HANDLE pipe, thread, file;
    DWORD written, got, handles_before, handles_after;
    char raw[256];
    const unsigned char invalid[] = { 'x', 0xff, '\n' };
    const char *first = "hello world\nx\xff\n";
    const char *second = "again\r\n";
    wchar_t cwd[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];

    wcscpy_s(parser.vm_name, ARRAYSIZE(parser.vm_name), L"unit");
    tty_process_bytes(&parser, (const unsigned char *)"hello ", 6);
    check(log_count == 0, "partial line must wait");
    tty_process_bytes(&parser, (const unsigned char *)"world\n", 6);
    check(log_count == 1 && logged(L"hello world"), "partial line joined");
    clear_logs();
    tty_process_bytes(&parser, (const unsigned char *)"a\rb\nc\r\nd\n", 9);
    check(log_count == 4 && logged(L"a") && logged(L"d"), "CR LF CRLF lines");
    clear_logs();
    tty_process_bytes(&parser, invalid, sizeof(invalid));
    check(logged(L"x\\xFF"), "invalid UTF-8 rendered safely");
    clear_logs();
    parser.line_len = LINUX_TTY_RENDER_LIMIT;
    parser.line_cap = LINUX_TTY_RENDER_LIMIT;
    parser.line = (unsigned char *)malloc(parser.line_cap);
    check(parser.line != NULL, "line allocation");
    tty_process_bytes(&parser, (const unsigned char *)"z\nnext\n", 7);
    check(logged(L"line exceeded render limit") && logged(L"next"),
          "render overflow discards only one line");
    free(parser.line);

    GetCurrentDirectoryW(ARRAYSIZE(cwd), cwd);
    swprintf_s(dir, ARRAYSIZE(dir), L"%s\\tests\\tty_test_tmp", cwd);
    CreateDirectoryW(dir, NULL);
    swprintf_s(vm.vhdx_path, ARRAYSIZE(vm.vhdx_path), L"%s\\disk.vhdx", dir);
    swprintf_s(path, ARRAYSIZE(path), L"%s\\tty.log", dir);
    swprintf_s(vm.name, ARRAYSIZE(vm.name), L"tty_test_%lu", GetCurrentProcessId());
    wcscpy_s(vm.os_type, ARRAYSIZE(vm.os_type), L"Linux");

    linux_tty_start(&vm);
    check(vm.linux_tty_capture != NULL, "capture starts before pipe exists");
    linux_tty_transfer(&vm, &moved);
    check(!vm.linux_tty_capture && moved.linux_tty_capture,
          "single capture owner after transfer");
    pipe = server(vm.name);
    check(pipe != INVALID_HANDLE_VALUE, "create pipe");
    check(ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED,
          "connect pipe");
    wait_for_log(L"COM1 connected");
    WriteFile(pipe, first, (DWORD)strlen(first), &written, NULL);
    wait_for_log(L"hello world");
    wait_for_log(L"x\\xFF");
    CloseHandle(pipe);
    wait_for_log(L"COM1 disconnected; reconnecting");
    pipe = server(vm.name);
    check(pipe != INVALID_HANDLE_VALUE, "recreate pipe");
    check(ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED,
          "reconnect pipe");
    WriteFile(pipe, second, (DWORD)strlen(second), &written, NULL);
    wait_for_log(L"again");
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

    clear_logs();
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
    DeleteFileW(path);
    RemoveDirectoryW(dir);
    puts("linux_tty_test: PASS");
    return 0;
}
