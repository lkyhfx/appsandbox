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

## D3D12 cross-process 4K60 share probe

`d3d12-cross-process-share-probe` is the next-stage gate for native sharing.
The launcher `fork+exec`s independent producer and consumer processes; the
consumer never inherits a producer D3D12 device. The producer exports three
3840x2160 BGRA8 resources and sends their native dxg descriptors with
`SCM_RIGHTS`. The consumer opens them with its own
`ID3D12Device::OpenSharedHandle()` and both sides close the resource transport
descriptors after the open acknowledgement.

The frame path uses a three-slot ring. Producer readiness and consumer
completion each use `SetEventOnCompletion()` on Linux `eventfd`s, with the
other process waiting through `poll()`. The consumer performs a GPU-only
texture copy into a private default-heap texture. CPU readback is limited to
one diagnostic check every 120 frames. The probe is paced at 60 Hz for 3600
frames and fails on any timeout, resource reopen failure, sequence mismatch,
or sustained-rate failure.

Build and run inside the GPU-PV guest:

```sh
make d3d12-cross-process-share-probe
bash gpu-d3d12-cross-process-share-probe.sh results-d3d12-cross-process
```

Set `GPU_RUNNER=` when launching the binary directly instead of through the
guest's `appsandbox-gpu` wrapper. The expected successful run ends with
`PASS d3d12-cross-process-zero-copy`; the measured log is recorded in
`gpu-d3d12-cross-process-share-probe-results.md`.

## D3D12 shared texture to hardware encode probe

`d3d12-video-encode-probe` has two gates. `--capability` queries
`ID3D12VideoDevice3` for H.264/HEVC, NV12, 3840x2160, and a 60/1 encoder
configuration, then constructs the native video encoder and heap. The default
invocation reuses the validated independent-process `SCM_RIGHTS`/eventfd ring,
performs GPU-only BGRA-to-NV12 conversion, submits D3D12 HEVC encode work, and
runs 3600 frames through a three-slot ring.

```sh
make d3d12-video-encode-probe
bash gpu-d3d12-video-encode-probe.sh results-d3d12-video-encode
```

The measured result belongs in
`gpu-d3d12-video-encode-probe-results.md`. The Guest run passes capability,
cross-process sharing, GPU-only conversion, hardware encode, 4K60 throughput,
and the final `3840x2160`, 3600-frame FFmpeg decode check. The probe supplies
the host-owned VPS/SPS/PPS headers required by D3D12's video-encode contract.
The target Guest does not currently ship FFmpeg, so the runner reports the
decode environment as blocked there; the final stream was decoded on the
development host. Mutter integration can now proceed as a separate isolated
gate, while `appsandbox-display`'s `mmap + send_all()` path remains unchanged.

## Uninstall

```sh
sudo make disable
sudo rm /usr/local/bin/appsandbox-agent /etc/systemd/system/appsandbox-agent.service
sudo systemctl daemon-reload
```
