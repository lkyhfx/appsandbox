#ifndef VM_DISPLAY_IDD_H
#define VM_DISPLAY_IDD_H

#include <windows.h>
#include "hcs_vm.h"

typedef struct VmDisplayIdd VmDisplayIdd;

/* Create IDD display window for VM. Connects to VM's AF_HYPERV channels
   (:0002 frames, :0003 input, :0005 clipboard writer, :0006 clipboard reader)
   and renders received frames via D3D11.
   main_hwnd receives WM_VM_DISPLAY_CLOSED when closed. */
VmDisplayIdd *vm_display_idd_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd);

void vm_display_idd_destroy(VmDisplayIdd *display);
BOOL vm_display_idd_is_open(VmDisplayIdd *display);

/* Bring an already-open display window to the foreground/focus.
   Safe to call from any thread; the work is marshaled to the window thread. */
void vm_display_idd_focus(VmDisplayIdd *display);

/* Request one of the fixed guest display presets while the VM remains
   running. Completion is reported asynchronously to the main window after a
   matching ASFR frame, never on ACK alone. */
BOOL vm_display_idd_set_runtime_display(VmDisplayIdd *display,
                                         DWORD width, DWORD height);

/* main-window notification sent by the IDD receiver when a runtime request
   completes. LPARAM owns a HeapAlloc'ed VmDisplayRuntimeResult. */
#define WM_VM_DISPLAY_RUNTIME_RESULT (WM_APP + 20)
typedef struct VmDisplayRuntimeResult {
    VmInstance *vm;
    DWORD width;
    DWORD height;
    BOOL success;
} VmDisplayRuntimeResult;

#endif /* VM_DISPLAY_IDD_H */
