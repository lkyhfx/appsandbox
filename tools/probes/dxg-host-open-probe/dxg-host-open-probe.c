/*
 * Gate B0 probe from the AppSandbox Linux display zero-copy decision tree.
 *
 * This intentionally stops after D3DKMTRegisterVailProcess.  It does not
 * create a D3D12 resource and does not touch the guest.  The next probe
 * stages (A/B) must only run after this registration succeeds.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

typedef void *HCS_SYSTEM;
typedef void *HCS_OPERATION;

typedef HCS_OPERATION (WINAPI *PFN_HcsCreateOperation)(
    const void *context, void *callback);
typedef HRESULT (WINAPI *PFN_HcsWaitForOperationResult)(
    HCS_OPERATION operation, DWORD timeout_ms, PWSTR *result_doc);
typedef void (WINAPI *PFN_HcsCloseOperation)(HCS_OPERATION operation);
typedef HRESULT (WINAPI *PFN_HcsOpenComputeSystem)(
    PCWSTR id, DWORD requested_access, HCS_SYSTEM *compute_system);
typedef HRESULT (WINAPI *PFN_HcsGetComputeSystemProperties)(
    HCS_SYSTEM compute_system, HCS_OPERATION operation, PCWSTR property_query);
typedef void (WINAPI *PFN_HcsCloseComputeSystem)(HCS_SYSTEM compute_system);
typedef HRESULT (WINAPI *PFN_HcsEnumerateComputeSystems)(
    PCWSTR query, HCS_OPERATION operation);

typedef LONG (APIENTRY *PFN_D3DKMTRegisterVailProcess)(GUID *virtual_machine_guid);

static void print_current_process_identity(void)
{
    HANDLE token = NULL;
    DWORD needed = 0;
    PTOKEN_USER user = NULL;
    PTOKEN_MANDATORY_LABEL label = NULL;
    TOKEN_ELEVATION elevation = {0};
    LPSTR sid = NULL;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        printf("host_identity: FAIL win32_error=%lu\n", GetLastError());
        return;
    }

    GetTokenInformation(token, TokenUser, NULL, 0, &needed);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        user = (PTOKEN_USER)HeapAlloc(GetProcessHeap(), 0, needed);
    if (user && GetTokenInformation(token, TokenUser, user, needed, &needed) &&
        ConvertSidToStringSidA(user->User.Sid, &sid)) {
        printf("host_user_sid=%s\n", sid);
        LocalFree(sid);
    }

    needed = 0;
    GetTokenInformation(token, TokenIntegrityLevel, NULL, 0, &needed);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        label = (PTOKEN_MANDATORY_LABEL)HeapAlloc(GetProcessHeap(), 0, needed);
    if (label && GetTokenInformation(token, TokenIntegrityLevel,
                                     label, needed, &needed)) {
        DWORD count = *GetSidSubAuthorityCount(label->Label.Sid);
        DWORD rid = *GetSidSubAuthority(label->Label.Sid, count - 1);
        printf("host_integrity_rid=0x%lX\n", rid);
    }

    needed = sizeof(elevation);
    if (GetTokenInformation(token, TokenElevation, &elevation,
                            sizeof(elevation), &needed))
        printf("host_elevated=%u\n", elevation.TokenIsElevated ? 1u : 0u);

    if (label) HeapFree(GetProcessHeap(), 0, label);
    if (user) HeapFree(GetProcessHeap(), 0, user);
    CloseHandle(token);
}

static void print_guid(const GUID *guid)
{
    printf("%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
           guid->Data1, guid->Data2, guid->Data3,
           guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
           guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
}

static int parse_guid_after(const wchar_t *text, const wchar_t *key, GUID *out)
{
    const wchar_t *p = wcsstr(text, key);
    unsigned long d1;
    unsigned int d2, d3;
    unsigned int b[8];

    if (!p) return 0;
    p += wcslen(key);
    if (swscanf_s(p,
            L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            &d1, &d2, &d3,
            &b[0], &b[1], &b[2], &b[3],
            &b[4], &b[5], &b[6], &b[7]) != 11)
        return 0;

    out->Data1 = d1;
    out->Data2 = (USHORT)d2;
    out->Data3 = (USHORT)d3;
    for (int i = 0; i < 8; ++i)
        out->Data4[i] = (BYTE)b[i];
    return 1;
}

static int parse_guid_value(const wchar_t *text, GUID *out)
{
    unsigned long d1;
    unsigned int d2, d3;
    unsigned int b[8];

    if (swscanf_s(text,
            L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            &d1, &d2, &d3,
            &b[0], &b[1], &b[2], &b[3],
            &b[4], &b[5], &b[6], &b[7]) != 11)
        return 0;

    out->Data1 = d1;
    out->Data2 = (USHORT)d2;
    out->Data3 = (USHORT)d3;
    for (int i = 0; i < 8; ++i)
        out->Data4[i] = (BYTE)b[i];
    return 1;
}

static int hcs_runtime_id(const wchar_t *vm_name, GUID *out)
{
    HMODULE hcs = NULL;
    HCS_SYSTEM system = NULL;
    HCS_OPERATION operation = NULL;
    PWSTR result_doc = NULL;
    HRESULT hr;
    PFN_HcsOpenComputeSystem open_system;
    PFN_HcsCreateOperation create_operation;
    PFN_HcsGetComputeSystemProperties get_properties;
    PFN_HcsWaitForOperationResult wait_result;
    PFN_HcsCloseOperation close_operation;
    PFN_HcsCloseComputeSystem close_system;

    hcs = LoadLibraryW(L"computecore.dll");
    if (!hcs) {
        printf("B0.hcs_load: FAIL win32_error=%lu\n", GetLastError());
        return 0;
    }

    open_system = (PFN_HcsOpenComputeSystem)GetProcAddress(hcs, "HcsOpenComputeSystem");
    create_operation = (PFN_HcsCreateOperation)GetProcAddress(hcs, "HcsCreateOperation");
    get_properties = (PFN_HcsGetComputeSystemProperties)GetProcAddress(
        hcs, "HcsGetComputeSystemProperties");
    wait_result = (PFN_HcsWaitForOperationResult)GetProcAddress(
        hcs, "HcsWaitForOperationResult");
    close_operation = (PFN_HcsCloseOperation)GetProcAddress(hcs, "HcsCloseOperation");
    close_system = (PFN_HcsCloseComputeSystem)GetProcAddress(hcs, "HcsCloseComputeSystem");

    if (!open_system || !create_operation || !get_properties || !wait_result ||
        !close_operation || !close_system) {
        printf("B0.hcs_api: FAIL win32_error=%lu\n", GetLastError());
        FreeLibrary(hcs);
        return 0;
    }

    /* HCS accepts the same access level used by AppSandbox's reconnect path. */
    hr = open_system(vm_name, GENERIC_ALL, &system);
    printf("B0.hcs_open: hr=0x%08lX vm=%ls\n", (unsigned long)hr, vm_name);
    if (FAILED(hr) || !system) {
        FreeLibrary(hcs);
        return 0;
    }

    operation = create_operation(NULL, NULL);
    if (!operation) {
        printf("B0.hcs_operation: FAIL win32_error=%lu\n", GetLastError());
        close_system(system);
        FreeLibrary(hcs);
        return 0;
    }

    hr = get_properties(system, operation, L"{\"PropertyTypes\":[\"Basic\"]}");
    if (SUCCEEDED(hr))
        hr = wait_result(operation, 5000, &result_doc);
    printf("B0.hcs_basic: hr=0x%08lX\n", (unsigned long)hr);

    if (SUCCEEDED(hr) && result_doc) {
        if (!parse_guid_after(result_doc, L"\"RuntimeId\":\"", out)) {
            printf("B0.vm_guid: FAIL source=HCS.Basic runtime_id_missing\n");
            wprintf(L"B0.hcs_basic_doc: %ls\n", result_doc);
            LocalFree(result_doc);
            close_operation(operation);
            close_system(system);
            FreeLibrary(hcs);
            return 0;
        }
        printf("B0.vm_guid: PASS source=HCS.Basic value=");
        print_guid(out);
        printf("\n");
    } else {
        printf("B0.vm_guid: FAIL source=HCS.Basic\n");
        if (result_doc) LocalFree(result_doc);
        close_operation(operation);
        close_system(system);
        FreeLibrary(hcs);
        return 0;
    }

    LocalFree(result_doc);
    close_operation(operation);
    close_system(system);
    FreeLibrary(hcs);
    return 1;
}

