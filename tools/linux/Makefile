# tools/linux/Makefile
#
# Top-level orchestrator for everything that has to be built on a Linux box
# for AppSandbox to support Linux guests: the 5 agent daemons (userspace
# ELFs) plus the two out-of-tree kernel modules (dxgkrnl and asb_drm).
#
# Workflow
# --------
#
# 1. Clone the appsandbox repo onto a Linux machine that has matching kernel
#    headers installed (e.g. Ubuntu 26.04 with linux-headers-generic).
# 2. cd tools/linux
# 3. make PUBLIC_KEY_HEX=<release Ed25519 public key hex>
# 4. Build artifacts land under tools/linux/dist/ — gitignored, this dir is
#    only valid on this machine for this kernel.
# 5. Copy dist/ over to your Windows host's appsandbox checkout, into
#    release/resources/linux/. Final instructions are printed at the end of
#    a successful build.
# 6. On the Windows host, rebuild AppSandbox.sln. The existing PostBuildEvent
#    on AppSandbox.vcxproj xcopies release/resources/ -> bin/<cfg>/resources/,
#    so the Linux tree comes along for the ride.
#
# Targets
# -------
#   make           agents + modules + install to dist/   (default)
#   make compile   build only, don't populate dist/
#   make agents    build only the userspace daemons
#   make modules   build only the kernel modules
#   make clean     wipe build outputs (object files, binaries, .ko)
#   make distclean clean + remove dist/

KVER  := $(shell uname -r)
ARCH  := $(shell uname -m)
DISTDIR := $(CURDIR)/dist
GUEST_RUNTIME_VERSION ?= 1.0.0

.PHONY: all compile agents updater modules dxgkrnl asb_drm install-agents install-modules \
        validate-graphics-artifact clean distclean copy-help

all: compile install-agents install-modules copy-help

compile: agents updater modules

# ---- userspace daemons ------------------------------------------------------

# Re-uses the existing tools/linux/agent/Makefile which knows how to compile
# each daemon with the right libs (libdrm for display, libasound for audio,
# libxcb for clipboard).
agents:
	$(MAKE) -C agent
	@if pkg-config --exists DirectX-Headers 2>/dev/null && \
	    [ -f /opt/appsandbox/wsl-deps/libd3d12.so ]; then \
		$(MAKE) -C agent d3d12-helper; \
	else \
		echo "  D3D12 helper skipped (DirectX-Headers/runtime unavailable)"; \
	fi

updater:
	$(MAKE) -C updater PUBLIC_KEY_HEX=$(PUBLIC_KEY_HEX)
	$(MAKE) -C updater host-verifier PUBLIC_KEY_HEX=$(PUBLIC_KEY_HEX)

