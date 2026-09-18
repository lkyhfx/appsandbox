# App Sandbox Linux guest daemons — builds the in-VM binaries that match
# the wire protocol the Windows host stack expects. See
# docs/linux-idd-implementation-plan.md for architecture.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -pthread -D_GNU_SOURCE
LDFLAGS ?=

PREFIX        ?= /usr/local
DESTDIR       ?=
BINDIR        := $(PREFIX)/bin
SYSTEMDDIR    := /etc/systemd/system
USERUNITDIR   := /etc/systemd/user
POINTER_EXT   := appsandbox-pointer@appsandbox
EXTENSIONDIR  := /usr/share/gnome-shell/extensions/$(POINTER_EXT)

# Daemons get added here as each phase lands.
BINS := appsandbox-agent appsandbox-display appsandbox-input appsandbox-audio appsandbox-clipboard

SYSTEM_UNITS := systemd/appsandbox-agent.service \
                systemd/appsandbox-display.service \
                systemd/appsandbox-input.service \
                systemd/appsandbox-audio.service
# No clipboard systemd unit — the agent owns the clipboard helper's
# lifecycle (spawn_clipboard_helper in appsandbox-agent.c). Same
# pattern as the Windows agent which spawns clipboard.exe /
# clipboard-reader.exe.
USER_UNITS   :=

all: $(BINS)

appsandbox-agent: appsandbox-agent.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Display daemon needs libdrm for drmModeGetFB2 + drmPrimeHandleToFD.
appsandbox-display: appsandbox-display.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags libdrm) -o $@ $< \
	       $(LDFLAGS) $(shell pkg-config --libs libdrm)

appsandbox-input: appsandbox-input.c $(wildcard protocol.h ../../../src/core/protocol.h)
	$(CC) $(CFLAGS) -I../../../src/core -o $@ $< $(LDFLAGS) -lsystemd

# Audio daemon uses libasound to talk to the kernel snd-aloop driver.
appsandbox-audio: appsandbox-audio.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lasound

# Clipboard daemon: XCB (with XFIXES) for X11 selection ownership,
# libc iconv for text format conversion. libpng would be added when
# we wire the DIB ↔ PNG path; today CF_DIB → image/bmp is header-munge.
appsandbox-clipboard: appsandbox-clipboard.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lxcb -lxcb-xfixes

install-pointer:
	install -d $(DESTDIR)$(EXTENSIONDIR)
	install -m 0644 gnome/$(POINTER_EXT)/metadata.json gnome/$(POINTER_EXT)/extension.js $(DESTDIR)$(EXTENSIONDIR)/

install: $(BINS) install-pointer
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BINS) $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(SYSTEMDDIR)
	for u in $(SYSTEM_UNITS); do \
	    install -m 0644 $$u $(DESTDIR)$(SYSTEMDDIR)/; \
	done
	@if [ -n "$(USER_UNITS)" ]; then \
	    install -d $(DESTDIR)$(USERUNITDIR); \
	    for u in $(USER_UNITS); do \
	        install -m 0644 $$u $(DESTDIR)$(USERUNITDIR)/; \
	    done; \
	fi

# Convenience: install + enable all units in one shot. Requires root.
enable: install
	systemctl daemon-reload
	systemctl enable --now appsandbox-agent.service
	systemctl enable --now appsandbox-display.service
	systemctl enable --now appsandbox-input.service

disable:
	-systemctl disable --now appsandbox-input.service
	-systemctl disable --now appsandbox-display.service
	-systemctl disable --now appsandbox-agent.service

clean:
	rm -f $(BINS) d3d12-native-share-probe d3d12-cross-process-share-probe

# Diagnostic only: requires Microsoft's WSL libd3d12/libdxcore runtime and
# the official DirectX-Headers development package.  Keep it out of `all` so
# normal agent builds do not gain a proprietary runtime dependency.
d3d12-native-share-probe: d3d12-native-share-probe.cpp
	$(CXX) -std=c++17 $(CXXFLAGS) -D_WIN32_WINNT=0x0A00 -O2 -Wall -Wextra \
	       -Werror -Wno-unknown-pragmas -o $@ $< \
	       $$(pkg-config --cflags --libs DirectX-Headers) -ld3d12 -ldxcore

# Cross-process diagnostic: producer and consumer are fork+exec children and
# exchange native dxg resource/event descriptors with SCM_RIGHTS.  Keep this
# out of `all`; it requires the same opt-in WSL D3D12 runtime as the probe
# above, plus a 4K render/copy workload.
d3d12-cross-process-share-probe: d3d12-cross-process-share-probe.cpp
	$(CXX) -std=c++17 $(CXXFLAGS) -D_GNU_SOURCE -D_WIN32_WINNT=0x0A00 \
	       -O2 -Wall -Wextra -Werror -Wno-unknown-pragmas -o $@ $< \
	       $$(pkg-config --cflags --libs DirectX-Headers) -ld3d12 -ldxcore

.PHONY: all install install-pointer enable disable clean \
	d3d12-native-share-probe d3d12-cross-process-share-probe
