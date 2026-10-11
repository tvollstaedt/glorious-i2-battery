# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Thomas Vollstädt
# HID-BPF battery driver for the Glorious Model I 2 Wireless
#
#   make            build build/0010-Glorious__Model-I2-Wireless.bpf.o
#   make test       attach to the plugged-in receiver/mouse without installing
#   make install    install via udev-hid-bpf (persistent, needs sudo)
#   make uninstall
#
# Requirements: clang, bpftool (package "bpf" on Arch), udev-hid-bpf
# (for install/test), a kernel with CONFIG_HID_BPF and BTF (6.11+ for
# the struct_ops API used here).

NAME     := 0010-Glorious__Model-I2-Wireless
SRC      := bpf/$(NAME).bpf.c
BUILD    := build
OBJ      := $(BUILD)/$(NAME).bpf.o
UNSTRIP  := $(BUILD)/$(NAME).unstripped.bpf.o
VMLINUX  := $(BUILD)/vmlinux.h

CLANG    ?= clang
BPFTOOL  ?= bpftool
UDEV_HID_BPF ?= udev-hid-bpf
ARCH     := $(shell uname -m | sed -e 's/x86_64/x86_64/' -e 's/aarch64/aarch64/')

CFLAGS   := -std=gnu11 -fno-stack-protector -O2 -target bpf -g -c \
            -fms-extensions -Wno-microsoft-anon-tag \
            -D__$(ARCH)__ -I$(BUILD) -Ibpf

all: $(OBJ)

$(BUILD):
	mkdir -p $@

$(VMLINUX): | $(BUILD)
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

$(UNSTRIP): $(SRC) bpf/*.h $(VMLINUX)
	$(CLANG) $(CFLAGS) -o $@ $<

$(OBJ): $(UNSTRIP)
	$(BPFTOOL) gen object $@ $<

# Attach to the plugged-in receiver (interface 1 of 093a:821d). Not persistent.
test: $(OBJ)
	@for dev in /sys/bus/hid/devices/0003:093A:821D.*; do \
		[ -e "$$dev" ] || continue; \
		echo "attaching to $$dev"; \
		sudo $(UDEV_HID_BPF) add "$$dev" "$(abspath $(OBJ))" || true; \
	done
	@sleep 1; ls -d /sys/class/power_supply/hid-*battery* 2>/dev/null || echo "no HID battery yet"

install: $(OBJ)
	sudo $(UDEV_HID_BPF) install --force "$(abspath $(OBJ))"
	sudo udevadm control --reload
	@echo "Re-plug the receiver (or run: sudo udevadm trigger -c add -s hid) to attach."

uninstall:
	-sudo rm -f /etc/udev-hid-bpf/$(NAME).bpf.o /etc/udev/rules.d/*$(NAME)*.rules
	sudo udevadm control --reload

clean:
	rm -rf $(BUILD)

.PHONY: all test install uninstall clean
