/*
 * vhid_ioctl.h
 *
 * Control-device IOCTL definitions shared between kernel and user
 * mode. Two separate control surfaces live on two device-interface
 * GUIDs (see vhid_guids.h); this file declares the codes and
 * request/response layouts for both.
 *
 * Design rules that apply to every IOCTL in this file:
 *
 *   1. DeviceType = FILE_DEVICE_UNKNOWN. User-defined IOCTLs must
 *      avoid the system-assigned type-code space.
 *
 *   2. Method = METHOD_BUFFERED. The framework double-buffers both
 *      input and output, which (a) removes an entire class of TOCTOU
 *      bugs on user-mode memory, (b) keeps the IOCTL validator code
 *      trivial, (c) is a wash for the short payloads we ship.
 *
 *   3. Access = FILE_READ_ACCESS for read-only IOCTLs, FILE_WRITE_ACCESS
 *      for mutating operations. Combined with the SDDL on each control
 *      device this gives us a belt-and-suspenders access model.
 *
 *   4. Every request struct that can grow has an explicit Size field
 *      validated against sizeof(). This protects against caller-side
 *      version mismatches: a v1 driver receiving a v2 request will
 *      refuse the call with STATUS_INVALID_BUFFER_SIZE rather than
 *      reading uninitialized trailing bytes.
 *
 *   5. No embedded pointers. Every payload is POD with fixed layouts
 *      so kernel-mode can reason about the buffer without PROBE_FOR*
 *      dances (which METHOD_BUFFERED already eliminates for the
 *      outer buffer).
 *
 *   6. All numeric fields are fixed-width types (UINT8, INT16, ...)
 *      so the ABI is identical between 32-bit and 64-bit user mode
 *      and between x64 and ARM64 kernels.
 */

#pragma once
#ifndef VHID_IOCTL_H_
#define VHID_IOCTL_H_

#if defined(_KERNEL_MODE) || defined(_NTDDK_)
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

#include "vhid_reports.h"
#include "vhid_version.h"

/*
 * Function-code space map:
 *   0x800..0x8FF — vusbbus.sys control device
 *   0x900..0x9FF — vhidkm.sys control device
 * Keeping them in disjoint ranges means a code audit can tell at a
 * glance which driver a misrouted IOCTL was destined for.
 */

/* Shared response structs. */

#include <pshpack4.h>

typedef struct _VHID_VERSION {
    UINT32 Major;          /* VHID_VERSION_MAJOR at build time */
    UINT32 Minor;          /* VHID_VERSION_MINOR */
    UINT32 Build;          /* VHID_VERSION_BUILD */
    UINT32 ApiLevel;       /* VHID_API_LEVEL — user-mode pins against this */
} VHID_VERSION, *PVHID_VERSION;

/* vusbbus.sys control IOCTLs (0x800..0x8FF). */

/*
 * IOCTL_VUSBBUS_GET_VERSION
 *   Input:  none
 *   Output: VHID_VERSION
 *   Retrieves the bus driver's version/API level. User-mode callers
 *   MUST issue this before any other IOCTL and refuse to continue
 *   on ApiLevel mismatch. Read-access only.
 */
#define IOCTL_VUSBBUS_GET_VERSION \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)

/*
 * IOCTL_VUSBBUS_PLUG_IN
 *   Input:  VHID_PLUGIN_REQ
 *   Output: VHID_PLUGIN_RESP
 *   Creates the virtual USB child PDO and reports it to PnP. Blocks
 *   (at PASSIVE_LEVEL) until the PDO exists and has been published
 *   through InvalidateDeviceRelations. Returns the assigned slot id
 *   and the generated instance GUID. v1 allows only one live device,
 *   so calling this twice back-to-back fails with
 *   STATUS_DEVICE_ALREADY_ATTACHED.
 *
 *   Vid/Pid/Version are reserved in v1 and ignored: the device always
 *   presents the canonical VHID_DEFAULT_VID/PID/REV because the HID
 *   minidriver INF matches those hardware IDs statically. The fields
 *   are kept in the request so a future dynamic-INF revision can honor
 *   them without an ABI change. Serial IS honored (surfaced as the USB
 *   iSerialNumber string and the PnP instance id).
 */
