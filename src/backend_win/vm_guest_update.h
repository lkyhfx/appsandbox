#ifndef VM_GUEST_UPDATE_H
#define VM_GUEST_UPDATE_H

#include <windows.h>
#include "hcs_vm.h"

/* Start the signed Linux Guest Runtime Bundle workflow. The path is copied by
   the updater thread and may be released by the caller after this returns. */
ASB_API BOOL vm_guest_update_start(VmInstance *instance, const wchar_t *bundle_path);

/* Best-effort cancellation before activation. The guest remains authoritative
   if the host disappears or a reboot is already pending. */
ASB_API BOOL vm_guest_update_cancel(VmInstance *instance);

/* Stop a legacy guest, mount its existing VHDX offline, install the
   bootstrap-capable control agent/updater transaction, then restart the same
   disk with current HCS configuration and wait for update-v1 capability. */
BOOL vm_guest_update_bootstrap_legacy(VmInstance *instance);

#endif /* VM_GUEST_UPDATE_H */
