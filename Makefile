CC ?= gcc
CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -Werror -Wno-format-truncation -D_GNU_SOURCE
LDFLAGS ?=
PREFIX ?= /usr/local
VERSION := 0.1.0

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/%.o)

all: lcr

lcr: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: src/%.c src/lcr.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

install: lcr
	install -D -m 0755 lcr $(DESTDIR)$(PREFIX)/bin/lcr

test: lcr
	tests/run.sh

bench: lcr
	bench/run.sh

deb: lcr
	rm -rf build/deb
	install -D -m 0755 lcr build/deb/usr/bin/lcr
	mkdir -p build/deb/DEBIAN
	printf 'Package: lcr\nVersion: $(VERSION)\nSection: admin\nPriority: optional\nArchitecture: amd64\nDepends: nftables\nMaintainer: Adwitiya Shukla <shuklaaw@mail.uc.edu>\nDescription: small container runtime for Ubuntu\n Namespaces, cgroups, overlayfs, seccomp and veth networking in one C binary with no library dependencies.\n' > build/deb/DEBIAN/control
	dpkg-deb --root-owner-group --build build/deb build/lcr_$(VERSION)_amd64.deb

clean:
	rm -rf build lcr

.PHONY: all install test bench deb clean
