/*
 * hid/driver.h
 *
 * Top-level declarations for vhidkm.sys.
 *
 * The HID minidriver binds as an FDO on top of the USB-looking PDO
 * created by vusbbus.sys. hidclass.sys attaches above the minidriver
 * as an upper class filter and fans the device out into keyboard
 * and mouse child PDOs consumed by kbdhid.sys / mouhid.sys.
 *
 * Driver lifecycle summary:
 *
 *   DriverEntry
 *     └─> WPP_INIT_TRACING
 *         WdfDriverCreate
 *
 *   EvtDriverDeviceAdd (invoked per PDO to bind against)
 *     └─> VhidkmDeviceCreate (device.c)
 *           creates the function FDO
 *           queries the bus IPC interface (bus_iface.c)
 *           allocates the pending-read queue (report_queue.c)
 *           creates the control device (ctl_device.c)
 *
 *   EvtDriverContextCleanup
 *     └─> WPP_CLEANUP
 */

#pragma once
#ifndef VHID_HID_DRIVER_H_
#define VHID_HID_DRIVER_H_

#include <ntifs.h>
#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>

#include "vhid_guids.h"
#include "vhid_version.h"
#include "vhid_ioctl.h"
#include "vhid_protocol.h"
#include "../bus/public.h"

typedef struct _VHIDKM_DEVICE_CONTEXT VHIDKM_DEVICE_CONTEXT, *PVHIDKM_DEVICE_CONTEXT;
typedef struct _VHIDKM_CTL_FILE_CONTEXT VHIDKM_CTL_FILE_CONTEXT, *PVHIDKM_CTL_FILE_CONTEXT;
typedef struct _VHIDKM_CTL_DEV_CONTEXT VHIDKM_CTL_DEV_CONTEXT, *PVHIDKM_CTL_DEV_CONTEXT;

DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD       VhidkmEvtDriverDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP  VhidkmEvtDriverContextCleanup;

#endif /* VHID_HID_DRIVER_H_ */
