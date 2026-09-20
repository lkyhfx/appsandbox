#ifndef LINUX_TTY_H
#define LINUX_TTY_H

#include "hcs_vm.h"

/* Start/stop the host-side Linux COM1 capture.  The capture is diagnostic
   only: these functions never report an error to the VM lifecycle. */
void linux_tty_start(VmInstance *instance);
void linux_tty_stop(VmInstance *instance);
/* Move the sole capture owner when a temporary VM instance is finalized. */
void linux_tty_transfer(VmInstance *from, VmInstance *to);

#endif /* LINUX_TTY_H */
