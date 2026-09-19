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
#define BOOTSTRAP_SERVICE_PORT 10
#define BOOTSTRAP_MAX_SIZE (64ULL * 1024ULL * 1024ULL)
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

#pragma pack(push, 1)
typedef struct BootstrapHeader {
    char magic[8];
    DWORD protocol;
    DWORD header_size;
    ULONGLONG payload_size;
    BYTE sha256[32];
} BootstrapHeader;
typedef struct BootstrapPayloadHeader {
    ULONGLONG updater_size;
    ULONGLONG service_size;
    ULONGLONG watch_size;
} BootstrapPayloadHeader;
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

static BOOL sha256_buffer(const BYTE *data, SIZE_T size, BYTE digest[32])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object = NULL;
    DWORD object_len = 0, result = 0;
    BOOL ok = FALSE;
    if (!data || !digest || size > 0xffffffffULL) return FALSE;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0 ||
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_len,
                          sizeof(object_len), &result, 0) != 0) goto done;
    object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, object_len);
    if (!object || BCryptCreateHash(alg, &hash, object, object_len, NULL, 0, 0) != 0 ||
        BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0) != 0 ||
        BCryptFinishHash(hash, digest, 32, 0) != 0) goto done;
    ok = TRUE;
done:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (object) HeapFree(GetProcessHeap(), 0, object);
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
    wchar_t command_line[MAX_PATH * 2];
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
    if (swprintf_s(command_line, ARRAYSIZE(command_line), L"--verify-bundle ") < 0 ||
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

static BOOL pinned_updater_binary(wchar_t *binary, size_t cap)
{
    BYTE digest[32];
    ULONGLONG file_size = 0;
    if (!get_linux_resource_path(L"updater\\appsandbox-guest-updater", binary, cap) ||
        !sha256_file(binary, digest, &file_size) ||
        !verify_pinned_resource(binary, ASB_BOOTSTRAP_UPDATER_SHA256)) return FALSE;
    return TRUE;
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

static BOOL transfer_bootstrap(UpdateJob *job, const char *expected_sha)
{
    VmInstance *vm = asb_find_vm_by_id(job->vm_id);
    wchar_t binary_path[MAX_PATH], service_path[MAX_PATH], watch_path[MAX_PATH];
    BYTE *updater = NULL, *service = NULL, *watch = NULL, *payload = NULL, digest[32];
    ULONGLONG updater_size = 0, service_size = 0, watch_size = 0, payload_size;
    BootstrapPayloadHeader payload_header;
    BootstrapHeader header;
    SOCKET s = INVALID_SOCKET;
    char response[64] = {0};
    int n;
    BOOL ok = FALSE;
    if (!vm || !expected_sha ||
        !pinned_updater_binary(binary_path, ARRAYSIZE(binary_path)) ||
        !get_linux_resource_path(L"updater\\appsandbox-guest-updater.service", service_path, ARRAYSIZE(service_path)) ||
        !get_linux_resource_path(L"updater\\appsandbox-guest-update-watch.service", watch_path, ARRAYSIZE(watch_path)) ||
        !read_file_bytes(binary_path, &updater, &updater_size, BOOTSTRAP_MAX_SIZE) ||
        !read_file_bytes(service_path, &service, &service_size, 1024 * 1024) ||
        !read_file_bytes(watch_path, &watch, &watch_size, 1024 * 1024)) goto done;
    if (updater_size > BOOTSTRAP_MAX_SIZE || service_size > 1024 * 1024 || watch_size > 1024 * 1024 ||
        updater_size > 0xffffffffffffffffULL - sizeof(payload_header)) goto done;
    payload_size = sizeof(payload_header) + updater_size + service_size + watch_size;
    if (payload_size > BOOTSTRAP_MAX_SIZE || !sha256_buffer(updater, (SIZE_T)updater_size, digest)) goto done;
    {
        char actual[65];
        hex_digest(digest, actual);
        if (_stricmp(actual, expected_sha) != 0) goto done;
    }
    payload = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)payload_size);
    if (!payload) goto done;
    payload_header.updater_size = updater_size;
    payload_header.service_size = service_size;
    payload_header.watch_size = watch_size;
    memcpy(payload, &payload_header, sizeof(payload_header));
    memcpy(payload + sizeof(payload_header), updater, (SIZE_T)updater_size);
    memcpy(payload + sizeof(payload_header) + (SIZE_T)updater_size, service, (SIZE_T)service_size);
    memcpy(payload + sizeof(payload_header) + (SIZE_T)updater_size + (SIZE_T)service_size,
           watch, (SIZE_T)watch_size);
    if (!sha256_buffer(payload, (SIZE_T)payload_size, digest)) goto done;
    /* Arm the guest through the fixed legacy-agent command before opening the
     * dedicated bootstrap VSOCK channel. */
    {
        char sha[65], cmd[128];
        char arm_response[128];
        hex_digest(digest, sha);
        sprintf_s(cmd, sizeof(cmd), "bootstrap_updater %llu %s",
                  (unsigned long long)payload_size, sha);
        if (!vm_agent_request(vm, cmd, arm_response, sizeof(arm_response), 10000) ||
            strcmp(arm_response, "bootstrap_ready")) goto done;
    }
    s = connect_update_transport(vm, BOOTSTRAP_SERVICE_PORT);
    if (s == INVALID_SOCKET) goto done;
    ZeroMemory(&header, sizeof(header));
    memcpy(header.magic, "ASBBST1", 7);
    header.protocol = 1; header.header_size = sizeof(header); header.payload_size = payload_size;
    memcpy(header.sha256, digest, sizeof(digest));
    if (!send_all(s, &header, sizeof(header)) || !send_all(s, payload, (size_t)payload_size)) goto done;
    n = recv(s, response, sizeof(response) - 1, 0);
    if (n <= 0) goto done;
    response[n] = '\0';
    ok = strncmp(response, "ok", 2) == 0;
done:
    if (s != INVALID_SOCKET) closesocket(s);
    if (payload) HeapFree(GetProcessHeap(), 0, payload);
    if (updater) HeapFree(GetProcessHeap(), 0, updater);
    if (service) HeapFree(GetProcessHeap(), 0, service);
    if (watch) HeapFree(GetProcessHeap(), 0, watch);
    return ok;
}

