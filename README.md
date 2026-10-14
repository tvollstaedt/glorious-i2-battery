# glorious-i2-battery

Native battery reporting for the **Glorious Model I 2 Wireless** mouse on Linux,
implemented as a HID-BPF driver. Once attached, the kernel itself creates a
`power_supply` for the mouse, UPower picks it up, and the battery shows up in
the KDE Plasma / GNOME battery applets like any Logitech or Bluetooth mouse.
No kernel module, no DKMS, no daemon. A small udev-triggered helper adds the
"charging" state while the cable is plugged in (see below).

Also included:

- `tools/gi2-battery.py`: a small cross-platform (Linux, macOS) hidapi script
  that prints the battery level from the command line.
- `tools/uhid-test.py`: a self-test that runs the driver against a virtual
  (uhid) copy of the mouse and injects frames.

Configuration of the mouse (DPI, polling rate, lighting, button bindings,
macros) is a solved problem: use [Krodity/glorious-i2](https://github.com/Krodity/glorious-i2)
(`gi2` CLI + GTK app). This project only adds what that one cannot do:
putting the battery where the desktop expects it.

Everything here was derived from Glorious CORE 2.1.21's unminified Electron
bundle (`MouseI2DeviceHandler` / `MouseV2DeviceHandler`) and from watching
the device. No Glorious code is included.

**Status:** working on a Model I 2 Wireless with firmware `0036`, CachyOS
kernel 7.2, KDE Plasma 6.7 (UPower 1.91). Not affiliated with Glorious.

## How the mouse reports its battery

The receiver (`093a:821d`) and the mouse on its cable (`093a:821a`) both have
two HID interfaces. Interface 1 carries, next to the mouse collection, a
vendor collection (usage page `0xFF00`, usage `0xFF00`) with **input report 6**:

```
06 FB <percent> <toggle> 00 00 00 00     battery frame, percent 0..100
06 00 00 00 00 00 00 00                  always follows a battery frame
06 F9 >=80 <button>                      button bound to "launch program"
06 F6 <n>                                profile change (older MouseV2 form)
```

Facts that shape the design:

- **The mouse cannot be asked.** Glorious CORE never queries the battery for
  this model; it only listens for the frames above (its other mice have real
  query commands, the I 2 does not). Every `GET_REPORT` to the device, input
  or feature, times out after 5 s.
- Frames arrive **only over the 2.4 GHz receiver**, roughly every 20-40 s while
  the mouse is in use. With the cable plugged in, neither the cable nor the
  receiver delivered a single frame in 20 minutes of capture. While charging
  you therefore see the last value the receiver got.
- `toggle` flips between 0 and 1 on consecutive frames. CORE displays it as
  "charging", which is wrong; it is ignored here.
- Firmware version is the USB `bcdDevice` (`0036` at the time of writing).

## How the driver works

`bpf/0010-Glorious__Model-I2-Wireless.bpf.c` attaches to interface 1 of the
receiver (the probe checks for the 180-byte descriptor) and does two things:

1. **Report descriptor fixup.** The vendor collection is rewritten so that
   byte 2 of report 6 is declared as *Generic Device Controls / Battery
   Strength* with a logical range of 0..100. `hid-input` then registers a
   `power_supply` named `hid-0003:093A:821D.XXXX-battery-6`. The field is
   wrapped in a Physical collection with usage *Digitizer / Stylus*, which
   makes `hid-input` set `avoid_query`; without that, every read of
   `.../capacity` before the first frame would issue a `GET_REPORT` and stall
   for 5 s.
2. **Event filter.** `hid_device_event` sets byte 2 to `0xFF` (out of range,
   dropped by the kernel) for every report-6 frame that is not a `0xFB`
   battery frame. Otherwise the trailing zero frame would report 0 % and a
   profile-change frame would report 1-3 %.

3. **Charging bit.** Byte 4 of report 6 is padding on the wire. The descriptor
   declares its bit 0 as *Battery System / Charging*, which `hid-input` turns
   into the `status` of the `power_supply` (`Charging` / `Discharging`). The
   mouse itself never sets it; `hid_device_event` only lets it through on
   battery frames and clears it everywhere else.

Nothing is ever written to the mouse. The normal mouse, consumer-control and
keyboard interfaces are untouched; `hidraw` keeps working for configuration
tools.

## Charging state while the cable is plugged in

With the cable plugged in the mouse sends no frames at all, so the kernel
would keep the last wireless reading with `Discharging`. The cable itself is
the only signal there is (the mouse shows up as `093a:821a`), and it is enough
to know the mouse is being charged:

1. `udev/73-glorious-model-i2-charging.rules` fires when `093a:821a` appears or
   disappears, and when the receiver is plugged in.
2. It runs `tools/gi2-charging-sync` (installed to `/usr/local/bin`) one second
   later. The helper checks whether the cable is present, takes the last known
   level, and injects a synthetic `06 FB <level> 00 <charging> 00 00 00` frame
   into the receiver's HID device using `bpf/gi2-inject.bpf.c`
   (`hid_bpf_input_report` from a syscall program). The kernel then updates the
   `power_supply`, UPower and the desktop show "charging".
3. When the cable is pulled the mouse goes wireless again, sends a real frame
   (the level is current again) and the helper clears the charging bit.

The level shown while charging is the last wireless reading; it does not grow
while charging, there is no data source for it. The last known level is also
kept in `/var/lib/gi2-battery/last-level` so a reboot with the cable plugged
in still reports `Charging` with a valid level. If no level was ever seen, the
helper leaves the battery alone (the kernel only publishes a status once it
has a valid level). "Fully charged" cannot be told apart from "charging".

Check: `gi2-charging-sync` logs to the journal (`journalctl -t gi2-charging`),
and `cat /sys/class/power_supply/hid-*-battery-6/status` shows the state.

## Requirements

- Kernel with `CONFIG_HID_BPF=y` and BTF (`/sys/kernel/btf/vmlinux`), 6.11 or
  newer for the struct_ops API. CachyOS/Arch kernels qualify.
- `clang`, `bpftool` (Arch package `bpf`), `make`, `python3` (for the charging
  helper, stdlib only; x86_64, the `bpf()` syscall number is hard-coded).
- [`udev-hid-bpf`](https://gitlab.freedesktop.org/libevdev/udev-hid-bpf)
  as the loader (Arch package `udev-hid-bpf`).
- For the Python tool: `python-hidapi` and the udev rule in `udev/` so your
  user may open the hidraw node.

## Build, test, install

```bash
make                 # -> build/0010-Glorious__Model-I2-Wireless.bpf.o, build/gi2-inject.bpf.o
make test            # attach to the plugged-in receiver/mouse (not persistent)
make install         # driver + charging helper + udev rules, loads on every plug
```

`make install` runs `udev-hid-bpf install`, which copies the object to
`/etc/udev-hid-bpf/` and writes
`/etc/udev/rules.d/99-hid-bpf-0010-Glorious__Model-I2-Wireless.rules`.
If the `udev-hid-bpf` package is not available, build the loader from source
(`cargo build --release` in its repository) and let it install itself once:

```bash
sudo ./target/release/udev-hid-bpf install --install-exe --prefix /usr/local \
    /path/to/build/0010-Glorious__Model-I2-Wireless.bpf.o
```

After installing, unplug and replug the receiver once (or run
`sudo udevadm trigger -c add -s hid`). Check:

```bash
ls /sys/class/power_supply/            # hid-0003:093A:821D.*-battery-6
upower -e | grep 821D
upower -i /org/freedesktop/UPower/devices/battery_hid_0003o093Ao821Dx0018_battery_6
```

The driver self-test needs root (it creates a uhid device and loads BPF):

```bash
sudo tools/uhid-test.py build/0010-Glorious__Model-I2-Wireless.bpf.o
```

It checks that the power_supply appears, that reading `capacity` before any
frame does not stall, and that battery frames are accepted while zero,
button, profile and out-of-range frames are ignored.

## Known limitations

- **0 % until the first frame.** The kernel cannot know the level before the
  mouse sends it. After booting or plugging the receiver, the battery shows
  0 % / state unknown until the first frame, normally within a few seconds of
  moving the mouse (observed: 4 s after unplugging the cable), at most ~40 s.
  If this triggers a "battery low" notification on your desktop, that is why.
- **UPower calls it a keyboard.** UPower guesses the device kind from all
  input nodes under the same USB device, and the receiver's interface 0 is a
  keyboard (used for key bindings). Keyboard outranks mouse in UPower's
  priority list, so the icon is a keyboard; the name is still
  "Glorious Model I 2 Wireless". Changing that needs a UPower heuristic
  (there is a `TODO` for exactly this in `up-device-supply.c`) rather than
  a driver change.
- **Level frozen while cabled.** No frames while the cable is connected, so the
  percentage stays at the last wireless reading (the state shows `Charging`,
  see above). The cabled mouse (`093a:821a`) is deliberately not matched; it
  would only add a second, permanently empty battery entry.
- Bluetooth (`093a:821b`) is not handled; it uses a different report layout.

## Command-line battery readout (Linux and macOS)

```bash
tools/gi2-battery.py             # waits up to 60 s for a frame
tools/gi2-battery.py --watch     # print every frame
tools/gi2-battery.py --json
```

On Linux this needs the udev rule (`sudo cp udev/72-glorious-model-i2.rules
/etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger`).
On macOS, hidapi can open the vendor collection without extra permissions.
The macOS path is untested; it follows hidapi's documented per-collection
device model, so reports are welcome.

## Configuration of the mouse

Use [Krodity/glorious-i2](https://github.com/Krodity/glorious-i2):

```bash
gi2 status
gi2 dpi 400 800 1600 3200 -a 2
gi2 light breathing -c '#ff0000' '#0000ff'
gi2 bind forward key:ctrl+c
glorious-i2            # GTK 4 app
```

Its protocol notes (`PROTOCOL.md`) match what CORE 2.1.21 does; its
`mouse-battery-monitor` daemon is not needed when this driver is installed.

## Layout

```
bpf/0010-Glorious__Model-I2-Wireless.bpf.c   the driver
bpf/gi2-inject.bpf.c                         syscall program: injects one frame
tools/gi2-charging-sync                      udev helper: cable -> charging state
bpf/hid_bpf*.h, hid_report*.h, hid_usages.h  headers from udev-hid-bpf (GPL-2.0)
tools/gi2-battery.py                         hidapi reader, Linux + macOS
tools/uhid-test.py                           driver self-test
udev/72-glorious-model-i2.rules              hidraw access for the user
udev/73-glorious-model-i2-charging.rules     runs the helper on cable plug/unplug
Makefile
```

## Contributing

Captures of frames this driver does not know yet are the most useful thing:
`tools/gi2-battery.py --watch --all` prints every report-6 frame. Of
particular interest are frames while charging and from the Bluetooth link.

## License

GPL-2.0-only (see `LICENSE`). The headers in `bpf/` other than the driver
itself are copied unmodified from
[udev-hid-bpf](https://gitlab.freedesktop.org/libevdev/udev-hid-bpf), also
GPL-2.0-only.
