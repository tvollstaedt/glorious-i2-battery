#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Thomas Vollstädt
"""Read the Glorious Model I 2 Wireless battery level with hidapi (Linux, macOS).

The mouse never answers a battery query. Instead it pushes an input report
on its vendor collection (usage page 0xFF00, usage 0xFF00, report ID 6)
every 20-40 s while it is being used over the 2.4 GHz receiver:

    06 FB <percent> <toggle> 00 00 00 00

This tool opens that collection on the receiver (093a:821d) or the cabled
mouse (093a:821a) and waits for such a frame.

    gi2-battery.py            wait up to 60 s for one frame, print it, exit
    gi2-battery.py --json     same, as JSON
    gi2-battery.py --watch    keep printing every frame
    gi2-battery.py --timeout 120

Requires the "hid" Python package (pip install hid / pacman -S python-hidapi).
On Linux the hidraw node must be writable by your user (see the udev rule in
the repository); on macOS no special permission is needed.

Exit status: 0 frame received, 2 no frame within the timeout, 3 no device.
"""

import argparse
import json
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("python hidapi module missing: pip install hid  (Arch: python-hidapi)")

VID = 0x093A
PIDS = {0x821D: "receiver", 0x821A: "usb"}
VENDOR_PAGE = 0xFF00
VENDOR_USAGE = 0xFF00
REPORT_ID = 6
MARKER = 0xFB


def find_devices():
    """Return [(path, connection)] for the vendor input collection of each link."""
    found = []
    for info in hid.enumerate(VID, 0):
        if info["product_id"] not in PIDS:
            continue
        # macOS and Windows expose one hidapi device per top-level collection and
        # fill usage_page/usage; Linux (hidraw) exposes one per interface and
        # reports the first collection (0xff00/0x0001 on interface 1).
        page, usage = info.get("usage_page", 0), info.get("usage", 0)
        if sys.platform == "linux":
            if info.get("interface_number", -1) not in (1, -1):
                continue
            if page and page != VENDOR_PAGE:
                continue
        elif (page, usage) != (VENDOR_PAGE, VENDOR_USAGE):
            continue
        found.append((info["path"], PIDS[info["product_id"]]))
    # prefer the receiver: battery frames have only been observed wireless
    found.sort(key=lambda d: d[1] != "receiver")
    return found


def parse(frame):
    if len(frame) >= 3 and frame[0] == REPORT_ID and frame[1] == MARKER and 0 <= frame[2] <= 100:
        return {"percent": frame[2], "raw": frame[:8].hex(" ")}
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--watch", action="store_true", help="keep listening and print every frame")
    ap.add_argument("--json", action="store_true", help="JSON output")
    ap.add_argument("--timeout", type=float, default=60.0, help="seconds to wait for a frame (default 60)")
    ap.add_argument("--all", action="store_true", help="print every report-6 frame, not only battery ones")
    args = ap.parse_args()

    devices = find_devices()
    if not devices:
        print("no Model I 2 Wireless receiver or cable found", file=sys.stderr)
        return 3

    handles = []
    for path, conn in devices:
        try:
            h = hid.device()
            h.open_path(path)
            h.set_nonblocking(True)
            handles.append((h, conn))
        except OSError as e:
            print(f"cannot open {conn} ({path!r}): {e}", file=sys.stderr)
    if not handles:
        return 3

    deadline = time.time() + args.timeout
    got = False
    try:
        while args.watch or time.time() < deadline:
            for h, conn in handles:
                data = h.read(64)
                if not data:
                    continue
                frame = bytes(data)
                if frame[0] != REPORT_ID:
                    continue
                info = parse(frame)
                if info is None:
                    if args.all:
                        print(f"{conn}: other report-6 frame {frame[:8].hex(' ')}")
                    continue
                info["connection"] = conn
                info["time"] = time.strftime("%Y-%m-%dT%H:%M:%S")
                print(json.dumps(info) if args.json else f"{info['time']} {conn}: battery {info['percent']}%  ({info['raw']})")
                sys.stdout.flush()
                got = True
                if not args.watch:
                    return 0
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        for h, _ in handles:
            h.close()
    if got:
        return 0
    print("no battery frame received (the mouse only sends them while in use over the receiver)", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
