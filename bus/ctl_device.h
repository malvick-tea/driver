/*
 * bus/ctl_device.h
 *
 * Control-device creation for vusbbus.sys. The control device is a
 * WDFDEVICE with no PnP role; user-mode opens it by
 * GUID_DEVINTERFACE_VUSBBUS_CTL to issue plug/unplug/list IOCTLs.
 *
 * SDDL:
 *   D:P(A;;GA;;;SY)(A;;GA;;;BA) — SYSTEM + Administrators only.
 *   Applied via WdfControlDeviceInitSetSddlString.
 */

#pragma once
#ifndef VHID_BUS_CTL_DEVICE_H_
#define VHID_BUS_CTL_DEVICE_H_

#include "driver.h"
#include "fdo.h"

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VusbBusCtlDeviceCreate(
    _In_ PVUSBBUS_FDO_CONTEXT FdoCtx
    );

/*
 * Clear the singleton control device's backpointer if it still refers
 * to FdoCtx. Called from the bus FDO teardown path. Idempotent.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
VusbBusCtlDetachFdo(
    _In_ PVUSBBUS_FDO_CONTEXT FdoCtx
    );

/*
 * Resolve the control device's current FDO context and take a reference
 * on its WDFDEVICE so the context cannot be freed while the caller uses
 * it. Returns NULL if no FDO currently backs the control device. Each
 * non-NULL return must be balanced with VusbBusCtlReleaseFdo.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
PVUSBBUS_FDO_CONTEXT
VusbBusCtlAcquireFdo(
    _In_ WDFDEVICE ControlDevice
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
VusbBusCtlReleaseFdo(
    _In_ PVUSBBUS_FDO_CONTEXT FdoCtx
    );

/* Per-file context for the control device (unused slots for now). */
typedef struct _VUSBBUS_CTL_FILE_CONTEXT_T {
    ULONG Reserved;
} VUSBBUS_CTL_FILE_CONTEXT_T;
/* Alias for cross-file readability. */
#define VUSBBUS_CTL_FILE_CONTEXT VUSBBUS_CTL_FILE_CONTEXT_T

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VUSBBUS_CTL_FILE_CONTEXT_T, VusbBusCtlFileGetContext);

#endif /* VHID_BUS_CTL_DEVICE_H_ */
