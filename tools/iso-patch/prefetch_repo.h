/* prefetch_repo.h - host-side fetch of the appsandbox repo's
 * tools/linux/ subtree from GitHub, straight into the per-VM staging
 * extras dir. No host cache.
 */
#ifndef PREFETCH_REPO_H
#define PREFETCH_REPO_H

#include <windows.h>

/* Download github.com/jamesstringer90/appsandbox @ <branch> and place
 * the tools/linux/ subtree directly into <out_dir> in the final layout
 * the manifest writer expects:
 *
 *   <out_dir>/
 *     agent-src/           (5 .c + Makefile from tools/linux/agent/)
 *     asb_drm-src/         (tools/linux/asb_drm source tree — DKMS source)
 *     dxgkrnl-src/         (tools/linux/dxgkrnl source tree — DKMS source)
 *     systemd/
 *       appsandbox-{agent,audio,clipboard,display,input,firstboot}.service
 *       asb-evict-simpledrm.service   (renamed from systemd-asb-evict-simpledrm.service)
 *       modules-load.d-asb_drm.conf
 *       modules-load.d-snd-aloop.conf
 *     modprobe.d-asb_drm.conf
 *     50-appsandbox-gpu
 *     org.gnome.Shell-no-gpu.conf
 *     appsandbox-gpu
 *     wsl-mesa.tar.zst     (from tools/linux/wsl-mesa/prebuilt/...)
 *
 * Returns 0 on success; non-zero on any failure. */
int do_prefetch_repo(const wchar_t *branch, const wchar_t *out_dir);

/* Production provisioning copies only from the immutable Linux resource tree
 * shipped beside the exact Host release. It never contacts GitHub. */
int do_prefetch_release(const wchar_t *resources_dir, const wchar_t *out_dir);

#endif
