# Gate C0: primary framebuffer provenance

## Result

```text
Gate C0: PASS
C.provenance: LOCAL_SHMEM
C.import_attach: NULL
C.exporter: none
C.dxg_identity: NO
```

Across the complete test run, the `asb_drm` primary plane processed 5,200
atomic updates:

```text
local=5200 imported=0 disabled=0 changes=5200
gem_funcs=drm_gem_shmem_funcs
import_attach=NULL
```

The current Mutter primary framebuffer is therefore a local `asb_drm` GEM
shmem object, not a PRIME-imported dma-buf. It has no exporter and no observed
dxg resource identity. The present primary-FB-to-VAIL/host-handle research path
should stop here; changing workloads does not turn the KMS scanout object into
a dxg allocation.

This does not say that the applications avoided GPU rendering. In particular,
the Vulkan workload used Mesa `dzn` on the NVIDIA RTX 4070. It says that the
final Mutter composition/scanout boundary still landed in local shmem.

## Runtime environment

- Guest: `ubuntu1`
- Kernel: `7.0.0-30-generic`
- Desktop/compositor: GNOME Shell / Mutter, Wayland
- Mode: 1920x1080, XRGB8888 (`0x34325258`), linear modifier
- Pitch: 7680 bytes
- GEM size: 8,355,840 bytes
- OpenGL renderer: llvmpipe, direct rendering reported as enabled but not
  hardware accelerated
- Vulkan renderer: Mesa `dzn`, Microsoft Direct3D12, NVIDIA GeForce RTX 4070

## Instrumentation

`asb_primary_atomic_update()` now observes the framebuffer accepted by each
atomic commit and records:

- sequence, framebuffer id, dimensions, fourcc, modifier, pitch and offset;
- GEM object identity, size and function table;
- `import_attach`, dma-buf pointer, exporter name and dma-buf size;
- LOCAL/IMPORTED classification and creation path;
- whether the GEM object identity changed;
- cumulative local/imported/disabled/identity-change counts.

Tracing is disabled by default and controlled at runtime with:

```text
/sys/module/asb_drm/parameters/c0_trace
/sys/module/asb_drm/parameters/c0_sample_every
```

The live test module was loaded with per-update counting. The second matrix
logged every tenth update so that all workload boundary markers remained in
the kernel ring buffer. Tracing was disabled after collection.

## Workload matrix

The table below is generated from the last complete matrix in
`gate-c0-kernel-sampled.log`. Approximate update counts use adjacent sampled
sequence values and therefore have a maximum boundary error of nine updates.

| Workload | Approx. atomic updates | Samples | Provenance | GEM objects observed | FB IDs observed |
|---|---:|---:|---|---:|---:|
| GNOME idle, 10 s | 0 | 0 | no new commit | 0 | 0 |
| Moving a live GLX window 360 times | 453 | 46 | LOCAL 46/46 | 3 | 3 |
| GLX gears, 12 s | 710 | 71 | LOCAL 71/71 | 3 | 2 |
| Vulkan cube (`dzn`), 12 s | 700 | 70 | LOCAL 70/70 | 3 | 1 |
| 60fps GStreamer video test, 12 s | 720 | 72 | LOCAL 72/72 | 3 | 1 |

The workload matrix contains 259 sampled commits and no imported dma-buf.
Three GEM object addresses recur, establishing a three-buffer rotation rather
than a single-frame observation. The lifetime counters cover both full runs
and compositor startup and likewise contain zero imported commits.

## Interpretation

`import_attach == NULL` by itself only proves that the object was not obtained
through PRIME dma-buf import. Here it is combined with two additional facts:

1. Runtime `gem_funcs` resolves to `drm_gem_shmem_funcs` on every sample.
2. `asb_drm` installs `DRM_GEM_SHMEM_DRIVER_OPS` and has no custom GEM
   allocation callback.

Together these establish `LOCAL_SHMEM`, not merely `NOT_IMPORTED`.

The resulting architecture is:

```text
application GL/Vulkan/video rendering
  -> Mutter composition
  -> local asb_drm GEM shmem buffers (three-buffer rotation)
  -> primary KMS framebuffer
  -> appsandbox-display mmap/read + transport
```

Consequences:

- There is no current primary framebuffer dxg handle to pass to
  `D3DKMTShareObjectWithHost`.
- Gate B0 remains independently blocked on VAIL client identity, but it is no
  longer the first blocker for the current framebuffer.
- Work on direct GPU-allocation display sharing must move upstream to the
  compositor/GBM/KMS allocation architecture.
- Gate A remains useful as an independent proof that an arbitrary guest D3D12
  allocation can or cannot enter the GPU-PV host-sharing machinery.

## Reproduction artifacts

- `asb_drm/asb_drm_plane.c`: authoritative atomic-update instrumentation.
- `run-gate-c0.sh`: repeatable five-workload matrix.
- `analyze-gate-c0.py`: parser and per-workload summary.
- `gate-c0-kernel-sampled.log`: raw kernel evidence from the complete sampled
  matrix.

The Guest remains online with GNOME, `appsandbox-display`, and
`appsandbox-agent` active. The instrumented module was loaded only from a
temporary directory and was not installed over the boot module; the original
module will return on reboot.
