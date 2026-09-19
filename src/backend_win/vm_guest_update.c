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
#include <limits.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

#define UPDATE_SERVICE_PORT 9
#define UPDATE_PROTOCOL     1
#define UPDATE_MAX_BUNDLE   (512ULL * 1024ULL * 1024ULL)
#define UPDATE_MAX_WAIT_MS  (180000UL)

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

static SOCKET connect_update_transport(VmInstance *vm)
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
    hcs_service_guid(vm->os_type, UPDATE_SERVICE_PORT, &addr.ServiceId);
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

/* The guest remains the authority for the signed manifest, but reject an
 * obviously wrong container before opening the VSOCK update transaction. This
 * keeps the host-side state machine from staging arbitrary files and gives a
 * useful error for a user who selected the wrong artifact. */
static BOOL bundle_preflight(const wchar_t *path)
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
    return magic[0] == 0x28 && magic[1] == 0xB5 && magic[2] == 0x2F && magic[3] == 0xFD;
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
    s = connect_update_transport(vm);
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

static BOOL wait_for_commit(UpdateJob *job, const char *txid, BOOL reboot)
{
    DWORD start = GetTickCount();
    char cmd[128], response[256];
    for (;;) {
        VmInstance *vm = asb_find_vm_by_id(job->vm_id);
        if (job->cancel || !vm) return FALSE;
        if (!reboot && vm->agent_online) {
            sprintf_s(cmd, sizeof(cmd), "update_status %s", txid);
            if (vm_agent_request(vm, cmd, response, sizeof(response), 5000)) {
                if (!strcmp(response, "committed")) return TRUE;
                if (!strncmp(response, "rollback", 8) || !strncmp(response, "failed", 6) ||
                    !strncmp(response, "error:", 6)) return FALSE;
            }
        } else if (reboot && vm->agent_online) {
            sprintf_s(cmd, sizeof(cmd), "update_status %s", txid);
            if (vm_agent_request(vm, cmd, response, sizeof(response), 5000)) {
                if (!strcmp(response, "committed")) return TRUE;
                if (!strncmp(response, "rollback", 8) || !strncmp(response, "failed", 6) ||
                    !strncmp(response, "error:", 6)) return FALSE;
            }
        }
        if (GetTickCount() - start > UPDATE_MAX_WAIT_MS) return FALSE;
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
    if (!bundle_preflight(job->bundle_path)) {
        update_set(job->vm_id, ASB_UPDATE_FAILED, 0, FALSE, NULL, "bundle_format");
        goto done;
    }
    if (!sha256_file(job->bundle_path, digest, &size)) { update_set(job->vm_id, ASB_UPDATE_FAILED, 0, FALSE, NULL, "bundle_read_failed"); goto done; }
    hex_digest(digest, sha);
    sprintf_s(txid, sizeof(txid), "u%08lx-%08llx", GetTickCount(), (unsigned long long)job->vm_id);
    EnterCriticalSection(&g_update_cs);
    strncpy_s(job->txid, sizeof(job->txid), txid, _TRUNCATE);
    LeaveCriticalSection(&g_update_cs);
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
    reboot = !strcmp(response, "reboot_required");
    update_set(job->vm_id, reboot ? ASB_UPDATE_REBOOTING : ASB_UPDATE_HEALTH, 90, reboot, txid, NULL);
    if (wait_for_commit(job, txid, reboot)) update_set(job->vm_id, ASB_UPDATE_COMMITTED, 100, reboot, txid, NULL);
    else update_set(job->vm_id, ASB_UPDATE_ROLLED_BACK, 0, reboot, txid, "guest_rollback_or_timeout");
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
        !instance->agent_online || !instance->guest_updater_supported || instance->update_active ||
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
