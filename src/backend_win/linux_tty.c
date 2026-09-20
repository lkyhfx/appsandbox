#include "linux_tty.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* HCS creates the server end of this pipe when a Linux VM has a COM1
   connection in its configuration.  The client deliberately starts before
   the server: CreateFileW is retried until the guest/HCS side appears. */
#define LINUX_TTY_PIPE_PREFIX L"\\\\.\\pipe\\"
#define LINUX_TTY_RETRY_MS    250
#define LINUX_TTY_STOP_WAIT_MS 5000

typedef struct LinuxTtyCapture {
    HANDLE stop_event;
    HANDLE ready_event;
    HANDLE thread;
    HANDLE tty_file;
    volatile HANDLE pipe;
    volatile LONG references; /* VM owner, reader thread, start waiter */

    wchar_t vm_name[256];
    wchar_t pipe_name[512];
    wchar_t tty_path[MAX_PATH];

    uint64_t raw_offset;
} LinuxTtyCapture;

static SRWLOCK g_linux_tty_lock = SRWLOCK_INIT;

static void tty_release(LinuxTtyCapture *capture)
{
    if (InterlockedDecrement(&capture->references) == 0) {
        CloseHandle(capture->ready_event);
        CloseHandle(capture->stop_event);
        free(capture);
    }
}

static BOOL tty_is_stopping(LinuxTtyCapture *capture)
{
    return WaitForSingleObject(capture->stop_event, 0) == WAIT_OBJECT_0;
}

static void tty_log_error(LinuxTtyCapture *capture, const wchar_t *what,
                          DWORD error)
{
    wchar_t message[512];
    swprintf_s(message, ARRAYSIZE(message),
               L"[TTY:%s] %s failed (error=%lu).\n",
               capture->vm_name, what, (unsigned long)error);
    OutputDebugStringW(message);
}

/* The COM1 stream is kept as raw bytes only.  It is never forwarded to the
   application log callback or decoded for the main window. */

static BOOL tty_write_raw(LinuxTtyCapture *capture,
                          const unsigned char *bytes, DWORD count)
{
    DWORD offset = 0;

    if (capture->tty_file == INVALID_HANDLE_VALUE) return FALSE;
    while (offset < count) {
        DWORD written = 0;
        OVERLAPPED ov = { 0 };
        HANDLE waits[2];
        BOOL ok;
        DWORD error = ERROR_SUCCESS;
        ov.Offset = (DWORD)capture->raw_offset;
        ov.OffsetHigh = (DWORD)(capture->raw_offset >> 32);
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!ov.hEvent) error = GetLastError();
        if (error == ERROR_SUCCESS) {
            ok = WriteFile(capture->tty_file, bytes + offset, count - offset,
                           &written, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                waits[0] = capture->stop_event;
                waits[1] = ov.hEvent;
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0)
                    CancelIoEx(capture->tty_file, &ov);
                ok = GetOverlappedResult(capture->tty_file, &ov, &written, TRUE);
            }
            if (!ok) error = GetLastError();
            CloseHandle(ov.hEvent);
        }
        if (error != ERROR_SUCCESS || written == 0) {
            if (!tty_is_stopping(capture))
                tty_log_error(capture, L"WriteFile(tty.log)", error);
            CloseHandle(capture->tty_file);
            capture->tty_file = INVALID_HANDLE_VALUE;
            return FALSE;
        }
        offset += written;
        capture->raw_offset += written;
    }
    return TRUE;
}

static HANDLE tty_connect(LinuxTtyCapture *capture)
{
    for (;;) {
        HANDLE pipe;
        DWORD error;

        if (tty_is_stopping(capture)) return INVALID_HANDLE_VALUE;

        pipe = CreateFileW(capture->pipe_name, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        SetEvent(capture->ready_event); /* one connection attempt before HCS Start */
        if (pipe != INVALID_HANDLE_VALUE)
            return pipe;

        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND &&
            error != ERROR_PATH_NOT_FOUND &&
            error != ERROR_PIPE_BUSY &&
            error != ERROR_ACCESS_DENIED) {
            tty_log_error(capture, L"CreateFile(COM1 pipe)", error);
        }

        /* A short wait makes pipe creation retryable while keeping stop
           latency bounded even when HCS has not created COM1 yet. */
        if (WaitForSingleObject(capture->stop_event, LINUX_TTY_RETRY_MS) ==
            WAIT_OBJECT_0)
            return INVALID_HANDLE_VALUE;
    }
}

