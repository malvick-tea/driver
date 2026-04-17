/*
 * user/vhid.h
 *
 * User-mode C SDK for the VHID driver stack.
 *
 * The SDK is a thin, dependency-free wrapper around the two control
 * devices published by vusbbus.sys and vhidkm.sys. It provides:
 *
 *   - A single opaque handle (vhid_ctx) that owns the bus control
 *     handle, the HID control handle, the plugged-in slot id, and the
 *     cached device-interface path strings. Callers never touch the
 *     raw HANDLEs.
 *
 *   - A lifecycle pair (vhid_open / vhid_close) that orchestrates the
 *     full startup sequence documented in common/vhid_protocol.h:
 *     open bus, check API level, plug in, wait for the HID control
 *     interface to appear, open it, check API level. Teardown is the
 *     reverse plus an unplug.
 *
 *   - Typed helpers for every IOCTL exposed by vhidkm.sys, plus the
 *     bus-side plug/unplug/list helpers that admin tools need.
 *
 *   - Blocking and non-blocking flavors of wait-led-change. The
 *     blocking form is built on overlapped I/O so callers can honour
 *     a timeout and cancel cleanly if the caller's own lifecycle
 *     dictates it.
 *
 * The header is pure C and pure Win32 — no C++, no ATL, no COM. It is
 * safe to include in both C and C++ translation units. The library
 * itself links against setupapi.lib / cfgmgr32.lib (for device-
 * interface enumeration) and the usual kernel32/advapi32.
 *
 * Error model:
 *   Every function returns a DWORD status. Zero is success. Non-zero
 *   is either a Win32 error code (ERROR_*) surfaced by GetLastError
 *   on the underlying call, or an NTSTATUS passed through from
 *   DeviceIoControl. Callers that need the last error in Win32 form
 *   should consult GetLastError immediately on failure; the SDK does
 *   not clobber it.
 */

#pragma once
#ifndef VHID_USER_VHID_H_
#define VHID_USER_VHID_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <windows.h>
#include <stdint.h>

#include "../common/vhid_ioctl.h"
#include "../common/vhid_reports.h"
#include "../common/vhid_guids.h"
#include "../common/vhid_version.h"
#include "../common/vhid_protocol.h"

typedef struct vhid_ctx vhid_ctx;

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */

/*
 * Open and fully initialise a VHID session.
 *
 *   - Enumerates GUID_DEVINTERFACE_VUSBBUS_CTL and opens the bus
 *     control device. Fails with ERROR_SERVICE_NOT_ACTIVE if the bus
 *     driver is not installed / loaded.
 *   - Validates the bus driver's API level against VHID_API_LEVEL.
 *   - Issues IOCTL_VUSBBUS_PLUG_IN with caller-supplied VID/PID/
 *     Version (zero fields use protocol defaults). Retrieves the
 *     assigned slot id.
 *   - Waits up to plugin_timeout_ms for the HID control-device
 *     interface to appear (PnP can take up to a few seconds on first
 *     install) and opens it.
 *
 * On success *out_ctx receives a newly allocated context; the caller
 * must release it with vhid_close.
 */
DWORD vhid_open(
    uint16_t vid,
    uint16_t pid,
    uint16_t version,
    const wchar_t *serial_optional,   /* may be NULL, max 32 chars */
    uint32_t plugin_timeout_ms,
    vhid_ctx **out_ctx
    );

/*
 * Issue an unplug for the slot this context plugged in (best effort)
 * and release all associated resources. Safe to call with a NULL
 * argument.
 */
void vhid_close(vhid_ctx *ctx);

/* ------------------------------------------------------------------
 * Versioning
 * ------------------------------------------------------------------ */

DWORD vhid_get_bus_version(vhid_ctx *ctx, VHID_VERSION *out_ver);
DWORD vhid_get_hid_version(vhid_ctx *ctx, VHID_VERSION *out_ver);

/* ------------------------------------------------------------------
 * Screen metrics (required before vhid_mouse_abs_px)
 * ------------------------------------------------------------------ */

/*
 * Configure the virtual-screen bounding box the driver uses when
 * translating pixel coordinates from vhid_mouse_abs_px. The values
 * mirror the Win32 virtual-screen metrics retrieved via
 * GetSystemMetrics(SM_XVIRTUALSCREEN / SM_YVIRTUALSCREEN /
 * SM_CXVIRTUALSCREEN / SM_CYVIRTUALSCREEN).
 *
 * The driver stores the metrics in the per-file-object context; they
 * are scoped to this handle and do not leak to other processes.
 */
