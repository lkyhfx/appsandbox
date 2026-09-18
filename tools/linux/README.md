# tools/linux — Linux-side build of AppSandbox guest components

Everything in this tree compiles on a Linux box and produces artifacts that
the Windows AppSandbox host needs in order to stage a Linux VM. The
components:

| Subdir       | What it builds                          | Where it ends up in the guest                              |
|--------------|-----------------------------------------|------------------------------------------------------------|
| `agent/`     | 5 userspace daemons (ELF)               | `/usr/local/bin/appsandbox-{agent,audio,clipboard,display,input}` |
| `dxgkrnl/`   | Hyper-V GPU paravirt kernel module      | `/lib/modules/<kver>/updates/dxgkrnl.ko` (or DKMS)         |
| `asb_drm/`   | Stub DRM driver for KMS capture         | `/lib/modules/<kver>/updates/asb_drm.ko` (or DKMS)         |
| `wsl-mesa/`  | (optional) Mesa with d3d12 + dzn        | tarball extracted to `/opt/wsl-mesa/`                      |

`wsl-deps/` (Microsoft's proprietary `libd3d12.so` etc.) is **not** built
here — it's fetched by the Windows host from Microsoft's public NuGet feed
into `C:\ProgramData\AppSandbox\wsl-deps\`. See `tools/wsl-deps/README.md`.

## Build

Prereqs (Ubuntu 26.04+; package names may differ on other distros):

```bash
sudo apt-get install -y \
    build-essential dkms zstd \
    linux-headers-$(uname -r) \
    libdrm-dev libasound2-dev libxcb1-dev libxcb-xfixes0-dev
```

Then from this directory:

```bash
make
```

That builds the 5 daemons, both kernel modules against your running kernel,
and installs them under `tools/linux/dist/`:

```
tools/linux/dist/
├── appsandbox-agent           ← strip'd ELF
├── appsandbox-audio
├── appsandbox-clipboard
├── appsandbox-display
├── appsandbox-input
└── modules/
    └── <uname-r>/
        ├── dxgkrnl.ko
        └── asb_drm.ko
```

`dist/` is gitignored — it's only valid on this machine for this kernel.

Other targets:

| Target          | What it does                                              |
|-----------------|-----------------------------------------------------------|
| `make compile`  | builds in place but doesn't copy to `dist/`               |
| `make agents`   | builds only the userspace daemons                         |
| `make modules`  | builds only the kernel modules                            |
| `make clean`    | removes `*.o`, `*.ko`, daemon ELFs from source dirs       |
| `make distclean`| `clean` + removes `dist/`                                 |

## Transfer to the Windows host

This step is **manual on purpose** — there's no assumption of network
connectivity between your Linux build box and the Windows machine that
runs AppSandbox.

After a successful build, get `dist/`'s contents over to your Windows host
and place them at:

```
<appsandbox-repo>\release\resources\linux\
```

The simplest approach is a tarball:

```bash
tar -C dist -czf ~/asb-linux.tar.gz .
```

Then transfer `asb-linux.tar.gz` to the Windows host by whatever means you
normally use to move files between these two systems (USB stick, shared
filesystem, file-share VM, scp, manual upload — whatever works for your
setup). On the Windows side, in your appsandbox checkout:

```cmd
mkdir release\resources\linux
tar xzf C:\path\to\asb-linux.tar.gz -C release\resources\linux\
```

`tar.exe` ships with Windows 10+; if it isn't on your PATH the Git for
Windows / 7-Zip / WinRAR equivalents all handle `.tar.gz` fine.

## Isolated Mutter compositor-output probe

The Mutter experiment is opt-in and does not modify or stop the production
`appsandbox-display` path. It uses the existing validated consumer stages,
but replaces the synthetic producer with an isolated Mutter/Mesa producer:

1. Apply `wsl-mesa/patches/0002-d3d12-mutter-native-share-probe.patch` to the
   isolated Mesa d3d12 build. The hook observes the real Gallium D3D12
   framebuffer and records a GPU-only copy into a three-slot native shared
   BGRA8 ring.
2. Apply `agent/mutter-d3d12-share-probe.patch` to the isolated Mutter build.
   The hook is enabled only when
   `ASB_MUTTER_D3D12_SHARE_SOCKET` is set.
3. Build the socket-facing consumer:

   ```bash
   cd tools/linux/agent
   make d3d12-mutter-consumer D3D12_LIBDIR=/opt/appsandbox/wsl-deps
   ```

4. Run the isolated session from the same directory:

   ```bash
   MUTTER_TEST_CLIENT_CMD='/path/to/opaque-wayland-test-client --fullscreen' \
     GPU_RUNNER=appsandbox-gpu \
     ./gpu-mutter-d3d12-share-probe.sh gpu-mutter-d3d12-share-probe-results
   ```

`MUTTER_TEST_CLIENT_CMD` is mandatory. It must keep a real opaque dynamic
Wayland surface visible for the 3,600-frame capture and paint the advertised
frame marker at the diagnostic points; screenshots, synthetic producer
buffers, framebuffer mmap, and CPU pixel copies are not accepted as evidence.
The runner writes `mutter-consumer.log`, `mutter-session.log`, `runner.log`,
`decode.log`, and `mutter.hevc` under the output directory. It prints the
final PASS only after real-render-target evidence, SCM_RIGHTS/eventfd
handoff, GPU BGRA-to-NV12, GPU HEVC encode, and 4K60/3,600-frame decode all
pass. If the isolated Linux/GPU-PV environment is unavailable it records a
BLOCKED result instead of claiming success.

## How the staged files reach a running VM

1. `release/resources/linux/` is a committed staging dir on the Windows
   side — same model as the existing `release/resources/appsandbox-*.exe`
   files used for Windows guests.
2. `AppSandbox.vcxproj`'s PostBuildEvent xcopies `release/resources/` into
   `bin/<cfg>/resources/` next to `AppSandbox.exe` and `AppSandboxCore.dll`.
   That happens on every build, so a rebuild after copying the new files
   makes them runtime-visible.
3. When you create a Linux VM, `disk_util.c::iso_create_resources_ubuntu`
   builds a `cidata.iso` with `user-data` (autoinstall) + an `extras/` tree
   that includes everything from `<exe_dir>/resources/linux/`.
4. On first boot of the VM, autoinstall mounts the cidata ISO, copies
   `extras/` into `/opt/appsandbox/`, then runs `setup.sh` via
   `curtin in-target`. `setup.sh` installs the agent ELFs into
   `/usr/local/bin/`, the kernel modules into `/lib/modules/<kver>/updates/`,
   wires up systemd units, and enables them.

## Kernel module version skew

Kernel modules built here only load against an **identical** kernel version
in the guest (`uname -r` must match exactly). If the guest is on a different
kernel from your build machine, `setup.sh` automatically falls back to the
DKMS build path — it copies the source from `extras/dxgkrnl/src/` to
`/usr/src/dxgkrnl-<ver>/` and runs `dkms install`. DKMS handles future
kernel upgrades automatically. The committed `.ko` is just a fast path.