static BOOL tty_read_once(LinuxTtyCapture *capture, HANDLE pipe,
                          unsigned char *buffer, DWORD buffer_size,
                          DWORD *bytes_read)
{
    OVERLAPPED overlapped;
    HANDLE waits[2];
    BOOL ok;
    DWORD wait_result;

    ZeroMemory(&overlapped, sizeof(overlapped));
    overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent) {
        tty_log_error(capture, L"CreateEvent(ReadFile)", GetLastError());
        return FALSE;
    }

    ok = ReadFile(pipe, buffer, buffer_size, bytes_read, &overlapped);
    if (!ok) {
        DWORD error = GetLastError();
        if (error == ERROR_MORE_DATA && *bytes_read > 0) {
            CloseHandle(overlapped.hEvent);
            return TRUE;
        }
        if (error == ERROR_IO_PENDING)
            goto read_pending;
        CloseHandle(overlapped.hEvent);
        if (error != ERROR_BROKEN_PIPE && error != ERROR_NO_DATA &&
            error != ERROR_OPERATION_ABORTED)
            tty_log_error(capture, L"ReadFile(COM1 pipe)", error);
        return FALSE;
    }

read_pending:
    waits[0] = capture->stop_event;
    waits[1] = overlapped.hEvent;
    wait_result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (wait_result == WAIT_OBJECT_0) {
        /* CancelIoEx is safe from this reader thread and is also issued by
           linux_tty_stop, so shutdown never depends on a guest write arriving. */
        CancelIoEx(pipe, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        *bytes_read = 0;
        CloseHandle(overlapped.hEvent);
        return FALSE;
    }

    if (wait_result != WAIT_OBJECT_0 + 1 ||
        !GetOverlappedResult(pipe, &overlapped, bytes_read, FALSE)) {
        DWORD error = GetLastError();
        CloseHandle(overlapped.hEvent);
        if (error == ERROR_MORE_DATA && *bytes_read > 0)
            return TRUE;
        if (!tty_is_stopping(capture) &&
            error != ERROR_BROKEN_PIPE && error != ERROR_NO_DATA &&
            error != ERROR_OPERATION_ABORTED)
            tty_log_error(capture, L"GetOverlappedResult(COM1 pipe)", error);
        *bytes_read = 0;
        return FALSE;
    }

    CloseHandle(overlapped.hEvent);
    return TRUE;
}

static DWORD WINAPI tty_thread_proc(LPVOID parameter)
{
    LinuxTtyCapture *capture = (LinuxTtyCapture *)parameter;
    unsigned char buffer[16 * 1024];

    /* Opening the diagnostic log never holds the global capture lock. */
    capture->tty_file = CreateFileW(capture->tty_path, GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                    NULL, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                    NULL);
    if (capture->tty_file == INVALID_HANDLE_VALUE && !tty_is_stopping(capture))
        tty_log_error(capture, L"CreateFile(tty.log)", GetLastError());

    while (!tty_is_stopping(capture)) {
        HANDLE pipe = tty_connect(capture);
        DWORD count = 0;

        if (pipe == INVALID_HANDLE_VALUE) break;
        InterlockedExchangePointer((PVOID volatile *)&capture->pipe, pipe);
        while (!tty_is_stopping(capture)) {
            if (!tty_read_once(capture, pipe, buffer, sizeof(buffer), &count))
                break;
            if (count == 0) break;

            /* Preserve the guest's raw bytes without routing them to ui_log. */
            (void)tty_write_raw(capture, buffer, count);
        }

        InterlockedExchangePointer((PVOID volatile *)&capture->pipe, NULL);
        CloseHandle(pipe);
    }

    if (capture->tty_file != INVALID_HANDLE_VALUE) {
        CloseHandle(capture->tty_file);
        capture->tty_file = INVALID_HANDLE_VALUE;
    }
    tty_release(capture);
    return 0;
}