typedef struct _VHID_PLUGIN_REQ {
    UINT32 Size;           /* must equal sizeof(VHID_PLUGIN_REQ) */
    UINT16 Vid;            /* reserved in v1; ignored */
    UINT16 Pid;            /* reserved in v1; ignored */
    UINT16 Version;        /* reserved in v1; ignored */
    UINT16 Reserved;       /* must be 0 */
    WCHAR  Serial[32];     /* non-null-terminated; driver copies up to 32 */
} VHID_PLUGIN_REQ, *PVHID_PLUGIN_REQ;

typedef struct _VHID_PLUGIN_RESP {
    UINT32 SlotId;         /* small integer identifying this instance */
    GUID   InstanceId;     /* logical identifier; not the PnP instance id */
} VHID_PLUGIN_RESP, *PVHID_PLUGIN_RESP;

#define IOCTL_VUSBBUS_PLUG_IN \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/*
 * IOCTL_VUSBBUS_UNPLUG
 *   Input:  VHID_UNPLUG_REQ
 *   Output: none
 *   Marks the indicated slot's PDO as missing and invalidates bus
 *   relations so PnP tears down the stack above it. Safe to call
 *   repeatedly; unknown slot ids fail with STATUS_NOT_FOUND.
 */
typedef struct _VHID_UNPLUG_REQ {
    UINT32 Size;           /* must equal sizeof(VHID_UNPLUG_REQ) */
    UINT32 SlotId;
} VHID_UNPLUG_REQ, *PVHID_UNPLUG_REQ;

#define IOCTL_VUSBBUS_UNPLUG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/*
 * IOCTL_VUSBBUS_LIST
 *   Input:  none
 *   Output: VHID_SLOT_LIST (variable length)
 *   Enumerates every live slot. Output buffer length determines how
 *   many entries are returned; Information field of the OVERLAPPED
 *   result reflects bytes written. Short buffers fail with
 *   STATUS_BUFFER_TOO_SMALL and Information = required bytes so the
 *   caller can size the next attempt.
 */
typedef struct _VHID_SLOT_INFO {
    UINT32 SlotId;
    UINT16 Vid;
    UINT16 Pid;
    UINT16 Version;
    UINT16 Reserved;
    GUID   InstanceId;
    WCHAR  Serial[32];     /* not guaranteed null-terminated */
} VHID_SLOT_INFO, *PVHID_SLOT_INFO;

typedef struct _VHID_SLOT_LIST {
    UINT32 Count;          /* number of populated Slots entries */
    UINT32 Reserved;
    VHID_SLOT_INFO Slots[1]; /* variable length */
} VHID_SLOT_LIST, *PVHID_SLOT_LIST;

#define IOCTL_VUSBBUS_LIST \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)

/*
 * IOCTL_VUSBBUS_GET_USB_DESCRIPTOR
 *   Input:  VHID_DESC_REQ
 *   Output: raw descriptor bytes
 *   Diagnostic access to the canonical USB descriptor tables. Not on
 *   the hot path; used by tests and tooling to verify the driver's
 *   declared identity matches expectations.
 */
typedef struct _VHID_DESC_REQ {
    UINT32 Size;
    UINT8  Type;           /* USB descriptor type byte (0x01, 0x02, 0x22 ...) */
    UINT8  Index;          /* sub-index (string descriptor index, ...) */
    UINT16 LanguageId;     /* for string descriptors, 0x0409 = en-US */
} VHID_DESC_REQ, *PVHID_DESC_REQ;

#define IOCTL_VUSBBUS_GET_USB_DESCRIPTOR \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_READ_ACCESS)

