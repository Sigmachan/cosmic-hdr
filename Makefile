# kms-hdr monorepo — builds the C injector + the Rust workspace and installs everything.
PREFIX  ?= /usr/local
APPS    ?= /usr/share/applications
CARGO   ?= cargo

.PHONY: all c rust install enable disable uninstall clean

all: c rust

c:
	$(MAKE) -C core

rust:
	$(CARGO) build --release

install: all
	$(MAKE) -C core install PREFIX=$(PREFIX)
	install -Dm755 target/release/kms-hdr-cal $(DESTDIR)$(PREFIX)/bin/kms-hdr-cal
	install -Dm644 core/kms-hdr-cal.desktop   $(DESTDIR)$(APPS)/ru.sigmachan.KmsHdrCal.desktop

enable: install
	$(MAKE) -C core enable

disable:
	$(MAKE) -C core disable

uninstall:
	$(MAKE) -C core uninstall PREFIX=$(PREFIX)
	rm -f $(DESTDIR)$(PREFIX)/bin/kms-hdr-cal
	rm -f $(DESTDIR)$(APPS)/ru.sigmachan.KmsHdrCal.desktop

clean:
	$(MAKE) -C core clean
	$(CARGO) clean
