/*
 * bus/pdo.h
 *
 * Declarations for the virtual USB child PDO.
 *
 * Each PDO carries:
 *   - Its own framework handle, captured at creation, so the published
 *     IPC interface reference/dereference callbacks act on a stable
 *     object rather than the mutable slot table entry.
 *   - A backpointer to its VUSBBUS_SLOT entry (so cleanup can mark the
 *     slot free when the device is removed).
 *   - A captured copy of the per-instance serial used to answer
 *     iSerialNumber over the IPC descriptor path without touching the
 *     slot.
 *   - Published bus IPC state (function-ready flag, LED baseline,
 *     bound HID FDO pointer, spinlock).
 *
 * PDOs are fully KMDF-managed (no raw IoCreateDevice); the non-trivial
 * WDM IRPs (QUERY_INTERFACE for our published bus IPC) are wired via
 * WdfDeviceInitAssignWdmIrpPreprocessCallback.
 */

#pragma once
#ifndef VHID_BUS_PDO_H_
#define VHID_BUS_PDO_H_

#include "driver.h"
#include "fdo.h"

struct _VUSBBUS_PDO_CONTEXT {
    /*
     * This PDO's own framework handle, captured at creation. The IPC
     * interface reference/dereference callbacks ref/deref THIS handle
     * rather than chasing Slot->PdoDevice, which the unplug path zeroes
     * and a subsequent plug may recycle to a different device.
     */
    WDFDEVICE          Pdo;

    /* Backpointer to the owning slot (cleared by cleanup). */
    PVUSBBUS_SLOT      Slot;
    PVUSBBUS_FDO_CONTEXT FdoCtx;

    /*
     * Per-instance serial captured at creation so the published IPC
     * descriptor path can serve iSerialNumber without dereferencing the
     * mutable slot. Not guaranteed null-terminated; SerialChars records
     * the populated length (0 means "use the default serial string").
     */
    WCHAR              Serial[32];
    ULONG              SerialChars;

    /*
     * Serializes access to FunctionReady, HidFdo, LedBaseline,
     * LedWaiters. Acquired at DISPATCH_LEVEL by LED propagation,
     * PASSIVE_LEVEL by PnP / IPC setup. A spinlock suffices -- none of
     * the critical sections wait on anything external.
     */
    WDFSPINLOCK        IpcLock;

    BOOLEAN            FunctionReady;
    PDEVICE_OBJECT     HidFdo;         /* weak ref; set by NotifyFunctionReady */
    UCHAR              LedBaseline;    /* last observed LED bits */

    /* Manual queue of pending IOCTL_VUSBBUS_WAIT_LED_CHANGE IRPs. */
    WDFQUEUE           LedWaitQueue;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VUSBBUS_PDO_CONTEXT, VusbBusPdoGetContext);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VusbBusPdoCreate(
    _In_  PVUSBBUS_FDO_CONTEXT FdoCtx,
    _In_  PVUSBBUS_SLOT        Slot,
    _Out_ WDFDEVICE*           PdoDevice
    );

#endif /* VHID_BUS_PDO_H_ */
