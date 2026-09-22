/*
 * Validate Gate B0 from inside an actual RDP/VAIL client process.
 *
 * Unlike dxg-host-open-probe.c, this probe first connects the VM's
 * VideoMonitor BasicSession endpoint through the same RDP ActiveX path used
 * by AppSandbox's legacy viewer.  D3DKMTRegisterVailProcess is then retried
 * while that connection is being established.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <d3d12.h>

#include "vm_display.h"

typedef LONG (APIENTRY *PFN_D3DKMTRegisterVailProcess)(GUID *vm_guid);

void ui_log(const wchar_t *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    wprintf(L"rdp: ");
    vwprintf(fmt, args);
    wprintf(L"\n");
    fflush(stdout);
    va_end(args);
}

static int parse_guid(const wchar_t *text, GUID *out)
{
    unsigned long d1;
    unsigned int d2, d3, b[8];

    if (swscanf_s(text,
            L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3],
            &b[4], &b[5], &b[6], &b[7]) != 11)
        return 0;

    out->Data1 = d1;
    out->Data2 = (USHORT)d2;
    out->Data3 = (USHORT)d3;
    for (int i = 0; i < 8; ++i)
        out->Data4[i] = (BYTE)b[i];
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    VmInstance vm;
    VmDisplay *display;
    GUID vm_guid;
    HMODULE gdi32;
    PFN_D3DKMTRegisterVailProcess register_vail;
    ID3D12Device *d3d_device = NULL;
    HRESULT hr;
    LONG status = (LONG)0xC0000022L;

    if ((argc != 3 && argc != 4) || !parse_guid(argv[2], &vm_guid)) {
        fwprintf(stderr, L"usage: %ls <vm-name> <runtime-guid> [--register-only]\n",
                 argv[0]);
        return 2;
    }

    ZeroMemory(&vm, sizeof(vm));
    wcsncpy_s(vm.name, sizeof(vm.name) / sizeof(vm.name[0]),
              argv[1], _TRUNCATE);
    vm.runtime_id = vm_guid;
    vm.running = TRUE;

    gdi32 = LoadLibraryW(L"gdi32.dll");
    register_vail = gdi32 ? (PFN_D3DKMTRegisterVailProcess)
        GetProcAddress(gdi32, "D3DKMTRegisterVailProcess") : NULL;
    if (!register_vail) {
        printf("B0.api: FAIL win32_error=%lu\n", GetLastError());
        if (gdi32) FreeLibrary(gdi32);
        return 1;
    }

    wprintf(L"probe=rdp-vail-context-probe vm=%ls pid=%lu\n",
            vm.name, GetCurrentProcessId());
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                           &IID_ID3D12Device, (void **)&d3d_device);
    printf("B0.graphics_context: hr=0x%08lX created=%u\n",
           (unsigned long)hr, d3d_device ? 1u : 0u);
    if (FAILED(hr)) {
        FreeLibrary(gdi32);
        return 1;
    }

    if (argc == 4 && wcscmp(argv[3], L"--register-only") == 0) {
        status = register_vail(&vm_guid);
        printf("B0.child_register_vail: status=0x%08lX\n",
               (unsigned long)status);
        ID3D12Device_Release(d3d_device);
        FreeLibrary(gdi32);
        return status == 0 ? 0 : 1;
    }

    display = vm_display_create(&vm, GetModuleHandleW(NULL), NULL);
    if (!display) {
        printf("B0.rdp_client: FAIL create_failed\n");
        ID3D12Device_Release(d3d_device);
        FreeLibrary(gdi32);
        return 1;
    }

    /* The pipe and ActiveX callbacks run on vm_display's STA thread. */
    for (int attempt = 1; attempt <= 30; ++attempt) {
        if (attempt == 6) {
            wchar_t exe_path[MAX_PATH];
            wchar_t command[2 * MAX_PATH];
            STARTUPINFOW si;
            PROCESS_INFORMATION pi;

            ZeroMemory(&si, sizeof(si));
            ZeroMemory(&pi, sizeof(pi));
            si.cb = sizeof(si);
            GetModuleFileNameW(NULL, exe_path, MAX_PATH);
            swprintf_s(command, sizeof(command) / sizeof(command[0]),
                       L"\"%s\" \"%s\" \"%s\" --register-only",
                       exe_path, argv[1], argv[2]);
            if (CreateProcessW(NULL, command, NULL, NULL, TRUE, 0,
                               NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 5000);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            } else {
                printf("B0.child: FAIL win32_error=%lu\n", GetLastError());
            }
        }
        Sleep(500);
        status = register_vail(&vm_guid);
        printf("B0.register_vail: attempt=%d status=0x%08lX\n",
               attempt, (unsigned long)status);
        fflush(stdout);
        if (status != (LONG)0xC0000022L)
            break;
    }

    Sleep(1000);
    vm_display_disconnect(display);
    vm_display_destroy(display);
    ID3D12Device_Release(d3d_device);
    FreeLibrary(gdi32);

    if (status == 0) {
        printf("B0: PASS context=active_rdp_basic_session\n");
        return 0;
    }
    printf("B0: FAIL context=active_rdp_basic_session status=0x%08lX\n",
           (unsigned long)status);
    return 1;
}
