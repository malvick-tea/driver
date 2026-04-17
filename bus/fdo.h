/*
 * bus/fdo.h
 *
 * Bus-FDO context and public API. The FDO is the root-enumerated
 * device object that owns the virtual bus; it hosts the slot table,
 * the published bus IPC callbacks for all PDOs, and the handle to
 * the control device.
 *
 * Lifetime:
 *   - Created in VusbBusFdoCreate during EvtDriverDeviceAdd.
 *   - Torn down implicitly when PnP issues IRP_MN_REMOVE_DEVICE —
 *     KMDF frees all parented objects without explicit calls here.
 *
 * Slot table:
 *   An array of Slots[VHID_MAX_SLOTS]; each entry owns at most one
 *   PDO. v1 is single-device, so the array has one entry and the
 *   "is live" flag is effectively a boolean, but the code treats
 *   the array uniformly so the multi-device upgrade is mechanical.
 *
 * Locking:
 *   Slot-table mutation uses a WDFWAITLOCK (PASSIVE_LEVEL only).
 *   IPC dispatch inside a PDO uses a separate per-PDO spinlock
 *   because it must be reachable from DISPATCH_LEVEL (LED change
 *   propagation runs from the HID minidriver's report path).
 */

#pragma once
#ifndef VHID_BUS_FDO_H_
#define VHID_BUS_FDO_H_

#include "driver.h"

typedef struct _VUSBBUS_SLOT {
    BOOLEAN           InUse;
    UINT32            SlotId;
    GUID              InstanceId;
    UINT16            Vid;
    UINT16            Pid;
    UINT16            Version;
    WCHAR             Serial[32];

    /*
     * PdoDevice is a weak reference: the child PDO is parented to
     * the WDFDRIVER (so its lifetime matches the driver), but the
     * bus FDO references it by handle to route IOCTLs and to
     * re-evaluate bus relations. Cleared by PDO cleanup callback.
     */
    WDFDEVICE         PdoDevice;
} VUSBBUS_SLOT, *PVUSBBUS_SLOT;

struct _VUSBBUS_FDO_CONTEXT {
    WDFDEVICE         Fdo;
    WDFDEVICE         ControlDevice;

    /* Guards Slots[] mutation. Never held at DISPATCH_LEVEL. */
    WDFWAITLOCK       SlotLock;

    VUSBBUS_SLOT      Slots[VHID_MAX_SLOTS];
    UINT32            NextSlotId;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VUSBBUS_FDO_CONTEXT, VusbBusFdoGetContext);

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
VusbBusFdoCreate(
    _In_ PWDFDEVICE_INIT DeviceInit
    );

/*
 * Slot-table primitives. Called by the control-device IOCTL
 * handlers (queue.c) to implement PLUG_IN / UNPLUG / LIST.
 * Runs at PASSIVE_LEVEL only.
 */

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VusbBusFdoPlugIn(
    _In_ PVUSBBUS_FDO_CONTEXT FdoCtx,
    _In_ PCVHID_PLUGIN_REQ    Req,
    _Out_ PVHID_PLUGIN_RESP   Resp
    );

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VusbBusFdoUnplug(
    _In_ PVUSBBUS_FDO_CONTEXT FdoCtx,
    _In_ UINT32               SlotId
    );

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VusbBusFdoList(
    _In_ PVUSBBUS_FDO_CONTEXT FdoCtx,
    _Out_writes_bytes_to_(OutputLength, *BytesReturned) PVHID_SLOT_LIST List,
    _In_ ULONG                OutputLength,
    _Out_ PULONG              BytesReturned
    );

/* EvtDeviceContextCleanup — publicly declared so fdo.c can use it. */
EVT_WDF_OBJECT_CONTEXT_CLEANUP VusbBusFdoEvtCleanup;

#endif /* VHID_BUS_FDO_H_ */
