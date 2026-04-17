/*
 * hid/ctl_device.h
 *
 * Control-device plumbing for vhidkm.sys.
 *
 * The control device is a standalone WDFDEVICE (not tied to the PnP
 * tree) through which user-mode issues IOCTL_VHIDKM_* — input-
 * injection, LED readback, screen metrics. It carries an SDDL string
 * restricting open to SYSTEM and BUILTIN\Administrators and
 * publishes the device-interface GUID_DEVINTERFACE_VHIDKM_CTL so
 * user-mode can enumerate it with SetupDiGetClassDevs.
 *
 * Lifetime:
 *   Created lazily by the first FDO to reach EvtDeviceAdd and torn
 *   down when the driver unloads. Subsequent FDOs share the same
 *   control device — a single user-mode handle can talk to whichever
 *   of (currently one) plugged instance is backing the injection
 *   calls. The control device carries a pointer to the active
 *   VHIDKM_DEVICE_CONTEXT in its own device context.
 *
 * Per-handle state:
 *   Each open file carries screen metrics (set via
 *   IOCTL_VHIDKM_SET_SCREEN_METRICS) so pixel-coordinate injection
 *   can be converted to HID logical units without relying on process-
 *   wide state. File cleanup flushes an all-up reset report to avoid
 *   stuck keys when the controlling process exits.
 */

#pragma once
#ifndef VHID_HID_CTL_DEVICE_H_
#define VHID_HID_CTL_DEVICE_H_

#include "driver.h"

struct _VHIDKM_DEVICE_CONTEXT;

/* Per-handle context on the control device. */
typedef struct _VHIDKM_CTL_FILE_CONTEXT {
    BOOLEAN  HasMetrics;
    INT32    VirtualX;
    INT32    VirtualY;
    INT32    VirtualWidth;
    INT32    VirtualHeight;
} VHIDKM_CTL_FILE_CONTEXT_T, *PVHIDKM_CTL_FILE_CONTEXT_T;

/*
 * Redeclaration under the forward-declared tag in driver.h so other
 * sources can use the type name without pulling the structure body.
 */
struct _VHIDKM_CTL_FILE_CONTEXT {
    VHIDKM_CTL_FILE_CONTEXT_T Inner;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHIDKM_CTL_FILE_CONTEXT, VhidkmCtlFileGetContext);

/*
 * Per-control-device context. Holds a pointer to the active
 * VHIDKM_DEVICE_CONTEXT (the FDO backing the virtual USB device).
 * v1 is single-slot so this pointer is set once; multi-slot builds
 * would wrap it in a lookup by slot id.
 */
struct _VHIDKM_CTL_DEV_CONTEXT {
    struct _VHIDKM_DEVICE_CONTEXT* DevCtx;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHIDKM_CTL_DEV_CONTEXT, VhidkmCtlDevGetContext);

/*
 * Create the control device exactly once per driver lifetime. Safe
 * to call from multiple FDO create paths — the implementation
 * serializes on a driver-wide once-flag.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VhidkmCtlDeviceCreateOnce(
    _In_ WDFDEVICE                        AssociatedFdo,
    _In_ struct _VHIDKM_DEVICE_CONTEXT*   DevCtx
    );

#endif /* VHID_HID_CTL_DEVICE_H_ */
