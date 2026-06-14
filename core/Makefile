PREFIX  ?= /usr/local
SVCDIR  ?= /usr/lib/systemd/system
POLKIT  ?= /usr/share/polkit-1/actions
CFLAGS  := -O2 $(shell pkg-config --cflags libdrm)
LIBS    := $(shell pkg-config --libs libdrm) -lm

all: kms-hdr

kms-hdr: kms-hdr.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

install: kms-hdr
	install -Dm755 kms-hdr                      $(DESTDIR)$(PREFIX)/bin/kms-hdr
	install -Dm755 hdr-game                     $(DESTDIR)$(PREFIX)/bin/hdr-game
	install -Dm644 kms-hdr.service              $(DESTDIR)$(SVCDIR)/kms-hdr.service
	install -Dm644 kms-hdr.policy               $(DESTDIR)$(POLKIT)/ru.sigmachan.kms-hdr.policy

enable: install
	systemctl daemon-reload
	systemctl enable --now kms-hdr.service

disable:
	systemctl disable --now kms-hdr.service || true

uninstall: disable
	rm -f $(DESTDIR)$(PREFIX)/bin/kms-hdr
	rm -f $(DESTDIR)$(PREFIX)/bin/hdr-game
	rm -f $(DESTDIR)$(SVCDIR)/kms-hdr.service
	rm -f $(DESTDIR)$(POLKIT)/ru.sigmachan.kms-hdr.policy
	systemctl daemon-reload

clean:
	rm -f kms-hdr

.PHONY: all install enable disable uninstall clean
