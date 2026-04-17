/*
 * bus/pdo_iface.h
 *
 * Published bus IPC interface plumbing on the PDO side.
 *
 * The HID minidriver issues IRP_MN_QUERY_INTERFACE on the PDO with
 * GUID_VHID_BUS_INTERFACE_V1. KMDF does not expose an EvtDevice
 * callback for QUERY_INTERFACE, so we register a WDM IRP preprocess
 * callback in the PDOINIT and intercept this IRP ourselves.
 *
 * Separately, the USB-descriptor read-through and LED broadcast
 * primitives exposed through the interface are implemented here.
 */

#pragma once
#ifndef VHID_BUS_PDO_IFACE_H_
#define VHID_BUS_PDO_IFACE_H_

#include "driver.h"
#include "pdo.h"

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
VusbBusPdoSetInterfaceDispatch(
    _In_ PWDFDEVICE_INIT DeviceInit
    );

#endif /* VHID_BUS_PDO_IFACE_H_ */
