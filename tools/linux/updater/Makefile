CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror
LDFLAGS ?=
PREFIX ?= /usr/local
LIBEXECDIR ?= $(PREFIX)/libexec
SYSTEMDDIR ?= /etc/systemd/system

PUBLIC_KEY_HEX ?= d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a

all: appsandbox-guest-updater

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

.PHONY: all install clean test
