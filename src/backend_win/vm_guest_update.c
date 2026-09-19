/*
 * Host-side Linux Guest Runtime update state machine.
 *
 * vm_agent.c remains the control plane. This module owns bundle hashing,
 * binary VSOCK transport, progress, reconnect/reboot handling, and the host
 * mirror of the guest transaction state. The guest updater is the authority
 * for signature verification, extraction, activation, health, and rollback.
 */

#include <winsock2.h>
#include <bcrypt.h>
#include "vm_guest_update.h"
#include "vm_agent.h"
#include "asb_core.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

#define UPDATE_SERVICE_PORT 9
#define UPDATE_PROTOCOL     1
#define UPDATE_MAX_BUNDLE   (512ULL * 1024ULL * 1024ULL)
#define UPDATE_MAX_WAIT_MS  (180000UL)
#define PREFLIGHT_TIMEOUT_MS (30000UL)

/* Release builds inject these digests into the Host binary. Empty values are
 * deliberately fail-closed: a paired sidecar is useful for packaging checks,
 * but is not itself a trust anchor if an entire resource directory is
 * replaced. */
#ifndef ASB_BOOTSTRAP_UPDATER_SHA256
#define ASB_BOOTSTRAP_UPDATER_SHA256 ""
#endif
#ifndef ASB_BUNDLE_VERIFIER_SHA256
#define ASB_BUNDLE_VERIFIER_SHA256 ""
#endif
#ifndef ASB_RELEASE_PUBLIC_KEY_HEX
#define ASB_RELEASE_PUBLIC_KEY_HEX ""
#endif

/* Volatile storage keeps the exact release bindings visible in the Host PE;
 * the release script checks these bytes in addition to the generated JSON. */
static const volatile char g_embedded_release_public_key[] = ASB_RELEASE_PUBLIC_KEY_HEX;
static const volatile char g_embedded_bootstrap_updater_sha256[] = ASB_BOOTSTRAP_UPDATER_SHA256;
static const volatile char g_embedded_bundle_verifier_sha256[] = ASB_BUNDLE_VERIFIER_SHA256;

#define AF_HYPERV 34
#define HV_PROTOCOL_RAW 1
typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

#pragma pack(push, 1)
typedef struct UpdateStreamHeader {
    char magic[8];
    DWORD protocol;
    DWORD header_size;
    char txid[40];
    ULONGLONG bundle_size;
    BYTE sha256[32];
} UpdateStreamHeader;
#pragma pack(pop)

typedef struct UpdateJob {
    UINT64 vm_id;
    HANDLE thread;
    volatile BOOL cancel;
    char txid[40];
    wchar_t bundle_path[MAX_PATH];
} UpdateJob;

#define MAX_UPDATE_JOBS 16
static UpdateJob g_jobs[MAX_UPDATE_JOBS];
static INIT_ONCE g_update_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_update_cs;

static BOOL CALLBACK update_init(PINIT_ONCE once, PVOID param, PVOID *context)
{
    (void)once; (void)param; (void)context;
    InitializeCriticalSection(&g_update_cs);
    return TRUE;
}

static void update_ensure_init(void)
{
    InitOnceExecuteOnce(&g_update_once, update_init, NULL, NULL);
}

static void update_set(UINT64 vm_id, int state, int progress, BOOL reboot,
                       const char *txid, const char *error)
{
    VmInstance *vm = asb_find_vm_by_id(vm_id);
    if (!vm) return;
    vm->update_state = state;
    vm->update_progress = progress;
    vm->update_reboot_required = reboot;
    if (txid) strncpy_s(vm->update_txid, sizeof(vm->update_txid), txid, _TRUNCATE);
    if (error) strncpy_s(vm->update_error, sizeof(vm->update_error), error, _TRUNCATE);
    else vm->update_error[0] = '\0';
    vm_agent_notify_status(vm);
}

static BOOL send_all(SOCKET s, const void *data, size_t len)
{
    const char *p = (const char *)data;
    while (len) {
        int n = send(s, p, (int)(len > INT_MAX ? INT_MAX : len), 0);
        if (n <= 0) return FALSE;
        p += n; len -= (size_t)n;
    }
    return TRUE;
}

static BOOL get_runtime_id(VmInstance *vm, GUID *out)
{
    static const GUID zero_guid = {0};
    if (!vm || !out) return FALSE;
    if (memcmp(&vm->runtime_id, &zero_guid, sizeof(GUID)) != 0) {
        *out = vm->runtime_id;
        return TRUE;
    }
    if (!hcs_find_runtime_id(vm->name, out)) return FALSE;
    vm->runtime_id = *out;
    return TRUE;
}