static BOOL tty_prepare_paths(LinuxTtyCapture *capture,
                              const VmInstance *instance)
{
    wchar_t exe_path[MAX_PATH], logs_dir[MAX_PATH], vm_dir[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, exe_path, ARRAYSIZE(exe_path));
    wchar_t *slash;
    size_t name_len;

    if (!length || length >= ARRAYSIZE(exe_path)) return FALSE;
    name_len = wcslen(instance->name);
    if (!name_len || wcscmp(instance->name, L".") == 0 ||
        wcscmp(instance->name, L"..") == 0 ||
        instance->name[name_len - 1] == L'.' ||
        instance->name[name_len - 1] == L' ') {
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }
    for (size_t i = 0; i < name_len; i++) {
        if (instance->name[i] < 32 || wcschr(L"\\/:*?\"<>|", instance->name[i])) {
            SetLastError(ERROR_INVALID_NAME);
            return FALSE;
        }
    }
    slash = wcsrchr(exe_path, L'\\');
    if (!slash) return FALSE;
    *slash = L'\0';
    if (swprintf_s(capture->pipe_name, ARRAYSIZE(capture->pipe_name),
                   L"%s%s.com1", LINUX_TTY_PIPE_PREFIX, instance->name) < 0)
        return FALSE;
    if (swprintf_s(logs_dir, ARRAYSIZE(logs_dir),
                   L"%s\\tty-logs", exe_path) < 0 ||
        swprintf_s(vm_dir, ARRAYSIZE(vm_dir),
                   L"%s\\%s", logs_dir, instance->name) < 0)
        return FALSE;
    if (!CreateDirectoryW(logs_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return FALSE;
    if (!CreateDirectoryW(vm_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return FALSE;
    if (swprintf_s(capture->tty_path, ARRAYSIZE(capture->tty_path),
                   L"%s\\tty.log", vm_dir) < 0)
        return FALSE;
    return TRUE;
}

void linux_tty_start(VmInstance *instance)
{
    LinuxTtyCapture *capture;
    HANDLE thread;
    HANDLE waits[2];

    if (!instance || _wcsicmp(instance->os_type, L"Linux") != 0)
        return;

    AcquireSRWLockExclusive(&g_linux_tty_lock);
    if (instance->linux_tty_capture) {
        ReleaseSRWLockExclusive(&g_linux_tty_lock);
        return;
    }

    capture = (LinuxTtyCapture *)calloc(1, sizeof(*capture));
    if (!capture) {
        ReleaseSRWLockExclusive(&g_linux_tty_lock);
        return;
    }
    capture->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    capture->ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    capture->references = 3;
    capture->tty_file = INVALID_HANDLE_VALUE;
    wcscpy_s(capture->vm_name, ARRAYSIZE(capture->vm_name), instance->name);

    if (!capture->stop_event || !capture->ready_event ||
        !tty_prepare_paths(capture, instance)) {
        DWORD error = GetLastError();
        tty_log_error(capture, L"capture setup", error);
        if (capture->stop_event) CloseHandle(capture->stop_event);
        if (capture->ready_event) CloseHandle(capture->ready_event);
        free(capture);
        ReleaseSRWLockExclusive(&g_linux_tty_lock);
        return;
    }

    thread = CreateThread(NULL, 0, tty_thread_proc, capture, CREATE_SUSPENDED, NULL);
    if (!thread) {
        DWORD error = GetLastError();
        tty_log_error(capture, L"reader thread creation", error);
        if (capture->tty_file != INVALID_HANDLE_VALUE)
            CloseHandle(capture->tty_file);
        CloseHandle(capture->ready_event);
        CloseHandle(capture->stop_event);
        free(capture);
        ReleaseSRWLockExclusive(&g_linux_tty_lock);
        return;
    }

    capture->thread = thread;
    instance->linux_tty_capture = capture;
    if (ResumeThread(thread) == (DWORD)-1) {
        DWORD error = GetLastError();
        tty_log_error(capture, L"reader thread start", error);
        instance->linux_tty_capture = NULL;
        TerminateThread(thread, 1); /* suspended thread has not run */
        CloseHandle(thread);
        CloseHandle(capture->ready_event);
        CloseHandle(capture->stop_event);
        free(capture);
        ReleaseSRWLockExclusive(&g_linux_tty_lock);
        return;
    }
    ReleaseSRWLockExclusive(&g_linux_tty_lock);
    waits[0] = capture->ready_event;
    waits[1] = capture->stop_event;
    WaitForMultipleObjects(2, waits, FALSE, 1000);
    tty_release(capture);
}

void linux_tty_transfer(VmInstance *from, VmInstance *to)
{
    if (!from || !to || from == to) return;
    AcquireSRWLockExclusive(&g_linux_tty_lock);
    if (!to->linux_tty_capture) {
        to->linux_tty_capture = from->linux_tty_capture;
        from->linux_tty_capture = NULL;
    }
    ReleaseSRWLockExclusive(&g_linux_tty_lock);
}

void linux_tty_stop(VmInstance *instance)
{
    LinuxTtyCapture *capture;
    DWORD wait_result;

    if (!instance) return;

    AcquireSRWLockExclusive(&g_linux_tty_lock);
    capture = (LinuxTtyCapture *)instance->linux_tty_capture;
    instance->linux_tty_capture = NULL;
    if (!capture) {
        ReleaseSRWLockExclusive(&g_linux_tty_lock);
        return;
    }

    SetEvent(capture->stop_event);
    ReleaseSRWLockExclusive(&g_linux_tty_lock);

    /* Reader owns the pipe handle. It sees stop_event and cancels pending I/O.
       A stalled filesystem cannot hold other VMs or shutdown indefinitely. */
    wait_result = WaitForSingleObject(capture->thread, LINUX_TTY_STOP_WAIT_MS);
    CloseHandle(capture->thread);
    if (wait_result != WAIT_OBJECT_0)
        tty_log_error(capture, L"reader thread stop", wait_result);
    tty_release(capture);
}
