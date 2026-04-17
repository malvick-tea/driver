/*
 * bus/driver.h
 *
 * Driver-wide declarations for vusbbus.sys. The bus driver is a root
 * enumerator that creates a virtual USB-looking PDO on demand (via
 * IOCTL_VUSBBUS_PLUG_IN) and reports it to PnP.
 *
 * Project lock hierarchy (acquired top-down only):
 *
 *   BusFdoLock       — serializes slot table updates and PDO list walks.
 *   PdoSlotLock      — per-PDO state (IPC callback table, LED baseline).
 *   HidReportQueueLock — owned by vhidkm.sys; listed here so the
 *                        hierarchy document is complete even though
 *                        this driver never acquires it.
 *
 * Single WDFDRIVER, single WDFDEVICE (the bus FDO), zero-or-one
 * WDFCHILDLIST entries. v1 is single-slot; the device context is
 * still written as if it owned an arbitrary slot count so the
 * upgrade to multi-device requires only tweaking VHID_MAX_SLOTS and
 * a loop bound.
 */

#pragma once
#ifndef VHID_BUS_DRIVER_H_
#define VHID_BUS_DRIVER_H_

#include <ntifs.h>
#include <ntddk.h>
#include <wdf.h>

#include "vhid_guids.h"
#include "vhid_version.h"
#include "vhid_ioctl.h"
#include "vhid_protocol.h"
#include "public.h"

/* Forward declarations of context types defined in their owner .h */
typedef struct _VUSBBUS_FDO_CONTEXT VUSBBUS_FDO_CONTEXT, *PVUSBBUS_FDO_CONTEXT;
typedef struct _VUSBBUS_PDO_CONTEXT VUSBBUS_PDO_CONTEXT, *PVUSBBUS_PDO_CONTEXT;
typedef struct _VUSBBUS_CTL_FILE_CONTEXT VUSBBUS_CTL_FILE_CONTEXT, *PVUSBBUS_CTL_FILE_CONTEXT;

/*
 * EvtDriverUnload is declared here because DriverEntry wires it up.
 * The unload path is trivial (WPP cleanup + trace) because all
 * KMDF objects are parented to the WDFDRIVER and cleaned up
 * automatically by the framework.
 */

DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD VusbBusEvtDriverDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP VusbBusEvtDriverContextCleanup;

#endif /* VHID_BUS_DRIVER_H_ */
