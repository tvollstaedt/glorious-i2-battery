// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Thomas Vollstädt */
/*
 * Injects one synthetic input report into the receiver's HID device.
 *
 * The mouse sends no battery frames while its cable is plugged in, so the
 * kernel never learns that it is charging.  tools/gi2-charging-sync runs this
 * program whenever the cable (093a:821a) appears or disappears and feeds the
 * driver a frame that carries the last known percentage plus the charging
 * bit (byte 4, bit 0) the driver added to report 6.
 */

#include "vmlinux.h"
#include "hid_bpf.h"
#include "hid_bpf_helpers.h"
#include <bpf/bpf_tracing.h>

struct inject_args {
	__u32 hid_id;
	__u8 report[8];
};

SEC("syscall")
int gi2_inject(struct inject_args *args)
{
	struct hid_bpf_ctx *ctx = hid_bpf_allocate_context(args->hid_id);
	int ret;

	if (!ctx)
		return -ENODEV;

	ret = hid_bpf_input_report(ctx, HID_INPUT_REPORT, args->report,
				   sizeof(args->report));
	hid_bpf_release_context(ctx);

	return ret;
}

char _license[] SEC("license") = "GPL";