static SOCKET connect_update_transport(VmInstance *vm, DWORD port)
{
    SOCKADDR_HV addr;
    GUID runtime;
    SOCKET s;
    u_long nonblock = 1;
    fd_set wfds, efds;
    struct timeval tv;
    DWORD timeout = 10000;
    if (!get_runtime_id(vm, &runtime)) return INVALID_SOCKET;
    s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    ioctlsocket(s, FIONBIO, &nonblock);
    memset(&addr, 0, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = runtime;
    hcs_service_guid(vm->os_type, port, &addr.ServiceId);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s); return INVALID_SOCKET;
        }
        FD_ZERO(&wfds); FD_ZERO(&efds); FD_SET(s, &wfds); FD_SET(s, &efds);
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s); return INVALID_SOCKET;
        }
    }
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    timeout = 10000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    return s;
}

static BOOL sha256_file(const wchar_t *path, BYTE digest[32], ULONGLONG *size_out)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object = NULL;
    DWORD object_len = 0, result = 0, read = 0;
    LARGE_INTEGER size;
    BYTE buf[1024 * 1024];
    BOOL ok = FALSE;
    if (!path || !digest || !size_out) return FALSE;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        (ULONGLONG)size.QuadPart > UPDATE_MAX_BUNDLE) goto done;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0 ||
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_len,
                          sizeof(object_len), &result, 0) != 0) goto done;
    object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, object_len);
    if (!object || BCryptCreateHash(alg, &hash, object, object_len, NULL, 0, 0) != 0) goto done;
    for (;;) {
        if (!ReadFile(file, buf, sizeof(buf), &read, NULL)) goto done;
        if (!read) break;
        if (BCryptHashData(hash, buf, read, 0) != 0) goto done;
    }
    if (BCryptFinishHash(hash, digest, 32, 0) != 0) goto done;
    *size_out = (ULONGLONG)size.QuadPart;
    ok = TRUE;
done:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (object) HeapFree(GetProcessHeap(), 0, object);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

static BOOL read_file_bytes(const wchar_t *path, BYTE **data_out, ULONGLONG *size_out,
                            ULONGLONG max_size)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    BYTE *data = NULL;
    DWORD read = 0;
    BOOL ok = FALSE;
    if (!path || !data_out || !size_out) return FALSE;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        (ULONGLONG)size.QuadPart > max_size) goto done;
    data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart);
    if (!data || !ReadFile(file, data, (DWORD)size.QuadPart, &read, NULL) ||
        (ULONGLONG)read != (ULONGLONG)size.QuadPart) goto done;
    *data_out = data; *size_out = (ULONGLONG)size.QuadPart; data = NULL; ok = TRUE;
done:
    if (data) HeapFree(GetProcessHeap(), 0, data);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

/* The Host performs a bounded container check and delegates the complete
 * signed-manifest/policy check to the pinned release verifier. The Guest
 * repeats the same verification independently after transfer. */
static void hex_digest(const BYTE digest[32], char out[65]);
static BOOL get_linux_resource_path(const wchar_t *relative, wchar_t *out, size_t cap);
static BOOL verify_pinned_resource(const wchar_t *path, const char *embedded_sha);

static BOOL embedded_release_trust_valid(void)
{
    const volatile char *values[] = { g_embedded_release_public_key,
                                      g_embedded_bootstrap_updater_sha256,
                                      g_embedded_bundle_verifier_sha256 };
    size_t i, j;
    for (i = 0; i < ARRAYSIZE(values); i++) {
        size_t length = 0;
        while (length <= 64 && values[i][length] != '\0') length++;
        if (length != 64) return FALSE;
        for (j = 0; j < length; j++) {
            if (!isxdigit((unsigned char)values[i][j])) return FALSE;
        }
    }
    return TRUE;
}

static BOOL append_quoted_arg(wchar_t *command, size_t cap, const wchar_t *arg)
{
    size_t used = wcslen(command), slashes = 0;
    const wchar_t *p;
    if (used + 3 >= cap) return FALSE;
    command[used++] = L'"';
    for (p = arg; *p; p++) {
        if (*p == L'\\') {
            slashes++;
            continue;
        }
        if (*p == L'"') {
            while (slashes > 0) {
                slashes--;
                if (used + 2 >= cap) return FALSE;
                command[used++] = L'\\'; command[used++] = L'\\';
            }
            if (used + 2 >= cap) return FALSE;
            command[used++] = L'\\'; command[used++] = L'"';
        } else {
            while (slashes > 0) {
                slashes--;
                if (used + 1 >= cap) return FALSE;
                command[used++] = L'\\';
            }
            if (used + 1 >= cap) return FALSE;
            command[used++] = *p;
        }
    }
    while (slashes > 0) {
        slashes--;
        if (used + 2 >= cap) return FALSE;
        command[used++] = L'\\'; command[used++] = L'\\';
    }
    if (used + 2 >= cap) return FALSE;
    command[used++] = L'"'; command[used] = L'\0';
    return TRUE;
}

/* The helper is a release artifact, not a mutable tool found through PATH.
 * It must verify the detached Ed25519 signature and enforce the same schema,
 * arch, OS, protocol, updater-minimum, and kernel-component policy as the
 * guest updater. Its sidecar is checked before execution, so replacing the
 * verifier alone cannot weaken the Host gate. The expected bundle digest is
 * passed to bind verification to the exact bytes that will be transferred. */