install-agents: agents updater validate-graphics-artifact
	@install -d $(DISTDIR)
	@for bin in appsandbox-agent appsandbox-audio appsandbox-clipboard \
	            appsandbox-display appsandbox-input; do \
	    if [ -f agent/$$bin ]; then \
	        install -m 0755 agent/$$bin $(DISTDIR)/; \
	        echo "  installed $(DISTDIR)/$$bin"; \
	    fi; \
	done
	@if [ -f agent/appsandbox-display-d3d12 ]; then \
		install -d $(DISTDIR)/libexec; \
		install -m 0755 agent/appsandbox-display-d3d12 $(DISTDIR)/libexec/; \
		echo "  installed $(DISTDIR)/libexec/appsandbox-display-d3d12"; \
	fi
	install -d $(DISTDIR)/libexec
	install -m 0755 updater/appsandbox-guest-updater $(DISTDIR)/libexec/
	install -m 0644 updater/appsandbox-guest-updater.service $(DISTDIR)/
	install -m 0644 updater/appsandbox-guest-update-watch.service $(DISTDIR)/
	@printf '%s\n' '$(PUBLIC_KEY_HEX)' > $(DISTDIR)/trusted-public-key.hex
	@printf 'version=%s\n' '$(GUEST_RUNTIME_VERSION)' > $(DISTDIR)/guest-runtime.version
	install -d $(DISTDIR)/gnome/appsandbox-pointer@appsandbox
	install -m 0644 agent/gnome/appsandbox-pointer@appsandbox/*.json agent/gnome/appsandbox-pointer@appsandbox/*.js $(DISTDIR)/gnome/appsandbox-pointer@appsandbox/

	# Also emit the exact closed-world tree consumed by Windows production
	# provisioning. Keep the legacy flat dist outputs above for developer
	# workflows, but never make provisioning infer artifacts from them.
	install -d $(DISTDIR)/agent-src $(DISTDIR)/asb_drm-src \
	           $(DISTDIR)/dxgkrnl-src $(DISTDIR)/systemd $(DISTDIR)/updater
	cp -a agent/. $(DISTDIR)/agent-src/
	cp -a asb_drm/. $(DISTDIR)/asb_drm-src/
	cp -a dxgkrnl/src/. $(DISTDIR)/dxgkrnl-src/
	install -m 0644 agent/systemd/*.service $(DISTDIR)/systemd/
	install -m 0644 updater/appsandbox-guest-updater.service \
	           updater/appsandbox-guest-update-watch.service $(DISTDIR)/systemd/
	install -m 0644 asb_drm/systemd-asb-evict-simpledrm.service \
	           $(DISTDIR)/systemd/asb-evict-simpledrm.service
	install -m 0644 agent/modules-load.d-snd-aloop.conf \
	           $(DISTDIR)/systemd/modules-load.d-snd-aloop.conf
	install -m 0644 asb_drm/modules-load.d-asb_drm.conf \
	           $(DISTDIR)/systemd/modules-load.d-asb_drm.conf
	install -m 0755 updater/appsandbox-guest-updater $(DISTDIR)/updater/
	install -m 0755 updater/appsandbox-guest-bundle-verifier.exe $(DISTDIR)/updater/
	install -m 0644 updater/appsandbox-guest-updater.service \
	           updater/appsandbox-guest-update-watch.service $(DISTDIR)/updater/
	sha256sum updater/appsandbox-guest-updater > \
	           $(DISTDIR)/updater/appsandbox-guest-updater.sha256
	sha256sum updater/appsandbox-guest-bundle-verifier.exe > \
	           $(DISTDIR)/updater/appsandbox-guest-bundle-verifier.exe.sha256
	install -m 0644 $(DISTDIR)/trusted-public-key.hex \
	           $(DISTDIR)/updater/trusted-public-key.hex
	install -m 0644 asb_drm/modprobe.d-asb_drm.conf \
	           $(DISTDIR)/modprobe.d-asb_drm.conf
	install -m 0644 wsl-mesa/50-appsandbox-gpu \
	           wsl-mesa/org.gnome.Shell-no-gpu.conf wsl-mesa/appsandbox-gpu $(DISTDIR)/
	install -m 0644 wsl-mesa/prebuilt/ubuntu-26.04-amd64/BUILDINFO \
	           $(DISTDIR)/wsl-mesa.BUILDINFO
	install -m 0644 wsl-mesa/prebuilt/ubuntu-26.04-amd64/wsl-mesa.tar.zst \
	           $(DISTDIR)/wsl-mesa.tar.zst

	# The offline migration resource is copied into the closed-world tree so
	# Host migration never has to fetch or synthesize a control agent.
	install -m 0755 updater/offline_legacy_bootstrap.py \
	           $(DISTDIR)/offline_legacy_bootstrap.py
	install -d $(DISTDIR)/bootstrap-runtime/bin \
	           $(DISTDIR)/bootstrap-runtime/libexec \
	           $(DISTDIR)/bootstrap-runtime/systemd \
	           $(DISTDIR)/bootstrap-runtime/gnome \
	           $(DISTDIR)/bootstrap-runtime/config
	for bin in appsandbox-agent appsandbox-audio appsandbox-clipboard \
	           appsandbox-display appsandbox-input; do \
	    install -m 0755 agent/$$bin $(DISTDIR)/bootstrap-runtime/bin/; \
	done
	@if [ -f agent/appsandbox-display-d3d12 ]; then \
	    install -m 0755 agent/appsandbox-display-d3d12 \
	        $(DISTDIR)/bootstrap-runtime/libexec/; \
	fi
	install -m 0755 updater/appsandbox-guest-updater \
	           $(DISTDIR)/bootstrap-runtime/libexec/
	install -m 0644 agent/systemd/*.service updater/*.service \
	           $(DISTDIR)/bootstrap-runtime/systemd/
	cp -a agent/gnome/. $(DISTDIR)/bootstrap-runtime/gnome/
	printf 'version=%s\ncommit=bootstrap-runtime\n' \
	           '$(GUEST_RUNTIME_VERSION)' > $(DISTDIR)/bootstrap-runtime/RELEASE
	install -m 0644 wsl-mesa/50-appsandbox-gpu \
	           $(DISTDIR)/bootstrap-runtime/config/50-appsandbox-gpu
	install -m 0644 wsl-mesa/org.gnome.Shell-no-gpu.conf \
	           $(DISTDIR)/bootstrap-runtime/config/org.gnome.Shell-no-gpu.conf
	install -m 0755 wsl-mesa/appsandbox-gpu \
	           $(DISTDIR)/bootstrap-runtime/config/appsandbox-gpu
	install -m 0644 asb_drm/modprobe.d-asb_drm.conf \
	           $(DISTDIR)/bootstrap-runtime/config/asb_drm.conf

	# release-trust.json is the machine-readable trust boundary consumed by
	# make-release.ps1. The Windows release step rewrites the two binary hashes
	# after EV signing and refuses to package an inconsistent copy.
	@printf '{\n  "schema": 1,\n  "public_key_hex": "%s",\n  "guest_runtime_version": "%s",\n  "updater_sha256": "%s",\n  "verifier_sha256": "%s",\n  "bootstrap_updater_sha256": "%s",\n  "bundle_verifier_sha256": "%s",\n  "graphics_artifact_sha256": "%s",\n  "graphics_production_4k60": true\n}\n' \
		'$(PUBLIC_KEY_HEX)' '$(GUEST_RUNTIME_VERSION)' \
		"$$(cut -d' ' -f1 $(DISTDIR)/updater/appsandbox-guest-updater.sha256)" \
		"$$(cut -d' ' -f1 $(DISTDIR)/updater/appsandbox-guest-bundle-verifier.exe.sha256)" \
		"$$(cut -d' ' -f1 $(DISTDIR)/updater/appsandbox-guest-updater.sha256)" \
		"$$(cut -d' ' -f1 $(DISTDIR)/updater/appsandbox-guest-bundle-verifier.exe.sha256)" \
		"$$(sha256sum $(DISTDIR)/wsl-mesa.tar.zst | cut -d' ' -f1)" \
		> $(DISTDIR)/updater/release-trust.json

# Production release artifacts must describe an actual Mesa/Mutter/D3D12
# 4K60 path. A legacy/probe-only prebuilt is intentionally rejected here and
# by the Windows release gate.
validate-graphics-artifact:
	@test -f wsl-mesa/prebuilt/ubuntu-26.04-amd64/BUILDINFO
	@grep -Eq '^production-4k60: true$$' \
		wsl-mesa/prebuilt/ubuntu-26.04-amd64/BUILDINFO
	@grep -Eq '^mesa-source-commit: [0-9a-f]{40}$$' \
		wsl-mesa/prebuilt/ubuntu-26.04-amd64/BUILDINFO
	@grep -Eq '^mesa-patchset-version: [0-9][0-9A-Za-z._-]*$$' \
		wsl-mesa/prebuilt/ubuntu-26.04-amd64/BUILDINFO
	@actual=$$(sha256sum wsl-mesa/prebuilt/ubuntu-26.04-amd64/wsl-mesa.tar.zst | cut -d' ' -f1); \
		expected=$$(sed -n 's/^artifact-sha256: //p' \
		wsl-mesa/prebuilt/ubuntu-26.04-amd64/BUILDINFO); \
		test -n "$$expected" && test "$$actual" = "$$expected"

# ---- kernel modules ---------------------------------------------------------
#
# Both modules use the standard out-of-tree kbuild idiom (`make -C
# /lib/modules/<kver>/build M=<src>`). We don't go through DKMS here —
# DKMS is a runtime concern handled by setup.sh inside the guest VM. Here
# we just produce a .ko for the current kernel.

modules: dxgkrnl asb_drm

dxgkrnl:
	$(MAKE) -C /lib/modules/$(KVER)/build M=$(CURDIR)/dxgkrnl/src modules

asb_drm:
	$(MAKE) -C /lib/modules/$(KVER)/build M=$(CURDIR)/asb_drm modules

install-modules: modules
	@install -d $(DISTDIR)/modules/$(KVER)
	@if [ -f dxgkrnl/src/dxgkrnl.ko ]; then \
	    install -m 0644 dxgkrnl/src/dxgkrnl.ko $(DISTDIR)/modules/$(KVER)/; \
	    echo "  installed $(DISTDIR)/modules/$(KVER)/dxgkrnl.ko"; \
	fi
	@if [ -f asb_drm/asb_drm.ko ]; then \
	    install -m 0644 asb_drm/asb_drm.ko $(DISTDIR)/modules/$(KVER)/; \
	    echo "  installed $(DISTDIR)/modules/$(KVER)/asb_drm.ko"; \
	fi

# ---- cleanup ----------------------------------------------------------------

clean:
	-$(MAKE) -C agent clean
	-$(MAKE) -C /lib/modules/$(KVER)/build M=$(CURDIR)/dxgkrnl/src clean
	-$(MAKE) -C /lib/modules/$(KVER)/build M=$(CURDIR)/asb_drm clean

distclean: clean
	rm -rf $(DISTDIR)

# ---- copy-to-Windows reminder ----------------------------------------------

copy-help:
	@printf '\n'
	@printf '================================================================\n'
	@printf 'Build complete.  Artifacts staged in:\n'
	@printf '    %s\n' '$(DISTDIR)'
	@printf '\n'
	@printf 'Next step (manual): get this dist/ tree onto your Windows host\n'
	@printf 'and drop the contents at:\n'
	@printf '    <appsandbox>/release/resources/linux/\n'
	@printf '\n'
	@printf 'A simple tarball is fine; transfer it by whatever means you\n'
	@printf 'normally use to move files between this machine and the host\n'
	@printf '(USB stick, shared drive, file-share VM, scp, etc.):\n'
	@printf '\n'
	@printf '  tar -C %s -czf ~/asb-linux.tar.gz .\n' '$(DISTDIR)'
	@printf '\n'
	@printf 'Then on the Windows host, in your appsandbox checkout, extract\n'
	@printf 'over release/resources/linux/ (creating the dir if needed).\n'
	@printf '\n'
	@printf 'After copying, rebuild AppSandbox.sln (Release).  The PostBuildEvent\n'
	@printf 'on AppSandbox.vcxproj xcopies release/resources/ into\n'
	@printf 'bin/<cfg>/resources/, so the Linux artifacts travel to the runtime\n'
	@printf 'location automatically.\n'
	@printf '================================================================\n'
