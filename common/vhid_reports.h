/*
 * vhid_reports.h
 *
 * Wire-format HID report structs. These layouts mirror the report
 * descriptor byte-for-byte and are shared between the kernel HID
 * minidriver (which hands them to hidclass.sys through the pending
 * IOCTL_HID_READ_REPORT path) and the user-mode SDK (which builds
 * them before handing them to IOCTL_VHIDKM_*).
 *
 * Packing:
 *   The USB HID class does not insert alignment padding between
 *   fields. A 16-bit X coordinate following a 1-byte button field
 *   must start at offset 2, not 4. We force 1-byte alignment with
 *   pshpack1.h / poppack.h. This is the canonical pattern used by
 *   every Microsoft WDK sample that touches HID reports and is the
 *   only way to keep the struct layout invariant across compilers
 *   and CPU architectures.
 *
 * Report IDs:
 *   Embedded as the first byte of every report because the report
 *   descriptor declares ReportID usages. IOCTL_HID_READ_REPORT
 *   buffer handed to hidclass.sys MUST begin with the ReportID.
 *   The SDK populates this field from a named constant so callers
 *   never have to remember the magic number.
 *
 * Field-width rationale is documented next to the report descriptor
 * in docs/ARCHITECTURE.md §6 — the short version: modifier bitmap
 * matches the boot-protocol layout even though we're non-boot;
 * 6-key rollover matches a stock USB keyboard; 16-bit X/Y covers
 * the virtual-screen absolute range with room for signed deltas;
 * 8-bit wheel matches mouhid.sys's expectations.
 */

#pragma once
#ifndef VHID_REPORTS_H_
#define VHID_REPORTS_H_

#if defined(_KERNEL_MODE) || defined(_NTDDK_)
#include <ntdef.h>
#else
#include <windows.h>
#endif

/*
 * Report ID constants. Do not renumber — the report descriptor hard-
 * codes these values and hidclass.sys will route traffic based on
 * them. Adding a new report means adding a new descriptor block and
 * a new constant; it does not mean shuffling existing IDs.
 */
#define VHID_REPORTID_KEYBOARD_INPUT    0x01
#define VHID_REPORTID_KEYBOARD_OUTPUT   0x01
#define VHID_REPORTID_MOUSE_REL         0x02
#define VHID_REPORTID_MOUSE_ABS         0x03

/*
 * Modifier bit positions inside VHID_KEYBOARD_INPUT_REPORT.Modifiers.
 * Layout matches the HID Boot Protocol keyboard report so application
 * tools that assume the boot shape read the right bits.
 */
#define VHID_KBD_MOD_LCTRL      0x01
#define VHID_KBD_MOD_LSHIFT     0x02
#define VHID_KBD_MOD_LALT       0x04
#define VHID_KBD_MOD_LGUI       0x08
#define VHID_KBD_MOD_RCTRL      0x10
#define VHID_KBD_MOD_RSHIFT     0x20
#define VHID_KBD_MOD_RALT       0x40
#define VHID_KBD_MOD_RGUI       0x80

/*
 * Mouse button bit positions inside VHID_MOUSE_*_REPORT.Buttons.
 * Bits 5..7 are descriptor-level constants (pad) and must be zero
 * on the wire. Zero them in the SDK before submission.
 */
#define VHID_MOUSE_BTN_LEFT     0x01
#define VHID_MOUSE_BTN_RIGHT    0x02
#define VHID_MOUSE_BTN_MIDDLE   0x04
#define VHID_MOUSE_BTN_BACK     0x08
#define VHID_MOUSE_BTN_FORWARD  0x10
#define VHID_MOUSE_BTN_MASK     0x1F

/*
 * Keyboard LED bit positions inside VHID_KEYBOARD_OUTPUT_REPORT.Leds.
 * The bit order matches the "LEDs" usage page (0x08) usages 1..5 as
 * declared in the report descriptor.
 */
#define VHID_KBD_LED_NUM        0x01
#define VHID_KBD_LED_CAPS       0x02
#define VHID_KBD_LED_SCROLL     0x04
#define VHID_KBD_LED_COMPOSE    0x08
#define VHID_KBD_LED_KANA       0x10
#define VHID_KBD_LED_MASK       0x1F

/*
 * Maximum keys reported simultaneously in a single input report.
 * Matches the descriptor's Report Count (6) for the keycode array.
 * Anything beyond this is classic USB ghosting / rollover loss.
 */