DWORD vhid_set_screen_metrics(
    vhid_ctx *ctx,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
    );

/*
 * Convenience form that queries GetSystemMetrics itself.
 */
DWORD vhid_set_screen_metrics_auto(vhid_ctx *ctx);

/* ------------------------------------------------------------------
 * Keyboard injection
 * ------------------------------------------------------------------ */

/*
 * Submit a fully formed keyboard input report. modifiers is a
 * VHID_KBD_MOD_* bitmap; keys is an array of up to VHID_KBD_MAX_KEYS
 * HID Usage Page 0x07 keycodes (zero = unused slot).
 */
DWORD vhid_keyboard(
    vhid_ctx *ctx,
    uint8_t modifiers,
    const uint8_t keys[VHID_KBD_MAX_KEYS]
    );

/*
 * Press a single key with the given modifiers for hold_ms milliseconds
 * (clamped by the driver to VHID_KEYSTROKE_HOLD_MAX_MS), then release.
 * Returns as soon as the press is queued; the release is scheduled
 * asynchronously by the driver.
 */
DWORD vhid_keystroke(
    vhid_ctx *ctx,
    uint8_t modifiers,
    uint8_t usage,
    uint32_t hold_ms
    );

/* ------------------------------------------------------------------
 * Mouse injection
 * ------------------------------------------------------------------ */

/*
 * Relative mouse report. dx/dy are signed 16-bit deltas. wheel/hwheel
 * are signed 8-bit values (positive scrolls up / right).
 */
DWORD vhid_mouse_rel(
    vhid_ctx *ctx,
    uint8_t buttons,
    int16_t dx,
    int16_t dy,
    int8_t wheel,
    int8_t hwheel
    );

/*
 * Absolute mouse report in logical coordinates (0..VHID_MOUSE_ABS_MAX).
 */
DWORD vhid_mouse_abs(
    vhid_ctx *ctx,
    uint8_t buttons,
    uint16_t x,
    uint16_t y,
    int8_t wheel,
    int8_t hwheel
    );

/*
 * Absolute mouse report in pixel coordinates. Requires
 * vhid_set_screen_metrics (or vhid_set_screen_metrics_auto) to have
 * been called successfully on this handle at least once.
 */
DWORD vhid_mouse_abs_px(
    vhid_ctx *ctx,
    uint8_t buttons,
    int32_t x_px,
    int32_t y_px,
    int8_t wheel,
    int8_t hwheel
    );

/* ------------------------------------------------------------------
 * LED state
 * ------------------------------------------------------------------ */

/*
 * Non-blocking: read the last LED byte observed from hidclass.sys.
 * *out_leds receives a VHID_KBD_LED_* bitmap.
 */
DWORD vhid_get_led_state(vhid_ctx *ctx, uint8_t *out_leds);

/*
 * Blocking: wait until the LED byte differs from the supplied
 * baseline. If timeout_ms is INFINITE, waits indefinitely; any other
 * value caps the wait. On timeout returns ERROR_TIMEOUT and leaves
 * *out_leds unmodified.
 */
DWORD vhid_wait_led_change(
    vhid_ctx *ctx,
    uint8_t baseline,
    uint32_t timeout_ms,
    uint8_t *out_leds
    );

/* ------------------------------------------------------------------
 * Reset
 * ------------------------------------------------------------------ */

/*
 * Emit an all-up keyboard + all-up relative-mouse report, clearing
 * any stuck keys or buttons the upper stack might have latched.
 */
DWORD vhid_reset(vhid_ctx *ctx);

/* ------------------------------------------------------------------
 * Bus-level admin helpers (optional)
 * ------------------------------------------------------------------ */

/*
 * Enumerate live bus slots. out_slots is a caller-allocated buffer of
 * max_slots entries; *out_count is set to the number of entries the
 * driver reports. If max_slots is too small the call returns
 * ERROR_INSUFFICIENT_BUFFER and *out_count holds the required count.
 */
DWORD vhid_list_slots(
    vhid_ctx *ctx,
    VHID_SLOT_INFO *out_slots,
    uint32_t max_slots,
    uint32_t *out_count
    );

/*
 * Retrieve the slot id associated with the current context.
 */
uint32_t vhid_current_slot_id(vhid_ctx *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VHID_USER_VHID_H_ */
