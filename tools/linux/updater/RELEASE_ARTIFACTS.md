# Production Guest Update artifacts

The Windows Host release must ship this closed-world tree under
`resources/linux/`:

```text
agent-src/
asb_drm-src/
dxgkrnl-src/
systemd/
guest-runtime.version
offline_legacy_bootstrap.py
bootstrap-runtime/
updater/
  appsandbox-guest-updater
  appsandbox-guest-updater.sha256
  appsandbox-guest-updater.service
  appsandbox-guest-update-watch.service
  trusted-public-key.hex
  appsandbox-guest-bundle-verifier.exe
  appsandbox-guest-bundle-verifier.exe.sha256
  release-trust.json
modprobe.d-asb_drm.conf
50-appsandbox-gpu
org.gnome.Shell-no-gpu.conf
appsandbox-gpu
wsl-mesa.BUILDINFO
wsl-mesa.tar.zst
```

`tools/linux/Makefile install-agents` emits this layout under `dist/`; copy
that directory into the Host release's `resources/linux/` tree. Kernel modules
are deliberately not part of this normal runtime resource set.

The updater and the Host verifier are built with the same `PUBLIC_KEY_HEX`
supplied by the release pipeline. The verifier is built from
`updater/host/main.go` with `GOOS=windows GOARCH=amd64`; its `.exe` is never
accepted unless the generated sidecar matches the bytes that were packaged.
The final `release-trust.json` binds that public key to the exact updater,
verifier, graphics artifact, and guest runtime version. There is no repository
fallback key and the private signing key must remain outside both the repository
and the guest image. The graphics `BUILDINFO` must identify a real production
Mesa/Mutter 4K60 build; legacy/probe-only artifacts are rejected.

The same release pipeline must compile the Host with
`ASB_BOOTSTRAP_UPDATER_SHA256` and `ASB_BUNDLE_VERIFIER_SHA256` set to the
lowercase SHA-256 digests of the corresponding artifacts. Empty or missing
definitions intentionally make Guest Update fail closed; the `.sha256`
sidecars are packaging consistency checks, not trust anchors.

`appsandbox-guest-bundle-verifier.exe` is a pinned Host-side release helper.
Its fixed interface is:

```text
appsandbox-guest-bundle-verifier.exe --verify-bundle <bundle.tar.zst> <sha256>
```

It exits zero only after it has independently verified the exact bundle bytes,
the detached Ed25519 signature using the same release public key, and the
manifest policy: schema, `amd64`, `ubuntu-26.04`, Host protocol range,
`updater_min_version`, and `kernel_components_present=false`. The Host checks
the helper's `.sha256` sidecar before executing it and passes the digest that
will be used for the VSOCK transfer, preventing a preflight/transfer swap from
being accepted. The Guest repeats all signature, compatibility, archive, and
payload checks after transfer.

The old `--prefetch-repo --branch` flow is developer-only. Production VM
creation consumes this exact Host release tree and fails closed if a required
artifact is absent. A legacy guest is first stopped and migrated offline by
`offline_legacy_bootstrap.py`; the new control agent is installed under
`bootstrap-runtime/`, while the original runtime is preserved at
`releases/legacy-original` for rollback.