#define VHID_KBD_MAX_KEYS       6u

/*
 * Maximum value for absolute X/Y inputs, matching Logical Max (0x7FFF)
 * in the descriptor for Report ID 3 and Win32's MOUSEEVENTF_ABSOLUTE
 * convention (0..65535 for the full screen, but we use 0..32767 to
 * match the descriptor-declared 16-bit signed range upper bound and
 * so absolute X/Y survives as a signed short in some HID tools).
 */
#define VHID_MOUSE_ABS_MAX      32767

#include <pshpack1.h>

typedef struct _VHID_KEYBOARD_INPUT_REPORT {
    UCHAR ReportId;        /* 0x01; VHID_REPORTID_KEYBOARD_INPUT */
    UCHAR Modifiers;       /* VHID_KBD_MOD_* bitmap */
    UCHAR Reserved;        /* always 0; kept for boot-layout parity */
    UCHAR Keys[VHID_KBD_MAX_KEYS];
} VHID_KEYBOARD_INPUT_REPORT, *PVHID_KEYBOARD_INPUT_REPORT;

typedef struct _VHID_KEYBOARD_OUTPUT_REPORT {
    UCHAR ReportId;        /* 0x01; VHID_REPORTID_KEYBOARD_OUTPUT */
    UCHAR Leds;            /* VHID_KBD_LED_* bitmap */
} VHID_KEYBOARD_OUTPUT_REPORT, *PVHID_KEYBOARD_OUTPUT_REPORT;

typedef struct _VHID_MOUSE_REL_REPORT {
    UCHAR ReportId;        /* 0x02; VHID_REPORTID_MOUSE_REL */
    UCHAR Buttons;         /* VHID_MOUSE_BTN_* bitmap */
    SHORT X;               /* signed 16-bit relative delta */
    SHORT Y;               /* signed 16-bit relative delta */
    CHAR  Wheel;           /* signed 8-bit vertical wheel */
    CHAR  HWheel;          /* signed 8-bit horizontal (AC Pan) wheel */
} VHID_MOUSE_REL_REPORT, *PVHID_MOUSE_REL_REPORT;

typedef struct _VHID_MOUSE_ABS_REPORT {
    UCHAR  ReportId;       /* 0x03; VHID_REPORTID_MOUSE_ABS */
    UCHAR  Buttons;        /* VHID_MOUSE_BTN_* bitmap */
    USHORT X;              /* unsigned 16-bit virtual-screen abs X */
    USHORT Y;              /* unsigned 16-bit virtual-screen abs Y */
    CHAR   Wheel;          /* signed 8-bit vertical wheel */
    CHAR   HWheel;         /* signed 8-bit AC Pan wheel */
} VHID_MOUSE_ABS_REPORT, *PVHID_MOUSE_ABS_REPORT;

#include <poppack.h>

/*
 * Compile-time contract checks. If a developer changes a field width
 * without updating the descriptor, or a compiler inserts unexpected
 * padding, these asserts break the build before a bogus descriptor
 * reaches hidclass.sys and triggers a HidParser complaint or worse.
 */
#if defined(C_ASSERT)
C_ASSERT(sizeof(VHID_KEYBOARD_INPUT_REPORT)  == 9);
C_ASSERT(sizeof(VHID_KEYBOARD_OUTPUT_REPORT) == 2);
C_ASSERT(sizeof(VHID_MOUSE_REL_REPORT)       == 8);
C_ASSERT(sizeof(VHID_MOUSE_ABS_REPORT)       == 8);
#else
/* User-mode builds without nt*.h still get C_ASSERT from winnt.h */
_STATIC_ASSERT(sizeof(VHID_KEYBOARD_INPUT_REPORT)  == 9);
_STATIC_ASSERT(sizeof(VHID_KEYBOARD_OUTPUT_REPORT) == 2);
_STATIC_ASSERT(sizeof(VHID_MOUSE_REL_REPORT)       == 8);
_STATIC_ASSERT(sizeof(VHID_MOUSE_ABS_REPORT)       == 8);
#endif

/*
 * Largest on-the-wire report size. Used by the ring buffer in the
 * HID minidriver to size its backing storage, by the user-mode SDK
 * to pre-allocate send buffers, and as a cap on IOCTL input buffer
 * lengths for the raw-report paths.
 */
#define VHID_MAX_REPORT_SIZE    9u

#endif /* VHID_REPORTS_H_ */