static BOOL bootstrap_guest_updater(UpdateJob *job)
{
    wchar_t binary_path[MAX_PATH], hash_path[MAX_PATH];
    BYTE digest[32], *data = NULL;
    ULONGLONG size = 0, hash_size = 0;
    char sha[65], expected[65] = {0};
    size_t n = 0;
    (void)size;
    if (!get_linux_resource_path(L"updater\\appsandbox-guest-updater", binary_path, ARRAYSIZE(binary_path)) ||
        !get_linux_resource_path(L"updater\\appsandbox-guest-updater.sha256", hash_path, ARRAYSIZE(hash_path)) ||
        !sha256_file(binary_path, digest, &size) || !read_file_bytes(hash_path, &data, &hash_size, 4096)) return FALSE;
    hex_digest(digest, sha);
    while (n < (size_t)hash_size && n < sizeof(expected) - 1 && data[n] != '\r' && data[n] != '\n' &&
           data[n] != ' ' && data[n] != '\t') {
        expected[n] = (char)data[n];
        n++;
    }
    HeapFree(GetProcessHeap(), 0, data);
    if (n != 64 || _stricmp(sha, expected) != 0) return FALSE;
    if (!transfer_bootstrap(job, sha)) return FALSE;
    {
        VmInstance *vm = asb_find_vm_by_id(job->vm_id);
        char response[128];
        if (!vm || !vm_agent_request(vm, "update_migrate", response, sizeof(response), 10000) ||
            (strcmp(response, "migrated") && strcmp(response, "ready"))) return FALSE;
    }
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
    if (!vm || _wcsicmp(vm->os_type, L"Linux") != 0 || !vm->agent_online) goto fail;
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
    sprintf_s(cmd, sizeof(cmd), "update_query");
    if (!vm_agent_request(vm, cmd, response, sizeof(response), 5000) || strcmp(response, "ok")) {
        /* Existing Linux guests have only the legacy agent.  Bootstrap is a
         * fixed binary/unit protocol on VSOCK port 10; it is not a shell
         * escape hatch and it does not accept a destination path. */
        if (!vm->guest_updater_supported && bootstrap_guest_updater(job)) {
            Sleep(1500);
            if (!vm_agent_request(vm, cmd, response, sizeof(response), 10000) || strcmp(response, "ok")) {
                update_set(job->vm_id, ASB_UPDATE_FAILED, 0, FALSE, txid, "updater_unavailable"); goto done;
            }
        } else {
            update_set(job->vm_id, ASB_UPDATE_FAILED, 0, FALSE, txid, "updater_unavailable"); goto done;
        }
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
