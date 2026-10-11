#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Thomas Vollstädt
"""Exercise the HID-BPF battery driver against a virtual (uhid) Model I 2.

Creates a uhid device with the receiver's VID/PID and the original 180-byte
report descriptor of interface 1, attaches the compiled BPF object with
udev-hid-bpf, then injects report-6 frames and checks the power_supply the
kernel created from the rewritten descriptor.

Must run as root (uhid and BPF loading):

    sudo tools/uhid-test.py build/0010-Glorious__Model-I2-Wireless.bpf.o
"""

import glob
import os
import shutil
import struct
import subprocess
import sys
import time

# Interface 1 of 093a:821d / 093a:821a, as read from
# /sys/class/hidraw/hidrawN/device/report_descriptor (180 bytes).
RDESC = bytes.fromhex(
    "0600ff0901a10125ff7508953f85030901b102c0"
    "05010902a10185010901a10005091901290715002501950775018102"
    "9501750181010501093009311601802 6ff7f751095028106093815 81257f7508"
    "95018106 0601ff09f1750895051500 26ff008100c0c0"
    "050c0901a10185041900 2a9c0215002 69c0275109501 8100c0"
    "05010980a10185051a81002a8300150025017501950381029505 8101c0"
    "0600ff0a00ffa10185061500 26ff00093075089507 8102c0"
    .replace(" ", "")
)
assert len(RDESC) == 180, len(RDESC)

UHID_CREATE2 = 11
UHID_DESTROY = 1
UHID_INPUT2 = 12
BUS_USB = 3
VID, PID = 0x093A, 0x821D


def uhid_create(fd):
    name = b"GLORIOUS Model I 2 Wireless (uhid test)"
    phys = b"uhid-test"
    uniq = b""
    # struct uhid_create2_req: name[128], phys[64], uniq[64], rd_size u16,
    # bus u16, vendor u32, product u32, version u32, country u32, rd_data[4096]
    req = struct.pack(
        "<128s64s64sHHIIII4096s",
        name, phys, uniq, len(RDESC), BUS_USB, VID, PID, 0x36, 0, RDESC,
    )
    os.write(fd, struct.pack("<I", UHID_CREATE2) + req)


def uhid_input(fd, data):
    # struct uhid_input2_req: size u16, data[4096]
    os.write(fd, struct.pack("<I", UHID_INPUT2) + struct.pack("<H4096s", len(data), data))


def find_uhid_hid_device(timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        for p in glob.glob("/sys/bus/hid/devices/0003:093A:821D.*"):
            try:
                if "uhid-test" in open(os.path.join(p, "uevent")).read():
                    return p
            except OSError:
                pass
        time.sleep(0.1)
    raise SystemExit("uhid device did not show up")


def power_supplies(hid_sysfs):
    return sorted(glob.glob(os.path.join(hid_sysfs, "power_supply", "*")))


def read_attr(path, name):
    try:
        return open(os.path.join(path, name)).read().strip()
    except OSError as e:
        return f"<{e.strerror}>"


def main():
    if os.geteuid() != 0:
        raise SystemExit("run as root")
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    bpf_o = os.path.abspath(sys.argv[1])
    loader = os.environ.get("UDEV_HID_BPF") or shutil.which("udev-hid-bpf")
    if not loader:
        raise SystemExit("udev-hid-bpf not found (set UDEV_HID_BPF=/path/to/binary)")

    fd = os.open("/dev/uhid", os.O_RDWR)
    uhid_create(fd)
    try:
        dev = find_uhid_hid_device()
        print("uhid device:", dev)
        rdesc_len = len(open(os.path.join(dev, "report_descriptor"), "rb").read())
        print("report descriptor before BPF:", rdesc_len, "bytes; power supplies:", power_supplies(dev))

        print("attaching", bpf_o)
        subprocess.run([loader, "--verbose", "add", dev, bpf_o], check=True)
        time.sleep(0.5)

        rdesc_len = len(open(os.path.join(dev, "report_descriptor"), "rb").read())
        psy = power_supplies(dev)
        print("report descriptor after BPF:", rdesc_len, "bytes; power supplies:", psy)
        if not psy:
            raise SystemExit("FAIL: no power_supply registered")
        psy = psy[0]
        print("  type/scope/present:", read_attr(psy, "type"), read_attr(psy, "scope"), read_attr(psy, "present"))
        t = time.time()
        print("  capacity before any frame:", read_attr(psy, "capacity"), f"({time.time() - t:.2f}s, must not stall)")
        print("  status before any frame:  ", read_attr(psy, "status"))

        ok = True

        def check(label, frame, expect):
            nonlocal ok
            uhid_input(fd, frame)
            time.sleep(0.2)
            got = read_attr(psy, "capacity")
            flag = "ok " if got == str(expect) else "BAD"
            if flag == "BAD":
                ok = False
            print(f"  {flag} {label:<34} {frame[:4].hex(' ')} -> capacity {got} (expected {expect})")

        check("battery frame 50%", bytes([6, 0xFB, 50, 0, 0, 0, 0, 0]), 50)
        check("trailing zero frame (ignored)", bytes([6, 0, 0, 0, 0, 0, 0, 0]), 50)
        check("battery frame 73%, toggle=1", bytes([6, 0xFB, 73, 1, 0, 0, 0, 0]), 73)
        check("button event F9 (ignored)", bytes([6, 0xF9, 0x80, 3, 0, 0, 0, 0]), 73)
        check("profile event F6 2 (ignored)", bytes([6, 0xF6, 2, 0, 0, 0, 0, 0]), 73)
        check("bogus 150% (ignored)", bytes([6, 0xFB, 150, 0, 0, 0, 0, 0]), 73)
        check("battery frame 100%", bytes([6, 0xFB, 100, 0, 0, 0, 0, 0]), 100)
        check("battery frame 5%", bytes([6, 0xFB, 5, 1, 0, 0, 0, 0]), 5)
        uhid_input(fd, bytes([1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0]))  # mouse motion, unrelated
        time.sleep(0.2)
        print("  status after frames:", read_attr(psy, "status"), "model:", read_attr(psy, "model_name"))
        print("PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        os.write(fd, struct.pack("<I", UHID_DESTROY))
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