static BOOL verify_manifest_with_pinned_helper(const wchar_t *bundle,
                                               const char *expected_sha)
{
    wchar_t verifier[MAX_PATH], verifier_dir[MAX_PATH], *slash;
    wchar_t command_line[MAX_PATH * 4];
    wchar_t expected_w[65];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD wait_result, exit_code = 1;
    BOOL ok = FALSE;
    if (!get_linux_resource_path(L"updater\\appsandbox-guest-bundle-verifier.exe",
                                 verifier, ARRAYSIZE(verifier)) ||
        !verify_pinned_resource(verifier, ASB_BUNDLE_VERIFIER_SHA256) ||
        !bundle || !expected_sha ||
        strlen(expected_sha) != 64) return FALSE;
    for (size_t i = 0; i < 64; i++) expected_w[i] = (wchar_t)(unsigned char)expected_sha[i];
    expected_w[64] = L'\0';
    if (wcslen(verifier) >= ARRAYSIZE(verifier_dir)) return FALSE;
    wcscpy_s(verifier_dir, ARRAYSIZE(verifier_dir), verifier);
    slash = wcsrchr(verifier_dir, L'\\');
    if (!slash) return FALSE;
    *slash = L'\0';
    command_line[0] = L'\0';
    /* CreateProcess does not synthesize argv[0] when lpApplicationName is
     * supplied. Keep the verifier flag in argv[1], as its Go CLI requires. */
    if (!append_quoted_arg(command_line, ARRAYSIZE(command_line), verifier) ||
        wcscat_s(command_line, ARRAYSIZE(command_line), L" --verify-bundle ") != 0 ||
        !append_quoted_arg(command_line, ARRAYSIZE(command_line), bundle) ||
        swprintf_s(command_line + wcslen(command_line),
                   ARRAYSIZE(command_line) - wcslen(command_line),
                   L" %s", expected_w) < 0) return FALSE;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(verifier, command_line, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                         NULL, verifier_dir, &si, &pi)) return FALSE;
    wait_result = WaitForSingleObject(pi.hProcess, PREFLIGHT_TIMEOUT_MS);
    if (wait_result == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exit_code))
        ok = exit_code == 0;
    else if (wait_result == WAIT_TIMEOUT)
        TerminateProcess(pi.hProcess, 124);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return ok;
}

static BOOL bundle_preflight(const wchar_t *path, const char *expected_sha)
{
    HANDLE file;
    DWORD read = 0;
    BYTE magic[4];
    size_t path_len;
    if (!path || !path[0]) return FALSE;
    path_len = wcslen(path);
    if (path_len < 8 || _wcsicmp(path + path_len - 8, L".tar.zst") != 0) return FALSE;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!ReadFile(file, magic, sizeof(magic), &read, NULL) || read != sizeof(magic)) {
        CloseHandle(file); return FALSE;
    }
    CloseHandle(file);
    return magic[0] == 0x28 && magic[1] == 0xB5 && magic[2] == 0x2F && magic[3] == 0xFD &&
           verify_manifest_with_pinned_helper(path, expected_sha);
}

static BOOL get_linux_resource_path(const wchar_t *relative, wchar_t *out, size_t cap)
{
    wchar_t exe[MAX_PATH], *slash;
    DWORD n;
    if (!relative || !out || !cap) return FALSE;
    n = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    if (!n || n >= ARRAYSIZE(exe)) return FALSE;
    slash = wcsrchr(exe, L'\\');
    if (!slash) return FALSE;
    *slash = L'\0';
    return swprintf_s(out, cap, L"%s\\resources\\linux\\%s", exe, relative) > 0;
}

static BOOL verify_pinned_resource(const wchar_t *path, const char *embedded_sha)
{
    wchar_t hash_path[MAX_PATH];
    BYTE digest[32], *hash_data = NULL;
    ULONGLONG hash_size = 0, file_size = 0;
    char actual[65], expected[65] = {0};
    size_t i, n = 0;
    if (!path || swprintf_s(hash_path, ARRAYSIZE(hash_path), L"%s.sha256", path) < 0 ||
        !sha256_file(path, digest, &file_size) ||
        !read_file_bytes(hash_path, &hash_data, &hash_size, 4096)) return FALSE;
    hex_digest(digest, actual);
    while (n < (size_t)hash_size && n < sizeof(expected) - 1 &&
           hash_data[n] != '\r' && hash_data[n] != '\n' && hash_data[n] != ' ' && hash_data[n] != '\t')
    {
        expected[n] = (char)hash_data[n];
        n++;
    }
    HeapFree(GetProcessHeap(), 0, hash_data);
    if (n != 64 || !embedded_sha || strlen(embedded_sha) != 64) return FALSE;
    for (i = 0; i < n; i++) {
        if (!((expected[i] >= '0' && expected[i] <= '9') ||
              (expected[i] >= 'a' && expected[i] <= 'f') ||
              (expected[i] >= 'A' && expected[i] <= 'F'))) return FALSE;
        if (tolower((unsigned char)actual[i]) != tolower((unsigned char)embedded_sha[i])) return FALSE;
    }
    return _stricmp(actual, expected) == 0;
}

