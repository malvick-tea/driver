/*
 * vhid_protocol.h
 *
 * Invariants and compile-time sanity checks for the user <-> kernel
 * protocol. This header is deliberately short and lives alongside the
 * IOCTL/report definitions so that any future change to the protocol
 * trips a visible contract here.
 *
 * The file contains only:
 *
 *   - Numerical invariants: protocol-wide limits (maximum hold time,
 *     ring-buffer capacity hint, screen-metric sanity bounds).
 *
 *   - Field-width re-assertions: repeat the C_ASSERTs from the report
 *     and ioctl headers next to the protocol-level ABI number. A
 *     developer who updates VHID_API_LEVEL without reviewing the
 *     structs will see these fail immediately.
 *
 *   - A single documentation block that names every IOCTL and the
 *     order in which user-mode is expected to call them.
 *
 * There is deliberately no code or runtime state in this header.
 */

#pragma once
#ifndef VHID_PROTOCOL_H_
#define VHID_PROTOCOL_H_

#include "vhid_version.h"
#include "vhid_reports.h"
#include "vhid_ioctl.h"

/*
 * Maximum value accepted for the HoldMs field of
 * VHID_KEYSTROKE_REQ. Values above this clamp down on submit.
 * Bound chosen so one caller cannot park a KMDF timer worker
 * indefinitely; five seconds is far longer than any human keystroke.
 */
#define VHID_KEYSTROKE_HOLD_MAX_MS   5000u

/*
 * Ring buffer depth in the HID minidriver's report queue. Power of
 * two so the index arithmetic is a single AND rather than a
 * modulo. Overflow drops the oldest entry, mirroring the lossy
 * behaviour of a real interrupt-IN endpoint when the host cannot
 * keep up.
 */
#define VHID_REPORT_RING_CAPACITY    64u

/*
 * Absolute screen bounds. Rejected if width or height is zero (to
 * avoid divide-by-zero) or exceeds 1,000,000 logical pixels (no real
 * virtual screen is that wide; treat as corruption).
 */
#define VHID_SCREEN_METRIC_MIN_EXTENT 1
#define VHID_SCREEN_METRIC_MAX_EXTENT 1000000

/*
 * Maximum slots supported by the bus driver. v1 ships as a single-
 * device bus; the slot table is still indexed as an array so the
 * upgrade path to multiple concurrent devices is a constant-only
 * change in this header.
 */
#define VHID_MAX_SLOTS               1u

/*
 * Recommended startup sequence for user-mode:
 *
 *   1) Open \\?\{GUID_DEVINTERFACE_VUSBBUS_CTL}
 *      IOCTL_VUSBBUS_GET_VERSION -> check ApiLevel == VHID_API_LEVEL
 *      IOCTL_VUSBBUS_PLUG_IN     -> obtain SlotId
 *   2) Wait for \\?\{GUID_DEVINTERFACE_VHIDKM_CTL} to appear
 *      (use CM_Register_Notification or polling)
 *   3) Open the function control device
 *      IOCTL_VHIDKM_GET_VERSION  -> check ApiLevel == VHID_API_LEVEL
 *      IOCTL_VHIDKM_SET_SCREEN_METRICS (optional, required for _PX)
 *      IOCTL_VHIDKM_{KEYBOARD,MOUSE}_* -> inject reports
 *   4) Teardown
 *      Close function handle (driver enqueues an all-up reset)
 *      IOCTL_VUSBBUS_UNPLUG
 *      Close bus handle
 *
 * Closing handles out of order is legal but may cause transient
 * failures on the next PLUG_IN if PnP has not finished tearing down
 * the previous device.
 */

#if defined(C_ASSERT)
C_ASSERT(VHID_API_LEVEL == 1u);
C_ASSERT(VHID_MAX_SLOTS == 1u);
C_ASSERT((VHID_REPORT_RING_CAPACITY & (VHID_REPORT_RING_CAPACITY - 1)) == 0);
C_ASSERT(VHID_MAX_REPORT_SIZE >= sizeof(VHID_KEYBOARD_INPUT_REPORT));
C_ASSERT(VHID_MAX_REPORT_SIZE >= sizeof(VHID_MOUSE_REL_REPORT));
C_ASSERT(VHID_MAX_REPORT_SIZE >= sizeof(VHID_MOUSE_ABS_REPORT));
#endif

#endif /* VHID_PROTOCOL_H_ */
