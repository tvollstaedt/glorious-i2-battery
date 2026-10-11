// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Thomas Vollstädt */
/*
 * HID-BPF driver: battery reporting for the Glorious Model I 2 Wireless
 *
 * The mouse (and its 2.4 GHz receiver) has a vendor collection on
 * interface 1 that carries input report 6.  The firmware pushes battery
 * frames on that report roughly every 20-40 s while the mouse is in use:
 *
 *     06 FB <percent> <toggle> 00 00 00 00
 *     06 00 00 00 00 00 00 00          <- always follows, must be ignored
 *
 * Other frames on the same report ID (seen in Glorious CORE's handler):
 *     06 F9 >=80 <button>  button bound to "launch program" pressed
 *     06 F6 <n>            profile change (older MouseV2 form)
 *
 * The original descriptor declares the seven bytes as opaque vendor data,
 * so the kernel cannot know a battery is in there.  This program rewrites
 * the report-6 collection so byte 2 becomes
 *     Generic Device Controls / Battery Strength (0x06/0x20), 0..100
 * which makes hid-input register a power_supply for the device.  UPower
 * then exposes it as a mouse battery (it finds the sibling input node with
 * ID_INPUT_MOUSE) and the KDE/GNOME battery applets show it.
 *
 * Two details:
 *
 *  - The battery field is wrapped in a Physical collection whose usage is
 *    Digitizer/Stylus.  hid-input sets bat->avoid_query for that case.  We
 *    need it because the mouse never answers GET_REPORT (every request
 *    times out after 5 s), and without avoid_query every read of
 *    /sys/class/power_supply/.../capacity before the first pushed frame
 *    would stall for 5 s and fail.
 *
 *  - hid_device_event sanitizes the report: only frames with the 0xFB
 *    marker keep their percentage, every other report-6 frame gets 0xFF
 *    in the battery byte, which is outside logical max and therefore
 *    dropped by hidinput_update_battery().
 *
 * Original report descriptor of interface 1 (180 bytes):
 *
 * 0x06, 0x00, 0xff,  // Usage Page (Vendor Defined Page 1)   0
 * 0x09, 0x01,        // Usage (Vendor Usage 1)               3
 * 0xa1, 0x01,        // Collection (Application)             5
 * 0x25, 0xff,        //  Logical Maximum (255)               7
 * 0x75, 0x08,        //  Report Size (8)                     9
 * 0x95, 0x3f,        //  Report Count (63)                   11
 * 0x85, 0x03,        //  Report ID (3)                       13
 * 0x09, 0x01,        //  Usage (Vendor Usage 1)              15
 * 0xb1, 0x02,        //  Feature (Data,Var,Abs)              17   <- config channel
 * 0xc0,              // End Collection                       19
 * 0x05, 0x01,        // Usage Page (Generic Desktop)         20
 * 0x09, 0x02,        // Usage (Mouse)                        22
 * 0xa1, 0x01,        // Collection (Application)             24
 * 0x85, 0x01,        //  Report ID (1)                       26
 * ...                //  buttons, X/Y, wheel, 5 vendor bytes
 * 0xc0,              // End Collection                       101
 * 0x05, 0x0c,        // Usage Page (Consumer Devices)        102
 * 0x09, 0x01,        // Usage (Consumer Control)             104
 * 0xa1, 0x01,        // Collection (Application)             106
 * 0x85, 0x04,        //  Report ID (4)                       108
 * ...
 * 0xc0,              // End Collection                       124
 * 0x05, 0x01,        // Usage Page (Generic Desktop)         125
 * 0x09, 0x80,        // Usage (System Control)               127
 * 0xa1, 0x01,        // Collection (Application)             129
 * 0x85, 0x05,        //  Report ID (5)                       131
 * ...
 * 0xc0,              // End Collection                       155
 * 0x06, 0x00, 0xff,  // Usage Page (Vendor Defined Page 1)   156  <- replaced from here
 * 0x0a, 0x00, 0xff,  // Usage (Vendor Usage 0xff00)          159
 * 0xa1, 0x01,        // Collection (Application)             162
 * 0x85, 0x06,        //  Report ID (6)                       164
 * 0x15, 0x00,        //  Logical Minimum (0)                 166
 * 0x26, 0xff, 0x00,  //  Logical Maximum (255)               168
 * 0x09, 0x30,        //  Usage (Vendor Usage 0x30)           171
 * 0x75, 0x08,        //  Report Size (8)                     173
 * 0x95, 0x07,        //  Report Count (7)                    175
 * 0x81, 0x02,        //  Input (Data,Var,Abs)                177
 * 0xc0,              // End Collection                       179
 */

#include "vmlinux.h"
#include "hid_bpf.h"
#include "hid_bpf_helpers.h"
#include <bpf/bpf_tracing.h>

#define VID_GLORIOUS_PIXART	0x093A
#define PID_MODEL_I2_RECEIVER	0x821D	/* 2.4 GHz dongle */
#define PID_MODEL_I2_USB	0x821A	/* mouse on its cable, not matched */

#define ORIG_RDESC_SIZE		180
#define VENDOR_COLLECTION_OFF	156

