#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef void *HCS_SYSTEM;
typedef void *HCS_OPERATION;

typedef HCS_OPERATION (WINAPI *PFN_HcsCreateOperation)(const void *, void *);
typedef HRESULT (WINAPI *PFN_HcsWaitForOperationResult)(HCS_OPERATION, DWORD, PWSTR *);
typedef void (WINAPI *PFN_HcsCloseOperation)(HCS_OPERATION);
typedef HRESULT (WINAPI *PFN_HcsOpenComputeSystem)(PCWSTR, DWORD, HCS_SYSTEM *);
typedef HRESULT (WINAPI *PFN_HcsShutdownComputeSystem)(HCS_SYSTEM, HCS_OPERATION, PCWSTR);
typedef void (WINAPI *PFN_HcsCloseComputeSystem)(HCS_SYSTEM);

int wmain(int argc, wchar_t **argv)
{
    HMODULE dll;
    HCS_SYSTEM system = NULL;
    HCS_OPERATION operation = NULL;
    PWSTR result = NULL;
    HRESULT hr;
    PFN_HcsCreateOperation create_operation;
    PFN_HcsWaitForOperationResult wait_result;
    PFN_HcsCloseOperation close_operation;
    PFN_HcsOpenComputeSystem open_system;
    PFN_HcsShutdownComputeSystem shutdown_system;
    PFN_HcsCloseComputeSystem close_system;

    if (argc != 2) {
        fwprintf(stderr, L"usage: hcs-vm-shutdown.exe <vm-name>\n");
        return 2;
    }

    dll = LoadLibraryW(L"computecore.dll");
    if (!dll) return 3;

    create_operation = (PFN_HcsCreateOperation)GetProcAddress(
        dll, "HcsCreateOperation");
    wait_result = (PFN_HcsWaitForOperationResult)GetProcAddress(
        dll, "HcsWaitForOperationResult");
    close_operation = (PFN_HcsCloseOperation)GetProcAddress(
        dll, "HcsCloseOperation");
    open_system = (PFN_HcsOpenComputeSystem)GetProcAddress(
        dll, "HcsOpenComputeSystem");
    shutdown_system = (PFN_HcsShutdownComputeSystem)GetProcAddress(
        dll, "HcsShutdownComputeSystem");
    close_system = (PFN_HcsCloseComputeSystem)GetProcAddress(
        dll, "HcsCloseComputeSystem");

    if (!create_operation || !wait_result || !close_operation || !open_system ||
        !shutdown_system || !close_system) {
        FreeLibrary(dll);
        return 4;
    }

    hr = open_system(argv[1], GENERIC_ALL, &system);
    wprintf(L"hcs_open: hr=0x%08lX vm=%ls\n", (unsigned long)hr, argv[1]);
    if (FAILED(hr)) goto done;

    operation = create_operation(NULL, NULL);
    if (!operation) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }

    hr = shutdown_system(system, operation, L"{}");
    wprintf(L"hcs_shutdown: hr=0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        hr = wait_result(operation, 60000, &result);
        wprintf(L"hcs_shutdown_wait: hr=0x%08lX\n", (unsigned long)hr);
        if (result && result[0]) wprintf(L"hcs_result: %ls\n", result);
    }

done:
    if (result) LocalFree(result);
    if (operation) close_operation(operation);
    if (system) close_system(system);
    FreeLibrary(dll);
    return FAILED(hr) ? 1 : 0;
}
