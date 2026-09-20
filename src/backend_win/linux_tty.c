#include "linux_tty.h"

#include "ui.h"

#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* HCS creates the server end of this pipe when a Linux VM has a COM1
   connection in its configuration.  The client deliberately starts before
   the server: CreateFileW is retried until the guest/HCS side appears. */
#define LINUX_TTY_PIPE_PREFIX L"\\\\.\\pipe\\"
#define LINUX_TTY_RETRY_MS    250

typedef struct LinuxTtyCapture {
    HANDLE stop_event;
    HANDLE thread;
    HANDLE tty_file;
    volatile HANDLE pipe;

    wchar_t vm_name[256];
    wchar_t pipe_name[512];
    wchar_t tty_path[MAX_PATH];

    /* Owned by the reader thread.  It contains bytes for the current logical
       line, not a UTF-8 string; line boundaries must be found before decoding. */
    unsigned char *line;
    size_t line_len;
    size_t line_cap;
    BOOL pending_cr;
} LinuxTtyCapture;

static SRWLOCK g_linux_tty_lock = SRWLOCK_INIT;

static BOOL tty_is_stopping(LinuxTtyCapture *capture)
{
    return WaitForSingleObject(capture->stop_event, 0) == WAIT_OBJECT_0;
}

static void tty_log_error(LinuxTtyCapture *capture, const wchar_t *what,
                          DWORD error)
{
    ui_log(L"[TTY:%s] %s failed (error=%lu). Capture will continue where possible.",
           capture->vm_name, what, (unsigned long)error);
}

/* Return the VM-owned directory from either disk.vhdx or a snapshot/branch
   VHDX.  This mirrors asb_core.c's get_vm_disk_root without coupling the
   capture module to its private helper. */
static BOOL tty_get_vm_disk_root(const wchar_t *disk_path, wchar_t *out)
{
    wchar_t *slash;
    BOOL branch;
    size_t len;

    if (!disk_path || !disk_path[0] || wcslen(disk_path) >= MAX_PATH)
        return FALSE;

    wcscpy_s(out, MAX_PATH, disk_path);
    slash = wcsrchr(out, L'\\');
    if (!slash) return FALSE;

    branch = (_wcsnicmp(slash + 1, L"branch_", 7) == 0 ||
              _wcsnicmp(slash + 1, L"snapshot_", 9) == 0);
    *slash = L'\0';
    len = wcslen(out);
    if (branch && len >= 10 && _wcsicmp(out + len - 10, L"\\snapshots") == 0)
        out[len - 10] = L'\0';

    /* Never write a diagnostic file into a drive root if a malformed path was
       persisted. */
    return wcslen(out) > 3;
}

static BOOL tty_append_byte(LinuxTtyCapture *capture, unsigned char byte)
{
    unsigned char *new_line;
    size_t new_cap;

    if (capture->line_len < capture->line_cap) {
        capture->line[capture->line_len++] = byte;
        return TRUE;
    }

    new_cap = capture->line_cap ? capture->line_cap * 2 : 256;
    if (new_cap < capture->line_cap || new_cap > SIZE_MAX / 2)
        return FALSE;
    new_line = (unsigned char *)realloc(capture->line, new_cap);
    if (!new_line) return FALSE;

    capture->line = new_line;
    capture->line_cap = new_cap;
    capture->line[capture->line_len++] = byte;
    return TRUE;
}

/* Convert one complete raw line for the existing wide host log API.  Valid
   UTF-8 is rendered normally.  If the line is not valid UTF-8, render every
   byte as ASCII (printable bytes stay readable and high/control bytes become
   \xNN), so malformed guest output can never make the reader fail. */