#define BATTERY_REPORT_ID	0x06
#define BATTERY_MARKER		0xFB

/*
 * Receiver only.  The cabled mouse (PID_MODEL_I2_USB) has the identical
 * descriptor but never sends battery frames, so a power_supply on it would
 * sit at 0 % forever and show up as a second device in the battery applet.
 */
HID_BPF_CONFIG(
	HID_DEVICE(BUS_USB, HID_GROUP_GENERIC, VID_GLORIOUS_PIXART, PID_MODEL_I2_RECEIVER)
);

/* Replacement for the vendor collection at offset 156 */
static const __u8 battery_collection[] = {
	0x06, 0x00, 0xff,	// Usage Page (Vendor Defined Page 1)
	0x0a, 0x00, 0xff,	// Usage (Vendor Usage 0xff00)
	0xa1, 0x01,		// Collection (Application)
	0x85, 0x06,		//  Report ID (6)
	/* byte 1: frame marker (0xFB battery, 0xF9 button, ...) */
	0x09, 0x30,		//  Usage (Vendor Usage 0x30)
	0x15, 0x00,		//  Logical Minimum (0)
	0x26, 0xff, 0x00,	//  Logical Maximum (255)
	0x75, 0x08,		//  Report Size (8)
	0x95, 0x01,		//  Report Count (1)
	0x81, 0x02,		//  Input (Data,Var,Abs)
	/* byte 2: battery percentage, wrapped so hid-input never queries it */
	0x05, 0x0d,		//  Usage Page (Digitizers)
	0x09, 0x20,		//  Usage (Stylus)
	0xa1, 0x00,		//  Collection (Physical)
	0x05, 0x06,		//   Usage Page (Generic Device Controls)
	0x09, 0x20,		//   Usage (Battery Strength)
	0x15, 0x00,		//   Logical Minimum (0)
	0x25, 0x64,		//   Logical Maximum (100)
	0x75, 0x08,		//   Report Size (8)
	0x95, 0x01,		//   Report Count (1)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0xc0,			//  End Collection
	/* bytes 3..7: toggle bit and padding, kept as vendor data */
	0x06, 0x00, 0xff,	//  Usage Page (Vendor Defined Page 1)
	0x09, 0x30,		//  Usage (Vendor Usage 0x30)
	0x15, 0x00,		//  Logical Minimum (0)
	0x26, 0xff, 0x00,	//  Logical Maximum (255)
	0x75, 0x08,		//  Report Size (8)
	0x95, 0x05,		//  Report Count (5)
	0x81, 0x02,		//  Input (Data,Var,Abs)
	0xc0,			// End Collection
};

SEC(HID_BPF_RDESC_FIXUP)
int BPF_PROG(hid_rdesc_fixup_glorious_i2, struct hid_bpf_ctx *hctx)
{
	__u8 *data = hid_bpf_get_data(hctx, 0 /* offset */, HID_MAX_DESCRIPTOR_SIZE);

	if (!data)
		return 0; /* EPERM check */

	__builtin_memcpy(data + VENDOR_COLLECTION_OFF, battery_collection,
			 sizeof(battery_collection));

	return VENDOR_COLLECTION_OFF + sizeof(battery_collection);
}

SEC(HID_BPF_DEVICE_EVENT)
int BPF_PROG(hid_device_event_glorious_i2, struct hid_bpf_ctx *hctx)
{
	__u8 *data = hid_bpf_get_data(hctx, 0 /* offset */, 8 /* size */);

	if (!data)
		return 0; /* EPERM check */

	if (data[0] != BATTERY_REPORT_ID)
		return 0;

	/*
	 * Only "06 FB pct ..." carries a battery level.  Push everything else
	 * (the trailing all-zero frame, button and profile events) out of the
	 * 0..100 range so hid-input ignores the field.
	 */
	if (data[1] != BATTERY_MARKER || data[2] > 100)
		data[2] = 0xff;

	return 0;
}

HID_BPF_OPS(glorious_i2) = {
	.hid_rdesc_fixup = (void *)hid_rdesc_fixup_glorious_i2,
	.hid_device_event = (void *)hid_device_event_glorious_i2,
};

SEC("syscall")
int probe(struct hid_bpf_probe_args *ctx)
{
	/*
	 * Both interfaces of the receiver match the VID/PID.  Interface 0
	 * (keyboard emulation, 65 bytes) must be left alone; only interface 1
	 * with the 180-byte descriptor carries the vendor collection.
	 */
	ctx->retval = ctx->rdesc_size != ORIG_RDESC_SIZE;
	if (ctx->retval)
		ctx->retval = -EINVAL;

	/* make sure the vendor collection is where we expect it */
	if (ctx->rdesc[VENDOR_COLLECTION_OFF + 8] != 0x85 ||
	    ctx->rdesc[VENDOR_COLLECTION_OFF + 9] != BATTERY_REPORT_ID ||
	    ctx->rdesc[VENDOR_COLLECTION_OFF + 20] != 0x07)
		ctx->retval = -EINVAL;

	return 0;
}

char _license[] SEC("license") = "GPL";