/* vhidkm.sys control IOCTLs (0x900..0x9FF). */

#define IOCTL_VHIDKM_GET_VERSION \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)

/*
 * IOCTL_VHIDKM_KEYBOARD_REPORT
 *   Input:  VHID_KEYBOARD_INPUT_REPORT (9 bytes including ReportId)
 *   Output: none
 *   Raw-report injection: the caller hands the driver a fully formed
 *   keyboard input report, the driver forwards it straight into the
 *   pending-read queue. Expected ReportId is VHID_REPORTID_KEYBOARD_INPUT;
 *   mismatched report ids fail with STATUS_INVALID_PARAMETER.
 */
#define IOCTL_VHIDKM_KEYBOARD_REPORT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/*
 * IOCTL_VHIDKM_KEYBOARD_KEYS
 *   Input:  VHID_KBD_KEYS_REQ
 *   Output: none
 *   Ergonomic wrapper: caller supplies a modifier bitmap and up to
 *   six key usages; driver builds the report. Useful for callers
 *   that don't want to include pshpack1.h/poppack.h headers.
 */
typedef struct _VHID_KBD_KEYS_REQ {
    UINT32 Size;
    UINT8  Modifiers;
    UINT8  Reserved;
    UINT8  Keys[VHID_KBD_MAX_KEYS];
} VHID_KBD_KEYS_REQ, *PVHID_KBD_KEYS_REQ;

#define IOCTL_VHIDKM_KEYBOARD_KEYS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/*
 * IOCTL_VHIDKM_KEY_STROKE
 *   Input:  VHID_KEYSTROKE_REQ
 *   Output: none
 *   Press the given usage with the given modifiers, hold for HoldMs
 *   milliseconds, then release. HoldMs is clamped to [0, 5000] at the
 *   kernel boundary so the bounded wait cannot be abused. The call is
 *   synchronous: it blocks the requesting thread for the hold duration
 *   and returns only after the release report has been enqueued.
 */
typedef struct _VHID_KEYSTROKE_REQ {
    UINT32 Size;
    UINT8  Modifiers;
    UINT8  Usage;
    UINT16 Reserved;
    UINT32 HoldMs;         /* clamped [0, 5000] */
} VHID_KEYSTROKE_REQ, *PVHID_KEYSTROKE_REQ;

#define IOCTL_VHIDKM_KEY_STROKE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/*
 * IOCTL_VHIDKM_MOUSE_REL
 *   Input:  VHID_MOUSE_REL_REPORT (8 bytes)
 *   Output: none
 *   Raw relative-mouse injection. ReportId must be
 *   VHID_REPORTID_MOUSE_REL.
 */
#define IOCTL_VHIDKM_MOUSE_REL \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x910, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/*
 * IOCTL_VHIDKM_MOUSE_ABS
 *   Input:  VHID_MOUSE_ABS_REPORT (8 bytes)
 *   Output: none
 *   Raw absolute-mouse injection. X/Y are logical 0..VHID_MOUSE_ABS_MAX.
 */
#define IOCTL_VHIDKM_MOUSE_ABS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x911, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/*
 * IOCTL_VHIDKM_MOUSE_ABS_PX
 *   Input:  VHID_MOUSE_ABS_PX_REQ
 *   Output: none
 *   Pixel-coordinate absolute-mouse injection. Driver converts pixels
 *   to logical units using the screen metrics most recently set on
 *   THIS file handle via IOCTL_VHIDKM_SET_SCREEN_METRICS.
 *   Handles without metrics set get STATUS_INVALID_DEVICE_STATE.
 */
typedef struct _VHID_MOUSE_ABS_PX_REQ {
    UINT32 Size;
    UINT8  Buttons;
    INT8   Wheel;
    INT8   HWheel;
    UINT8  Reserved;
    INT32  XPx;
    INT32  YPx;
} VHID_MOUSE_ABS_PX_REQ, *PVHID_MOUSE_ABS_PX_REQ;

