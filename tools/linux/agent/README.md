# App Sandbox Linux guest daemons

In-VM Linux daemons that speak the same vsock wire protocol the Windows
App Sandbox host expects. See `docs/linux-idd-implementation-plan.md` in
the repo root for full architecture and wire-protocol reference.

## Daemons (this directory)

| Binary | Channel | Listens on | Privilege | Status |
|---|---|---|---|---|
| `appsandbox-agent` | control | vsock :1 | root (system) | implemented |
| `appsandbox-input` | input | vsock :3 | root (system) | planned |
| `appsandbox-display` | frames + cursor | vsock :2 | root (system) | planned |
| `appsandbox-clipboard` | clipboard | vsock :5, :6 | user (user unit) | planned |

## Build (inside the VM)

```sh
sudo apt-get install -y build-essential
make
```

## Install + enable (inside the VM)

```sh
sudo make enable
```

This installs to `/usr/local/bin/`, drops the systemd unit into
`/etc/systemd/system/`, runs `daemon-reload`, then
`systemctl enable --now appsandbox-agent.service`.

## Smoke test

```sh
systemctl status appsandbox-agent
journalctl -u appsandbox-agent -f
```

You should see `listening on AF_VSOCK port 1`. Once the host-side Phase 1
service-GUID switch lands, the App Sandbox UI's agent dot will turn green
within a few seconds of the unit being active.

## D3D12 native sharing diagnostic

`d3d12-native-share-probe` is an opt-in GPU-PV diagnostic and is not part of
the normal daemon build. It creates two D3D12 devices on one DXCore adapter,
exports a shared texture and fence from the first, opens both native handles
on the second, then verifies fence ordering, BGRA pixel round-trip, and a
second clear/copy cycle using the same resource and fences.
The returned dxg file descriptors are used only with `OpenSharedHandle`; the
probe never treats them as dma-buf or DRM PRIME descriptors.

Inside a Guest that has the official DirectX-Headers development files and
the WSL `libd3d12.so` / `libdxcore.so` runtime:

```sh
make d3d12-native-share-probe
bash gpu-d3d12-native-share-probe.sh results-d3d12-native-share
bash gpu-d3d12-native-share-probe.sh results-d3d12-eventfd --cpu-sync-fallback
```

`--cpu-sync-fallback` is a diagnostic mode: it waits for the producer on the
CPU using a Linux `eventfd` registered with
`ID3D12Fence::SetEventOnCompletion`, so texture pixels and reuse can still be
tested when native shared-fence open is unavailable. The same event-driven
wait checks completion of the consumer readback queue. It deliberately returns
3 even if those resource checks pass, so it cannot be mistaken for full native
resource+fence acceptance.

The Make target deliberately remains outside `all`, so the five production
agents do not acquire a D3D12 runtime dependency.

## Uninstall

```sh
sudo make disable
sudo rm /usr/local/bin/appsandbox-agent /etc/systemd/system/appsandbox-agent.service
sudo systemctl daemon-reload
```