int main(int argc, char **argv)
{
    const char *vm_name_utf8 = argc > 1 ? argv[1] : "ubuntu1";
    const char *vm_guid_utf8 = argc > 2 ? argv[2] : NULL;
    wchar_t vm_name[256];
    wchar_t vm_guid_text[64];
    GUID vm_guid;
    HMODULE gdi32;
    PFN_D3DKMTRegisterVailProcess register_vail;
    LONG status;

    if (MultiByteToWideChar(CP_UTF8, 0, vm_name_utf8, -1,
                            vm_name, (int)(sizeof(vm_name) / sizeof(vm_name[0]))) <= 0) {
        fprintf(stderr, "invalid UTF-8 VM name\n");
        return 2;
    }

    printf("probe=dxg-host-open-probe gate=B0\n");
    printf("host_pid=%lu\n", GetCurrentProcessId());
    printf("vm_name=%s\n", vm_name_utf8);
    print_current_process_identity();

    if (vm_guid_utf8) {
        if (MultiByteToWideChar(CP_UTF8, 0, vm_guid_utf8, -1,
                                vm_guid_text, (int)(sizeof(vm_guid_text) /
                                                   sizeof(vm_guid_text[0]))) <= 0 ||
            !parse_guid_value(vm_guid_text, &vm_guid)) {
            printf("B0.vm_guid: FAIL source=argument invalid_guid=%s\n", vm_guid_utf8);
            printf("B0: FAIL reason=vm_guid_unavailable\n");
            return 1;
        }
        printf("B0.vm_guid: PASS source=argument value=");
        print_guid(&vm_guid);
        printf("\n");
    } else if (!hcs_runtime_id(vm_name, &vm_guid)) {
        printf("B0: FAIL reason=vm_guid_unavailable\n");
        return 1;
    }

    gdi32 = LoadLibraryW(L"gdi32.dll");
    if (!gdi32) {
        printf("B0.api: FAIL api=D3DKMTRegisterVailProcess win32_error=%lu\n",
               GetLastError());
        return 1;
    }
    register_vail = (PFN_D3DKMTRegisterVailProcess)GetProcAddress(
        gdi32, "D3DKMTRegisterVailProcess");
    if (!register_vail) {
        printf("B0.api: FAIL api=D3DKMTRegisterVailProcess win32_error=%lu\n",
               GetLastError());
        FreeLibrary(gdi32);
        return 1;
    }

    printf("B0.register: calling D3DKMTRegisterVailProcess vm_guid=");
    print_guid(&vm_guid);
    printf("\n");
    status = register_vail(&vm_guid);
    printf("B0.register: status=0x%08lX\n", (unsigned long)(ULONG)status);

    if (status != 0) {
        if ((ULONG)status == 0xC0000022UL)
            printf("B0: FAIL first_fail=VAIL_PROCESS_NOT_AUTHORIZED "
                   "check=VideoMonitor.ConnectionOptions.AccessSids\n");
        else
            printf("B0: FAIL first_fail=VAIL_REGISTRATION_REQUIRED\n");
        FreeLibrary(gdi32);
        return 1;
    }

    printf("B0: PASS next=GATE_A\n");
    FreeLibrary(gdi32);
    return 0;
}