static BOOL windows_path_to_wsl(const wchar_t *windows_path, wchar_t *out, size_t cap)
{
    const wchar_t *p = windows_path;
    size_t i, used;
    if (!p || !out || cap < 8) return FALSE;
    if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;
    if (!((p[0] >= L'A' && p[0] <= L'Z') ||
          (p[0] >= L'a' && p[0] <= L'z')) || p[1] != L':' ||
        (p[2] != L'\\' && p[2] != L'/')) return FALSE;
    wcscpy_s(out, cap, L"/mnt/");
    used = 5;
    out[used++] = (p[0] >= L'A' && p[0] <= L'Z') ? (wchar_t)(p[0] + (L'a' - L'A')) : p[0];
    out[used++] = L'/';
    for (i = 3; p[i]; i++) {
        if (used + 1 >= cap) return FALSE;
        out[used++] = p[i] == L'\\' ? L'/' : p[i];
    }
    out[used] = L'\0';
    return TRUE;
}

static BOOL run_wsl_args(const wchar_t *args, DWORD timeout_ms)
{
    wchar_t exe[MAX_PATH], command_line[8192];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD n, wait_result, exit_code = 1;
    if (!args) return FALSE;
    n = GetSystemDirectoryW(exe, ARRAYSIZE(exe));
    if (!n || n >= ARRAYSIZE(exe) || swprintf_s(exe + n, ARRAYSIZE(exe) - n,
                                                  L"\\wsl.exe") < 0) return FALSE;
    if (swprintf_s(command_line, ARRAYSIZE(command_line), L"wsl.exe %s", args) < 0) return FALSE;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(exe, command_line, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        ui_log(L"offline migration: could not start wsl.exe (error %lu)", GetLastError());
        return FALSE;
    }
    wait_result = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait_result == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &exit_code);
    else if (wait_result == WAIT_TIMEOUT)
        TerminateProcess(pi.hProcess, 124);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait_result == WAIT_OBJECT_0 && exit_code == 0;
}

static BOOL wait_for_vm_stopped(VmInstance *vm)
{
    DWORD start = GetTickCount();
    if (!vm) return FALSE;
    while (GetTickCount() - start < 120000UL) {
        if (!vm->running && !hcs_is_running_by_enum(vm->name)) return TRUE;
        Sleep(250);
    }
    return FALSE;
}

/* HCS callback cleanup can close the compute handle on another thread. A
 * shared open/flush does not prove vmwp has released the backing disk. */
static BOOL wait_for_vhdx_release(const wchar_t *path)
{
    DWORD start = GetTickCount(), error;
    do {
        HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            BOOL ok = FlushFileBuffers(file);
            CloseHandle(file);
            return ok;
        }
        error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) break;
        Sleep(250);
    } while (GetTickCount() - start < 120000UL);
    ui_log(L"offline migration: VHDX not exclusively available (error %lu)", error);
    return FALSE;
}

static BOOL restart_after_offline_migration(VmInstance *vm, BOOL require_updater)
{
    DWORD start;
    if (!vm || FAILED(asb_vm_restart_after_offline_update((AsbVm)vm))) {
        if (vm && vm->handle) hcs_close_vm(vm);
        return FALSE;
    }
    start = GetTickCount();
    while (GetTickCount() - start < 120000UL) {
        if (vm->agent_online && (!require_updater || vm->guest_updater_supported)) return TRUE;
        Sleep(250);
    }
    return FALSE;
}

