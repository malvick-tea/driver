/*
 * hid/bus_iface.h
 *
 * Client side of the published bus IPC interface.
 *
 * During EvtDevicePrepareHardware, the HID minidriver issues an
 * IRP_MN_QUERY_INTERFACE with GUID_VHID_BUS_INTERFACE_V1 against the
 * PDO it binds on top of. The bus driver populates a
 * VHID_BUS_INTERFACE_V1 function table (see bus/public.h) and hands
 * a reference back via the INTERFACE_HEADER InterfaceReference
 * mechanism. We cache the table in the device context; every call
 * thereafter invokes the stashed function pointer directly.
 *
 * Release is symmetric: during EvtDeviceReleaseHardware we invoke
 * InterfaceDereference to drop our reference, and null out the
 * cached pointer to prevent use-after-free if a late callback races
 * the tear-down.
 *
 * The caller is responsible for holding no lock while invoking any
 * interface method; the bus-side implementations may acquire bus-
 * owned locks. The lock hierarchy in docs/ARCHITECTURE.md places
 * all HID-owned locks below the bus locks, so the "no locks held"
 * rule preserves the hierarchy.
 */

#pragma once
#ifndef VHID_HID_BUS_IFACE_H_
#define VHID_HID_BUS_IFACE_H_

#include "driver.h"
#include "device.h"

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VhidkmBusIfaceAcquire(
    _Inout_ PVHIDKM_DEVICE_CONTEXT DevCtx
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
VhidkmBusIfaceRelease(
    _Inout_ PVHIDKM_DEVICE_CONTEXT DevCtx
    );

#endif /* VHID_HID_BUS_IFACE_H_ */