#define IOCTL_VHIDKM_MOUSE_ABS_PX \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x912, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/*
 * IOCTL_VHIDKM_SET_SCREEN_METRICS
 *   Input:  VHID_SCREEN_METRICS
 *   Output: none
 *   Records the virtual-screen bounding box for subsequent
 *   IOCTL_VHIDKM_MOUSE_ABS_PX calls on the same handle. Metrics are
 *   stored in the per-file object context; closing the handle clears
 *   them. Width/Height of zero are rejected to avoid a divide-by-zero
 *   in the conversion path.
 */
typedef struct _VHID_SCREEN_METRICS {
    UINT32 Size;
    INT32  VirtualX;       /* GetSystemMetrics(SM_XVIRTUALSCREEN) */
    INT32  VirtualY;       /* GetSystemMetrics(SM_YVIRTUALSCREEN) */
    INT32  VirtualWidth;   /* GetSystemMetrics(SM_CXVIRTUALSCREEN) */
    INT32  VirtualHeight;  /* GetSystemMetrics(SM_CYVIRTUALSCREEN) */
} VHID_SCREEN_METRICS, *PVHID_SCREEN_METRICS;

#define IOCTL_VHIDKM_SET_SCREEN_METRICS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x913, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/*
 * IOCTL_VHIDKM_GET_LED_STATE
 *   Input:  none
 *   Output: UINT8 (bit layout matches VHID_KBD_LED_*)
 *   Non-blocking: returns the last observed LED state from
 *   hidclass.sys's output-report path.
 */
#define IOCTL_VHIDKM_GET_LED_STATE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x920, METHOD_BUFFERED, FILE_READ_ACCESS)

/*
 * IOCTL_VHIDKM_WAIT_LED_CHANGE
 *   Input:  UINT8 baseline
 *   Output: UINT8 new state
 *   Blocks the calling thread (via pending IRP, cancellable) until
 *   the LED state differs from the supplied baseline. If the current
 *   state already differs the IOCTL returns immediately. Cancelable
 *   with CancelIoEx / CloseHandle. Multiple concurrent waiters are
 *   supported; each wakes on a change.
 */
#define IOCTL_VHIDKM_WAIT_LED_CHANGE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x921, METHOD_BUFFERED, FILE_READ_ACCESS)

/*
 * IOCTL_VHIDKM_RESET
 *   Input:  none
 *   Output: none
 *   Immediately enqueues an all-up keyboard report (zero modifiers,
 *   zero keys) and an all-up relative-mouse report (zero buttons,
 *   zero deltas). The IOCTL dispatcher also invokes this path on
 *   file-handle cleanup so a process crashing with a key held does
 *   not leave the key stuck.
 */
#define IOCTL_VHIDKM_RESET \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x930, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#include <poppack.h>

/*
 * Cross-check the struct sizes at compile time so the ABI cannot
 * drift without a build failure. The numbers in the comment column
 * are the canonical layout — change them only alongside a
 * VHID_API_LEVEL bump.
 */
#if defined(C_ASSERT)
C_ASSERT(sizeof(VHID_VERSION)         == 16);
C_ASSERT(sizeof(VHID_PLUGIN_REQ)      == 12 + 32 * sizeof(WCHAR));
C_ASSERT(sizeof(VHID_PLUGIN_RESP)     == 4 + sizeof(GUID));
C_ASSERT(sizeof(VHID_UNPLUG_REQ)      == 8);
C_ASSERT(sizeof(VHID_DESC_REQ)        == 8);
C_ASSERT(sizeof(VHID_KBD_KEYS_REQ)    == 12);
C_ASSERT(sizeof(VHID_KEYSTROKE_REQ)   == 12);
C_ASSERT(sizeof(VHID_MOUSE_ABS_PX_REQ)== 16);
C_ASSERT(sizeof(VHID_SCREEN_METRICS)  == 20);
#endif

#endif /* VHID_IOCTL_H_ */
