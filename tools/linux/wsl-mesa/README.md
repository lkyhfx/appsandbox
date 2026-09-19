# wsl-mesa — Mesa with d3d12 gallium + dzn Vulkan, prebuilt

Custom Mesa build that exposes Hyper-V GPU-PV (via `/dev/dxg`) to
userspace OpenGL and Vulkan workloads on a Linux guest, with zero
Microsoft proprietary blobs.

Meson flags:

```
-D gallium-drivers=llvmpipe,d3d12
-D vulkan-drivers=swrast,microsoft-experimental
-D microsoft-clc=enabled
-D video-codecs=all
-D glvnd=enabled
```

The two drivers we care about are both pure-open-source: Mesa's
`d3d12` gallium driver and the `microsoft-experimental` Vulkan driver
(aka **dzn**, Direct3D 12 backend for Vulkan). Both reach the GPU
partition adapter via `/dev/dxg` ioctls directly — they ship their
own dxcore-equivalent code in `src/microsoft/` so we don't need
`libdxcore.so`/`libd3d12.so`/`libdirectml.so` from a WSL2 install.

## Layout

```
tools/wsl-mesa/
├── README.md
├── build/
│   └── build-mesa.sh                   ← reproduces a build on any 26.04 VM
└── prebuilt/
    └── ubuntu-26.04-amd64/
        ├── wsl-mesa.tar.zst            ← /opt/wsl-mesa tarball
        └── BUILDINFO                   ← Mesa version, source commit, LLVM ver
```

## Install on a Linux VM

```bash
sudo mkdir -p /opt/wsl-mesa
sudo zstd -d wsl-mesa.tar.zst -c | sudo tar -C / -x
echo /opt/wsl-mesa/current/lib/x86_64-linux-gnu | sudo tee /etc/ld.so.conf.d/wsl-mesa.conf
sudo ldconfig
# Vulkan ICD search:
sudo install -d /etc/vulkan/icd.d
sudo ln -sf /opt/wsl-mesa/current/share/vulkan/icd.d/dzn_icd.x86_64.json \
            /etc/vulkan/icd.d/dzn_icd.x86_64.json
```

To **opt in per-process** instead of system-wide:
```bash
export LD_LIBRARY_PATH=/opt/wsl-mesa/current/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
export VK_DRIVER_FILES=/opt/wsl-mesa/current/share/vulkan/icd.d/dzn_icd.x86_64.json
export __GLX_VENDOR_LIBRARY_NAME=mesa
export MESA_LOADER_DRIVER_OVERRIDE=d3d12
```

## Why a separate prefix

Installing to `/opt/wsl-mesa` instead of `/usr` keeps Ubuntu's apt
Mesa intact. `apt upgrade mesa-*` won't clobber our build, and we
won't break apps that need stock Mesa. Switch is via `ld.so.conf.d`
or per-process env vars.

## Versioning

Prebuilt artifacts are keyed on `<ubuntu-codename>-<arch>` since the
binaries are tied to glibc and libstdc++ ABIs, not kernel versions
(unlike dxgkrnl). One subdir per supported distro tuple.

## Rebuilding

Run `build/build-mesa.sh` on a VM with the target codename installed.
Takes ~30 min on a 6-core machine. The script handles deps, source
fetch, meson configure, ninja build, and install.

## Signed graphics updates

Guest updates carry the graphics artifact inside the signed bundle. The
updater verifies the bundle signature and every extracted file before placing
the artifact under `/opt/wsl-mesa/releases/<graphics-version>-<transaction>`;
`/opt/wsl-mesa/current` is then switched atomically. `/opt/wsl-mesa/previous`
is retained for health-check rollback. The systemd-user environment generator
and `appsandbox-gpu` intentionally resolve `current`, so applications never
need to know the active release directory.

The checked-in prebuilt tarball is a provisioning artifact, not proof of the
new 4K60 path: its `BUILDINFO` date/source must be refreshed by a reproducible
Mesa build before shipping a production bundle. The bundle's graphics version,
source commit, LLVM/Mesa build inputs, and artifact digest must be recorded in
the signed manifest. Kernel modules and the host's proprietary graphics stack
are excluded from this userspace artifact.

The production D3D12 share hook discovers the fixed
`/run/appsandbox/display-d3d12.sock` endpoint when the encoder service is
present; no per-session `ASB_MUTTER_D3D12_SHARE_SOCKET` export is required.
The optional variable remains available only as a diagnostic socket override.
