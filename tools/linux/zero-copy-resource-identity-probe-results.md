# Zero-copy resource identity probe: Gate B0 results

## Conclusion

Gate B0 is a real architectural blocker for the current AppSandbox IDD-only
display design. `D3DKMTRegisterVailProcess` consistently returns
`0xC0000022 (STATUS_ACCESS_DENIED)` even after testing all locally controllable
documented prerequisites and the HCS/RDP hypotheses below.

This is not fixed by elevation, `VideoMonitor.ConnectionOptions.AccessSids`, a
BasicSession named pipe, an active embedded RDP client, or creating a D3D12
graphics context. The earlier proposed HCS configuration change has therefore
been reverted; it did not authorize VAIL registration and unnecessarily
published an RDP endpoint.

Do not proceed to Gate A/B under the assumption that a guest-returned
`hVailProcessNtHandle` will target an ordinary AppSandbox process. A different
host architecture is required first: either code integrated with the Windows
RDP/VAIL client that Windows recognizes for this VM, or a transport that does
not depend on `D3DKMTShareObjectWithHost`.

## Environment

- VM: `ubuntu1`
- GPU mode: `1` (`Default GPU`)
- RuntimeId used: `af0dcd1a-a0fd-59a0-998e-cad0c3834a2c`
- Interactive user SID: `S-1-5-21-3427680875-3818661830-1318789199-1001`
- Normal integrity: medium (`0x2000`)
- Windows probe API: `D3DKMTRegisterVailProcess`

## Revalidation matrix

| Test | Result |
|---|---|
| Original standalone probe under the Codex sandbox token | `STATUS_ACCESS_DENIED` |
| Standalone probe under the real interactive user token | `STATUS_ACCESS_DENIED` |
| Same probe elevated | `STATUS_ACCESS_DENIED` |
| VM recreated with `AccessSids` only | `STATUS_ACCESS_DENIED` |
| VM recreated with `NamedPipe` + `AccessSids` | `STATUS_ACCESS_DENIED` |
| Same process after BasicSession RDP ActiveX connected | `STATUS_ACCESS_DENIED` on 30 retries |
| Same process with a successfully created D3D12 device | `STATUS_ACCESS_DENIED` |
| Separate same-token process, with D3D12 device, while RDP remained connected | `STATUS_ACCESS_DENIED` |
| Control: VM without the required VAIL/vGPU state | `STATUS_GRAPHICS_VAIL_STATE_CHANGED` |

The active-client probe logged both successful D3D12 initialization and RDP
connection before the final control process ran:

```text
B0.graphics_context: hr=0x00000000 created=1
rdp: RDP event: Connected "ubuntu1" (Basic)
probe=rdp-vail-context-probe vm=ubuntu1 pid=37440
B0.graphics_context: hr=0x00000000 created=1
B0.child_register_vail: status=0xC0000022
```

The child-process control rules out the possibility that only the parent was
rejected because the embedded ActiveX control had already registered it.

## What the result means

Microsoft documents two relevant requirements:

1. The VM must have a vGPU.
2. The caller must have the same privileges as the RDP client process created
   for that VM instance.

`AccessSids` controls access to the HCS VideoMonitor connection endpoint. It
does not make an arbitrary process the RDP/VAIL client recognized by the
graphics kernel. AppSandbox's legacy `RdpBase.dll` + mstsc ActiveX BasicSession
viewer can connect to the endpoint, but that is insufficient to satisfy the
VAIL registration security check.

The exact private mechanism by which Windows associates a Microsoft RDP client
with the VM is not exposed by the public HCS schema or the public
`D3DKMTRegisterVailProcess` contract. WSLg uses the Microsoft `msrdc.exe`
client, VM RuntimeId, an HV-socket service, and a WSL DVC plugin; that is a
substantially different architecture from AppSandbox's IDD/vsock display path.

References:

- Microsoft documents the return codes and RDP-client privilege requirement:
  https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtregistervailprocess
- Microsoft's HCS schema defines `NamedPipe` and `AccessSids` as RDP connection
  options, not as VAIL-process registration:
  https://github.com/microsoft/hcsshim/blob/main/internal/hcs/schema2/video_monitor.go
- WSLg launches `msrdc.exe` with the VM id, HV-socket service id, and a WSL DVC
  plugin:
  https://github.com/microsoft/wslg/blob/main/WSLGd/main.cpp

## Recommended handling

### Route 1: RDP-integrated prototype

If direct GPU-allocation identity is still mandatory, first build a minimal
prototype around the Microsoft RDP client model (VM identity + transport + DVC
plugin) and rerun registration/guest sharing there. Treat this as a separate
research track: the public API does not promise that a custom RDP ActiveX host
can acquire the required VAIL identity.

The decisive PASS is not merely another B0 call. It is a coupled test:

```text
recognized RDP/VAIL host process
  -> guest D3DKMTShareObjectWithHost succeeds
  -> returned hVailProcessNtHandle is valid in that host process
  -> ID3D12Device::OpenSharedHandle succeeds
```

### Route 2: Remove VAIL from the direct-display design

Keep IDD as the display path and use an explicitly provisioned shared-memory
transport instead of `D3DKMTShareObjectWithHost`. This can remove the current
socket pixel copy, but it is not automatically GPU-allocation zero-copy; the
GPU/CPU copy boundary must be measured and documented separately.

### Route 3: Vendor-supported interface

For a production-quality arbitrary-process GPU allocation bridge, obtain a
documented/supported contract from Microsoft. A privileged broker alone is not
supported by the evidence: both medium and elevated processes were denied.

## Artifacts

- `tools/probes/dxg-host-open-probe/dxg-host-open-probe.c`: standalone B0 and
  identity diagnostics.
- `tools/probes/dxg-host-open-probe/rdp-vail-context-probe.c`: D3D12 + embedded
  BasicSession + same-token child-process control.
- `tools/probes/dxg-host-open-probe/hcs-vm-shutdown.c`: graceful HCS shutdown
  helper used during configuration recreation tests.

## Gate status

```text
B0 (ordinary AppSandbox/IDD process): BLOCKED — reproducible STATUS_ACCESS_DENIED
Gate A:                              NOT RUN (B0 prerequisite unavailable)
Gate B:                              NOT RUN
Gate C0:                             PASS — LOCAL_SHMEM, dxg identity absent
```