static wchar_t *tty_line_for_log(const unsigned char *line, size_t line_len)
{
    int utf8_len;
    int wide_len;
    wchar_t *out;
    size_t i;
    size_t out_len = 0;

    if (line_len > INT_MAX) return NULL;
    utf8_len = (int)line_len;
    wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   (LPCCH)line, utf8_len, NULL, 0);

    if (wide_len > 0) {
        wchar_t *decoded = (wchar_t *)malloc((size_t)wide_len * sizeof(wchar_t));
        if (!decoded) return NULL;
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                (LPCCH)line, utf8_len,
                                decoded, wide_len) != wide_len) {
            free(decoded);
            decoded = NULL;
            wide_len = 0;
        }

        if (decoded) {
            /* A decoded NUL would truncate ui_log's %s argument.  Reserve
               four characters for its visible escaped form. */
            for (i = 0; i < (size_t)wide_len; i++)
                out_len += decoded[i] == L'\0' ? 4 : 1;

            out = (wchar_t *)malloc((out_len + 1) * sizeof(wchar_t));
            if (!out) {
                free(decoded);
                return NULL;
            }
            out_len = 0;
            for (i = 0; i < (size_t)wide_len; i++) {
                if (decoded[i] == L'\0') {
                    out[out_len++] = L'\\';
                    out[out_len++] = L'x';
                    out[out_len++] = L'0';
                    out[out_len++] = L'0';
                } else {
                    out[out_len++] = decoded[i];
                }
            }
            out[out_len] = L'\0';
            free(decoded);
            return out;
        }
    }

    /* Invalid UTF-8: use an ASCII escaped representation.  This also avoids
       passing embedded NUL bytes to the wide variadic logger. */
    if (line_len > (SIZE_MAX - 1) / 4) return NULL;
    out = (wchar_t *)malloc((line_len * 4 + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    for (i = 0; i < line_len; i++) {
        unsigned char byte = line[i];
        if (byte >= 0x20 && byte <= 0x7e && byte != '\\') {
            out[out_len++] = (wchar_t)byte;
        } else {
            static const wchar_t hex[] = L"0123456789ABCDEF";
            out[out_len++] = L'\\';
            out[out_len++] = L'x';
            out[out_len++] = hex[(byte >> 4) & 0xf];
            out[out_len++] = hex[byte & 0xf];
        }
    }
    out[out_len] = L'\0';
    return out;
}

static void tty_emit_line(LinuxTtyCapture *capture)
{
    wchar_t *line = tty_line_for_log(capture->line, capture->line_len);
    if (line) {
        ui_log(L"[TTY:%s] %s", capture->vm_name, line);
        free(line);
    } else {
        /* The raw stream remains intact even if diagnostic rendering runs out
           of memory.  Keep the log event line-oriented and non-fatal. */
        ui_log(L"[TTY:%s] <line could not be rendered>", capture->vm_name);
    }
    capture->line_len = 0;
}

static void tty_process_bytes(LinuxTtyCapture *capture,
                              const unsigned char *bytes, DWORD count)
{
    DWORD i;
    for (i = 0; i < count; i++) {
        unsigned char byte = bytes[i];

        if (capture->pending_cr) {
            capture->pending_cr = FALSE;
            if (byte == '\n') {
                /* CRLF is one line terminator. */
                tty_emit_line(capture);
                continue;
            }
            /* A bare CR ended the previous line.  Re-process this byte as
               the first byte of the next line. */
            tty_emit_line(capture);
        }

        if (byte == '\r') {
            capture->pending_cr = TRUE;
        } else if (byte == '\n') {
            tty_emit_line(capture);
        } else if (!tty_append_byte(capture, byte)) {
            /* Do not let an unbounded guest line take down the reader.  The
               raw file is already complete; discard only the host rendering
               of this line until its next terminator. */
            capture->line_len = 0;
            capture->line_cap = 0;
            free(capture->line);
            capture->line = NULL;
            ui_log(L"[TTY:%s] line buffer allocation failed; raw capture continues.",
                   capture->vm_name);
        }
    }
}

static BOOL tty_write_raw(LinuxTtyCapture *capture,
                          const unsigned char *bytes, DWORD count)
{
    DWORD offset = 0;

    if (capture->tty_file == INVALID_HANDLE_VALUE) return FALSE;
    while (offset < count) {
        DWORD written = 0;
        if (!WriteFile(capture->tty_file, bytes + offset, count - offset,
                       &written, NULL) || written == 0) {
            tty_log_error(capture, L"WriteFile(tty.log)", GetLastError());
            CloseHandle(capture->tty_file);
            capture->tty_file = INVALID_HANDLE_VALUE;
            return FALSE;
        }
        offset += written;
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

    while (!tty_is_stopping(capture)) {
        HANDLE pipe = tty_connect(capture);
        DWORD count = 0;

        if (pipe == INVALID_HANDLE_VALUE) break;
        InterlockedExchangePointer((PVOID volatile *)&capture->pipe, pipe);

        while (!tty_is_stopping(capture)) {
            if (!tty_read_once(capture, pipe, buffer, sizeof(buffer), &count))
                break;
            if (count == 0) break;

            /* Raw bytes are written exactly as received; host log rendering
               is a separate best-effort path. */
            (void)tty_write_raw(capture, buffer, count);
            tty_process_bytes(capture, buffer, count);
        }

        InterlockedExchangePointer((PVOID volatile *)&capture->pipe, NULL);
        CloseHandle(pipe);
    }

    if (capture->pending_cr)
        tty_emit_line(capture);
    free(capture->line);
    capture->line = NULL;
    if (capture->tty_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(capture->tty_file);
        CloseHandle(capture->tty_file);
        capture->tty_file = INVALID_HANDLE_VALUE;
    }
    return 0;
}

static BOOL tty_prepare_paths(LinuxTtyCapture *capture,
                              const VmInstance *instance)
{
    wchar_t root[MAX_PATH];

    if (!tty_get_vm_disk_root(instance->vhdx_path, root))
        return FALSE;
    if (swprintf_s(capture->pipe_name, ARRAYSIZE(capture->pipe_name),
                   L"%s%s.com1", LINUX_TTY_PIPE_PREFIX, instance->name) < 0)
        return FALSE;
    if (swprintf_s(capture->tty_path, ARRAYSIZE(capture->tty_path),
                   L"%s\\tty.log", root) < 0)
        return FALSE;
    return TRUE;
}

void linux_tty_start(VmInstance *instance)
{
    LinuxTtyCapture *capture;
    HANDLE thread;

    if (!instance || _wcsicmp(instance->os_type, L"Linux") != 0)
        return;

    AcquireSRWLockExclusive(&g_linux_tty_lock);
    if (instance->linux_tty_capture) {
        ReleaseSRWLockExclusive(&g_linux_tty_lock);
        return;
    }

    capture = (LinuxTtyCapture *)calloc(1, sizeof(*capture));
    if (!capture) {
        ui_log(L"[TTY:%s] capture allocation failed; VM continues without TTY capture.",
               instance->name);
        ReleaseSRWLockExclusive(&g_linux_tty_lock);
        return;
    }
    capture->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    capture->tty_file = INVALID_HANDLE_VALUE;
    wcscpy_s(capture->vm_name, ARRAYSIZE(capture->vm_name), instance->name);

    if (!capture->stop_event || !tty_prepare_paths(capture, instance)) {
        DWORD error = GetLastError();
        if (capture->stop_event) CloseHandle(capture->stop_event);
        free(capture);
        ui_log(L"[TTY:%s] capture setup failed (error=%lu); VM continues without TTY capture.",
               instance->name, (unsigned long)error);
        ReleaseSRWLockExclusive(&g_linux_tty_lock);
        return;
    }

    /* CREATE_ALWAYS is intentional: every VM boot gets a fresh raw stream. */
    capture->tty_file = CreateFileW(capture->tty_path, GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    NULL);
    if (capture->tty_file == INVALID_HANDLE_VALUE)
        tty_log_error(capture, L"CreateFile(tty.log)", GetLastError());

    thread = CreateThread(NULL, 0, tty_thread_proc, capture, 0, NULL);
    if (!thread) {
        DWORD error = GetLastError();
        if (capture->tty_file != INVALID_HANDLE_VALUE)
            CloseHandle(capture->tty_file);
        CloseHandle(capture->stop_event);
        free(capture);
        ui_log(L"[TTY:%s] reader thread creation failed (error=%lu); VM continues without TTY capture.",
               instance->name, (unsigned long)error);
        ReleaseSRWLockExclusive(&g_linux_tty_lock);
        return;
    }

    capture->thread = thread;
    instance->linux_tty_capture = capture;
    ReleaseSRWLockExclusive(&g_linux_tty_lock);
}

void linux_tty_stop(VmInstance *instance)
{
    LinuxTtyCapture *capture;
    HANDLE pipe;

    if (!instance) return;

    AcquireSRWLockExclusive(&g_linux_tty_lock);
    capture = (LinuxTtyCapture *)instance->linux_tty_capture;
    instance->linux_tty_capture = NULL;
    if (!capture) {
        ReleaseSRWLockExclusive(&g_linux_tty_lock);
        return;
    }

    SetEvent(capture->stop_event);
    pipe = (HANDLE)InterlockedExchangePointer(
        (PVOID volatile *)&capture->pipe, NULL);
    if (pipe != NULL && pipe != INVALID_HANDLE_VALUE)
        CancelIoEx(pipe, NULL);

    /* The reader owns pipe closing and final file flushing.  CancelIoEx above
       wakes any pending overlapped ReadFile before this join. */
    WaitForSingleObject(capture->thread, INFINITE);
    CloseHandle(capture->thread);
    CloseHandle(capture->stop_event);
    free(capture);
    ReleaseSRWLockExclusive(&g_linux_tty_lock);
}