BOOL vm_guest_update_bootstrap_legacy(VmInstance *instance)
{
    wchar_t mount_name[96], mount_root[128], script_wsl[4096], updater_wsl[4096];
    wchar_t service_wsl[4096], watch_wsl[4096], bootstrap_wsl[4096];
    wchar_t updater[MAX_PATH], script[MAX_PATH], service[MAX_PATH];
    wchar_t watch[MAX_PATH], bootstrap[MAX_PATH], args[8192];
    BOOL mounted = FALSE, migrated = FALSE, restarted = FALSE;
    BOOL stopped;
    if (!instance || _wcsicmp(instance->os_type, L"Linux") != 0 ||
        !instance->vhdx_path[0]) return FALSE;
    stopped = !instance->running;
    update_set(instance->unique_id, ASB_UPDATE_LEGACY_MIGRATION, 5, FALSE, NULL, NULL);
    /* Probe the exact default-distro/root/Python execution path before
     * stopping a working guest or attaching its disk. */
    if (!run_wsl_args(L"-u root -- python3 -c \"import sys; assert sys.version_info >= (3, 9)\"",
                      PREFLIGHT_TIMEOUT_MS)) {
        ui_log(L"Legacy migration requires a runnable default WSL distro with Python 3.9+.");
        update_set(instance->unique_id, ASB_UPDATE_FAILED, 0, FALSE, NULL,
                   "legacy_migration_requires_wsl_python3");
        return FALSE;
    }
    if (!get_linux_resource_path(L"updater\\appsandbox-guest-updater", updater, ARRAYSIZE(updater)) ||
        !verify_pinned_resource(updater, ASB_BOOTSTRAP_UPDATER_SHA256) ||
        !get_linux_resource_path(L"offline_legacy_bootstrap.py", script, ARRAYSIZE(script)) ||
        !get_linux_resource_path(L"updater\\appsandbox-guest-updater.service", service, ARRAYSIZE(service)) ||
        !get_linux_resource_path(L"updater\\appsandbox-guest-update-watch.service", watch, ARRAYSIZE(watch)) ||
        !get_linux_resource_path(L"bootstrap-runtime", bootstrap, ARRAYSIZE(bootstrap)) ||
        !windows_path_to_wsl(script, script_wsl, ARRAYSIZE(script_wsl)) ||
        !windows_path_to_wsl(updater, updater_wsl, ARRAYSIZE(updater_wsl)) ||
        !windows_path_to_wsl(service, service_wsl, ARRAYSIZE(service_wsl)) ||
        !windows_path_to_wsl(watch, watch_wsl, ARRAYSIZE(watch_wsl)) ||
        !windows_path_to_wsl(bootstrap, bootstrap_wsl, ARRAYSIZE(bootstrap_wsl))) {
        update_set(instance->unique_id, ASB_UPDATE_FAILED, 0, FALSE, NULL,
                   "legacy_migration_resources_invalid");
        return FALSE;
    }
    /* Verify the actual /mnt/<drive> resources before shutting down. This
     * also rejects default distros with incompatible automount settings. */
    wcscpy_s(args, ARRAYSIZE(args),
        L"-u root -- python3 -c \"import pathlib,sys; assert all(pathlib.Path(p).exists() for p in sys.argv[1:])\" ");
    {
        const wchar_t *paths[] = {script_wsl, updater_wsl, service_wsl, watch_wsl, bootstrap_wsl};
        size_t i;
        for (i = 0; i < ARRAYSIZE(paths); i++) {
            if (!append_quoted_arg(args, ARRAYSIZE(args), paths[i]) ||
                wcscat_s(args, ARRAYSIZE(args), L" ") != 0) return FALSE;
        }
    }
    if (!run_wsl_args(args, PREFLIGHT_TIMEOUT_MS)) {
        update_set(instance->unique_id, ASB_UPDATE_FAILED, 0, FALSE, NULL,
                   "legacy_migration_wsl_resources_unavailable");
        return FALSE;
    }
    swprintf_s(mount_name, ARRAYSIZE(mount_name), L"asb-update-%lu-%llu",
               GetCurrentProcessId(), (unsigned long long)instance->unique_id);
    swprintf_s(mount_root, ARRAYSIZE(mount_root), L"/mnt/wsl/%s", mount_name);
    if (instance->running) {
        if (FAILED(hcs_stop_vm(instance)) || !wait_for_vm_stopped(instance)) goto done;
        stopped = TRUE;
    }
    vm_agent_stop(instance);
    hcs_stop_monitor(instance);
    hcs_close_vm_sync(instance);
    if (!wait_for_vhdx_release(instance->vhdx_path)) {
        /* No WSL mount or offline write has begun. Try to bring the original
         * guest back even if another process still holds the disk; HCS will
         * reject the start if that lock has not cleared. */
        HRESULT restart_hr = asb_vm_restart_after_offline_update((AsbVm)instance);
        if (FAILED(restart_hr))
            ui_log(L"offline migration: could not restart guest after VHDX release timeout (0x%08X)",
                   restart_hr);
        update_set(instance->unique_id, ASB_UPDATE_FAILED, 0, FALSE, NULL,
                   "legacy_migration_vhdx_release_failed");
        return FALSE;
    }

    args[0] = L'\0';
    if (swprintf_s(args, ARRAYSIZE(args), L"--mount ") < 0 ||
        !append_quoted_arg(args, ARRAYSIZE(args), instance->vhdx_path) ||
        /* ubuntu_vhdx.c provisions GPT: partition 1 ESP, partition 2 root. */
        wcscat_s(args, ARRAYSIZE(args), L" --vhd --partition 2 --type ext4 --name ") != 0 ||
        !append_quoted_arg(args, ARRAYSIZE(args), mount_name)) goto done;
    /* A failed mount can still leave the VHD attached to WSL. */
    mounted = TRUE;
    if (!run_wsl_args(args, 120000UL)) goto done;
    update_set(instance->unique_id, ASB_UPDATE_LEGACY_MIGRATION, 45, FALSE, NULL, NULL);
    args[0] = L'\0';
    if (swprintf_s(args, ARRAYSIZE(args), L"-u root -- python3 ") < 0 ||
        !append_quoted_arg(args, ARRAYSIZE(args), script_wsl) ||
        wcscat_s(args, ARRAYSIZE(args), L" --root ") != 0 ||
        !append_quoted_arg(args, ARRAYSIZE(args), mount_root) ||
        wcscat_s(args, ARRAYSIZE(args), L" --updater ") != 0 ||
        !append_quoted_arg(args, ARRAYSIZE(args), updater_wsl) ||
        wcscat_s(args, ARRAYSIZE(args), L" --service ") != 0 ||
        !append_quoted_arg(args, ARRAYSIZE(args), service_wsl) ||
        wcscat_s(args, ARRAYSIZE(args), L" --watch ") != 0 ||
        !append_quoted_arg(args, ARRAYSIZE(args), watch_wsl) ||
        wcscat_s(args, ARRAYSIZE(args), L" --bootstrap-root ") != 0 ||
        !append_quoted_arg(args, ARRAYSIZE(args), bootstrap_wsl) ||
        !run_wsl_args(args, 300000UL)) goto done;
    migrated = TRUE;
done:
    if (mounted) {
        args[0] = L'\0';
        if (swprintf_s(args, ARRAYSIZE(args), L"--unmount ") < 0 ||
            !append_quoted_arg(args, ARRAYSIZE(args), instance->vhdx_path) ||
            !run_wsl_args(args, 120000UL)) {
            update_set(instance->unique_id, ASB_UPDATE_FAILED, 0, FALSE, NULL,
                       "legacy_migration_unmount_failed");
            return FALSE;
        }
        mounted = FALSE;
    }
    if (!stopped) {
        update_set(instance->unique_id, ASB_UPDATE_FAILED, 0, FALSE, NULL,
                   "legacy_migration_shutdown_failed");
        return FALSE;
    }
    if (!wait_for_vhdx_release(instance->vhdx_path)) {
        update_set(instance->unique_id, ASB_UPDATE_FAILED, 0, FALSE, NULL,
                   "legacy_migration_vhdx_release_failed");
        return FALSE;
    }
    restarted = restart_after_offline_migration(instance, migrated);
    if (!restarted) {
        update_set(instance->unique_id, ASB_UPDATE_FAILED, 0, FALSE, NULL,
                   "legacy_migration_restart_failed");
        return FALSE;
    }
    if (!migrated) {
        update_set(instance->unique_id, ASB_UPDATE_FAILED, 0, FALSE, NULL,
                   "legacy_migration_failed");
        return FALSE;
    }
    update_set(instance->unique_id, ASB_UPDATE_VERIFYING, 8, FALSE, NULL, NULL);
    return TRUE;
}

