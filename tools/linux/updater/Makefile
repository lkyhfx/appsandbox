CC ?= gcc
HOST_GO ?= go
HOST_GOOS ?= windows
HOST_GOARCH ?= amd64
CFLAGS ?= -O2 -Wall -Wextra -Werror
LDFLAGS ?=
PREFIX ?= /usr/local
LIBEXECDIR ?= $(PREFIX)/libexec
SYSTEMDDIR ?= /etc/systemd/system

ifndef PUBLIC_KEY_HEX
$(error PUBLIC_KEY_HEX must be provided for production Guest Updater builds)
endif

PUBLIC_KEY_VALID := $(shell printf '%s' '$(PUBLIC_KEY_HEX)' | awk 'length($$0) == 64 && $$0 ~ /^[0-9A-Fa-f]+$$/ { print "yes" }')
ifeq ($(PUBLIC_KEY_VALID),)
$(error PUBLIC_KEY_HEX must be exactly 64 hexadecimal characters)
endif

PUBLIC_KEY_NONZERO := $(shell printf '%s' '$(PUBLIC_KEY_HEX)' | awk '{ s=$$0; gsub(/[0]/,"",s); if (length(s) > 0) print "yes" }')
ifeq ($(PUBLIC_KEY_NONZERO),)
$(error PUBLIC_KEY_HEX must not be all zero)
endif

# RFC 8032 test-vector public keys are public and must never become a release
# trust anchor. Keep this rejection in the build gate as well as in the binary
# so an invalid CI invocation cannot accidentally produce a usable artifact.
KNOWN_TEST_PUBLIC_KEY := d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a
PUBLIC_KEY_NORMALIZED := $(shell printf '%s' '$(PUBLIC_KEY_HEX)' | tr 'A-F' 'a-f')
ifeq ($(PUBLIC_KEY_NORMALIZED),$(KNOWN_TEST_PUBLIC_KEY))
$(error PUBLIC_KEY_HEX is a known RFC/test key and cannot be used)
endif

all: appsandbox-guest-updater

# The Host verifier is built in the release pipeline with the same public key.
# Keep this target separate from the Linux updater so a missing cross compiler
# cannot silently produce a release without the pinned helper.
host-verifier: host/main.go
	GO111MODULE=off GOOS=$(HOST_GOOS) GOARCH=$(HOST_GOARCH) $(HOST_GO) build -trimpath -ldflags '-s -w -X main.releasePublicKeyHex=$(PUBLIC_KEY_HEX)' \
		-o appsandbox-guest-bundle-verifier.exe ./host

appsandbox-guest-updater: appsandbox-guest-updater.c
	$(CC) $(CFLAGS) -DASB_UPDATE_PUBLIC_KEY_HEX=\"$(PUBLIC_KEY_HEX)\" \
		-o $@ $< $(LDFLAGS) -lcrypto

install: appsandbox-guest-updater
	install -d $(DESTDIR)$(LIBEXECDIR) $(DESTDIR)$(SYSTEMDDIR)
	install -m 0755 appsandbox-guest-updater $(DESTDIR)$(LIBEXECDIR)/appsandbox-guest-updater
	install -m 0644 appsandbox-guest-updater.service $(DESTDIR)$(SYSTEMDDIR)/
	install -m 0644 appsandbox-guest-update-watch.service $(DESTDIR)$(SYSTEMDDIR)/

clean:
	rm -f appsandbox-guest-updater

test:
	python3 -m unittest discover -s tests -p 'test_*.py'

.PHONY: all host-verifier install clean test