static void hex_digest(const BYTE digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) { out[i * 2] = hex[digest[i] >> 4]; out[i * 2 + 1] = hex[digest[i] & 15]; }
    out[64] = '\0';
}

static BOOL transfer_bundle(UpdateJob *job, const char *txid,
                            ULONGLONG size, const BYTE digest[32])
{
    SOCKET s = INVALID_SOCKET;
    HANDLE file = INVALID_HANDLE_VALUE;
    UpdateStreamHeader h;
    BYTE buf[1024 * 1024];
    DWORD read = 0;
    ULONGLONG sent = 0;
    VmInstance *vm = asb_find_vm_by_id(job->vm_id);
    BOOL ok = FALSE;
    if (!vm) return FALSE;
    s = connect_update_transport(vm, UPDATE_SERVICE_PORT);
    if (s == INVALID_SOCKET) return FALSE;
    file = CreateFileW(job->bundle_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    ZeroMemory(&h, sizeof(h));
    memcpy(h.magic, "ASBUPD1", 7); h.protocol = UPDATE_PROTOCOL; h.header_size = sizeof(h);
    strncpy_s(h.txid, sizeof(h.txid), txid, _TRUNCATE); h.bundle_size = size; memcpy(h.sha256, digest, 32);
    if (!send_all(s, &h, sizeof(h))) goto done;
    while (sent < size) {
        DWORD want = (DWORD)((size - sent) > sizeof(buf) ? sizeof(buf) : size - sent);
        if (job->cancel || !ReadFile(file, buf, want, &read, NULL) || !read || !send_all(s, buf, read)) goto done;
        sent += read;
        update_set(job->vm_id, ASB_UPDATE_TRANSFERRING, 10 + (int)((sent * 65) / size), FALSE, txid, NULL);
    }
    ok = TRUE;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (s != INVALID_SOCKET) closesocket(s);
    return ok;
}

typedef enum UpdateOutcome { UPDATE_OUTCOME_COMMITTED, UPDATE_OUTCOME_ROLLBACK,
                             UPDATE_OUTCOME_FAILED, UPDATE_OUTCOME_TIMEOUT } UpdateOutcome;

static UpdateOutcome wait_for_commit(UpdateJob *job, const char *txid, BOOL *reboot)
{
    DWORD start = GetTickCount();
    char cmd[128], response[256];
    for (;;) {
        VmInstance *vm = asb_find_vm_by_id(job->vm_id);
        if (job->cancel || !vm) return UPDATE_OUTCOME_FAILED;
        if (vm->agent_online) {
            char state[32] = {0};
            int status_reboot = 0, progress = 0;
            sprintf_s(cmd, sizeof(cmd), "update_status %s", txid);
            if (vm_agent_request(vm, cmd, response, sizeof(response), 5000)) {
                if (sscanf_s(response, "state=%31[a-z_];reboot_required=%d;progress=%d",
                             state, (unsigned)sizeof(state), &status_reboot, &progress) == 3 &&
                    (status_reboot == 0 || status_reboot == 1) && progress >= 0 && progress <= 100) {
                    *reboot = status_reboot != 0;
                    if (!strcmp(state, "committed")) return UPDATE_OUTCOME_COMMITTED;
                    if (!strcmp(state, "rollback")) return UPDATE_OUTCOME_ROLLBACK;
                    if (!strcmp(state, "failed")) return UPDATE_OUTCOME_FAILED;
                    if (!strcmp(state, "reboot_pending"))
                        update_set(job->vm_id, ASB_UPDATE_REBOOTING, progress, TRUE, txid, NULL);
                    else if (!strcmp(state, "health_check"))
                        update_set(job->vm_id, ASB_UPDATE_HEALTH, progress, *reboot, txid, NULL);
                    else if (!strcmp(state, "receiving") || !strcmp(state, "received") ||
                             !strcmp(state, "apply_requested") || !strcmp(state, "verified") ||
                             !strcmp(state, "activating"))
                        update_set(job->vm_id, ASB_UPDATE_APPLYING, progress, *reboot, txid, NULL);
                }
            }
        }
        if (GetTickCount() - start > UPDATE_MAX_WAIT_MS) return UPDATE_OUTCOME_TIMEOUT;
        Sleep(1000);
    }
}

static DWORD WINAPI update_thread_proc(LPVOID param)
{
    UpdateJob *job = (UpdateJob *)param;
    VmInstance *vm = asb_find_vm_by_id(job->vm_id);
    BYTE digest[32];
    ULONGLONG size = 0;
    char sha[65], txid[40], response[256], cmd[256];
    BOOL reboot = FALSE;
    if (!vm || _wcsicmp(vm->os_type, L"Linux") != 0 || !vm->agent_online ||
        !embedded_release_trust_valid()) goto fail;
    update_set(job->vm_id, ASB_UPDATE_VERIFYING, 2, FALSE, NULL, NULL);
    if (!sha256_file(job->bundle_path, digest, &size)) { update_set(job->vm_id, ASB_UPDATE_FAILED, 0, FALSE, NULL, "bundle_read_failed"); goto done; }
    hex_digest(digest, sha);
    if (!bundle_preflight(job->bundle_path, sha)) {
        update_set(job->vm_id, ASB_UPDATE_FAILED, 0, FALSE, NULL, "bundle_signature_or_policy");
        goto done;
    }
    sprintf_s(txid, sizeof(txid), "u%08lx-%08llx", GetTickCount(), (unsigned long long)job->vm_id);
    EnterCriticalSection(&g_update_cs);
    strncpy_s(job->txid, sizeof(job->txid), txid, _TRUNCATE);
    LeaveCriticalSection(&g_update_cs);
    if (!vm->guest_updater_supported) {
        update_set(job->vm_id, ASB_UPDATE_LEGACY_MIGRATION, 5, FALSE, txid, NULL);
        if (!vm_guest_update_bootstrap_legacy(vm)) {
            goto done;
        }
        vm = asb_find_vm_by_id(job->vm_id);
        if (!vm || !vm->agent_online || !vm->guest_updater_supported) {
            update_set(job->vm_id, ASB_UPDATE_FAILED, 0, FALSE, txid,
                       "updater_unavailable");
            goto done;
        }
    }
    sprintf_s(cmd, sizeof(cmd), "update_query");
    if (!vm_agent_request(vm, cmd, response, sizeof(response), 5000) || strcmp(response, "ok")) {
        update_set(job->vm_id, ASB_UPDATE_FAILED, 0, FALSE, txid, "updater_unavailable"); goto done;
    }
    sprintf_s(cmd, sizeof(cmd), "update_begin %s %llu %s", txid, (unsigned long long)size, sha);
    if (!vm_agent_request(vm, cmd, response, sizeof(response), 10000) || strcmp(response, "ready")) { update_set(job->vm_id, ASB_UPDATE_FAILED, 0, FALSE, txid, "begin_rejected"); goto done; }
    if (!transfer_bundle(job, txid, size, digest)) { update_set(job->vm_id, ASB_UPDATE_FAILED, 0, FALSE, txid, "transfer_failed"); goto done; }
    update_set(job->vm_id, ASB_UPDATE_APPLYING, 80, FALSE, txid, NULL);
    sprintf_s(cmd, sizeof(cmd), "update_apply %s", txid);
    if (!vm_agent_request(vm, cmd, response, sizeof(response), 30000) ||
        !strcmp(response, "error:update_failed") || !strncmp(response, "error:", 6)) {
        update_set(job->vm_id, ASB_UPDATE_FAILED, 0, FALSE, txid, "apply_rejected"); goto done;
    }
    if (strcmp(response, "accepted")) {
        update_set(job->vm_id, ASB_UPDATE_FAILED, 0, FALSE, txid, "apply_rejected"); goto done;
    }
    {
        UpdateOutcome outcome = wait_for_commit(job, txid, &reboot);
        if (outcome == UPDATE_OUTCOME_COMMITTED)
            update_set(job->vm_id, ASB_UPDATE_COMMITTED, 100, reboot, txid, NULL);
        else if (outcome == UPDATE_OUTCOME_ROLLBACK)
            update_set(job->vm_id, ASB_UPDATE_ROLLED_BACK, 0, reboot, txid, "guest_rollback");
        else update_set(job->vm_id, ASB_UPDATE_FAILED, 0, reboot, txid,
                        outcome == UPDATE_OUTCOME_TIMEOUT ? "status_timeout" : "guest_update_failed");
    }
    goto done;
fail:
    update_set(job->vm_id, ASB_UPDATE_FAILED, 0, FALSE, NULL, "unsupported_or_offline");
done:
    vm = asb_find_vm_by_id(job->vm_id);
    if (vm) { vm->update_active = FALSE; vm_agent_notify_status(vm); }
    EnterCriticalSection(&g_update_cs);
    job->thread = NULL;
    job->vm_id = 0;
    job->cancel = FALSE;
    job->bundle_path[0] = L'\0';
    LeaveCriticalSection(&g_update_cs);
    return 0;
}

BOOL vm_guest_update_start(VmInstance *instance, const wchar_t *bundle_path)
{
    int i;
    UpdateJob *job = NULL;
    update_ensure_init();
    if (!instance || !bundle_path || !bundle_path[0] || _wcsicmp(instance->os_type, L"Linux") != 0 ||
        !instance->agent_online || instance->update_active ||
        wcslen(bundle_path) >= MAX_PATH) return FALSE;
    EnterCriticalSection(&g_update_cs);
    for (i = 0; i < MAX_UPDATE_JOBS; i++) if (g_jobs[i].thread && g_jobs[i].vm_id == instance->unique_id) { LeaveCriticalSection(&g_update_cs); return FALSE; }
    for (i = 0; i < MAX_UPDATE_JOBS; i++) if (!g_jobs[i].thread && !g_jobs[i].vm_id) { job = &g_jobs[i]; break; }
    if (!job) { LeaveCriticalSection(&g_update_cs); return FALSE; }
    ZeroMemory(job, sizeof(*job)); job->vm_id = instance->unique_id; wcscpy_s(job->bundle_path, MAX_PATH, bundle_path);
    instance->update_active = TRUE; instance->update_state = ASB_UPDATE_VERIFYING; instance->update_progress = 0; instance->update_error[0] = 0;
    job->thread = CreateThread(NULL, 0, update_thread_proc, job, 0, NULL);
    if (!job->thread) { job->vm_id = 0; instance->update_active = FALSE; LeaveCriticalSection(&g_update_cs); return FALSE; }
    CloseHandle(job->thread); /* The slot is marked active by vm_id; no join is required. */
    LeaveCriticalSection(&g_update_cs);
    vm_agent_notify_status(instance);
    return TRUE;
}

BOOL vm_guest_update_cancel(VmInstance *instance)
{
    int i;
    BOOL found = FALSE;
    char txid[sizeof(g_jobs[0].txid)] = "";
    update_ensure_init();
    if (!instance) return FALSE;
    EnterCriticalSection(&g_update_cs);
    for (i = 0; i < MAX_UPDATE_JOBS; i++) if (g_jobs[i].vm_id == instance->unique_id && g_jobs[i].thread) {
        g_jobs[i].cancel = TRUE;
        strncpy_s(txid, sizeof(txid), g_jobs[i].txid, _TRUNCATE);
        found = TRUE;
        break;
    }
    LeaveCriticalSection(&g_update_cs);
    if (found && txid[0]) {
        char cmd[128], response[128];
        sprintf_s(cmd, sizeof(cmd), "update_cancel %s", txid);
        /* The local flag stops the host transfer/wait immediately; this best
         * effort command also removes a staged guest bundle when activation
         * has not acquired the updater lock yet. */
        vm_agent_request(instance, cmd, response, sizeof(response), 5000);
    }
    return found;
}
