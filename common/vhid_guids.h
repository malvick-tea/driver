/*
 * vhid_guids.h
 *
 * Well-known identifiers shared between kernel and user mode:
 *
 *   - Device-interface GUIDs published by each driver's control device.
 *     User-mode uses SetupDiGetClassDevs / SetupDiEnumDeviceInterfaces
 *     with these GUIDs to discover the endpoints.
 *
 *   - Published bus IPC interface GUID. The HID minidriver issues
 *     IRP_MN_QUERY_INTERFACE on the PDO with this GUID to receive a
 *     VHID_BUS_INTERFACE_V1 function table from the bus driver (see
 *     bus/public.h).
 *
 *   - PnP hardware/compatible ID strings used by the bus driver when
 *     constructing the USB-looking PDO. These strings must match the
 *     values declared in the HID minidriver's INF.
 *
 *   - Pool tags used by both drivers. Two distinct 4-char tags make it
 *     trivial to isolate allocations per driver when reading a dump
 *     with `!poolused`.
 *
 * The file is included by kernel-mode sources (NTDDK / wdm.h already
 * pulls DEFINE_GUID) and by user-mode sources (INITGUID + guiddef.h).
 * The two-include pattern used in Win32 SDK headers (include once to
 * declare, re-include under INITGUID to instantiate) is not used here
 * because the surface is tiny and each consumer knows its context.
 */

#pragma once
#ifndef VHID_GUIDS_H_
#define VHID_GUIDS_H_

#if defined(_KERNEL_MODE) || defined(_NTDDK_)
#include <ntddk.h>
#else
#include <windows.h>
#include <initguid.h>
#include <guiddef.h>
#endif

/*
 * Interface GUID exposed by the vusbbus.sys control device.
 *   {B4A8F7E3-2E6A-4C1B-9D5F-9F3D2E7A1C01}
 * Used by user-mode admin tools to enumerate the bus control endpoint
 * and to issue plug/unplug/list IOCTLs.
 */
DEFINE_GUID(GUID_DEVINTERFACE_VUSBBUS_CTL,
    0xb4a8f7e3, 0x2e6a, 0x4c1b, 0x9d, 0x5f, 0x9f, 0x3d, 0x2e, 0x7a, 0x1c, 0x01);

/*
 * Interface GUID exposed by the vhidkm.sys control device.
 *   {C5B9F804-3F7B-4D2C-AE60-A04E3F8B2D02}
 * User-mode input-injection agents open this interface to feed reports.
 */
DEFINE_GUID(GUID_DEVINTERFACE_VHIDKM_CTL,
    0xc5b9f804, 0x3f7b, 0x4d2c, 0xae, 0x60, 0xa0, 0x4e, 0x3f, 0x8b, 0x2d, 0x02);

/*
 * Bus IPC interface GUID. Not published to user mode.
 *   {D6CAE915-4F8C-4E3D-BF71-B15F4F9C3E03}
 * Queried by vhidkm.sys on the PDO with IRP_MN_QUERY_INTERFACE to fetch
 * a VHID_BUS_INTERFACE_V1 function table from the bus driver.
 */
DEFINE_GUID(GUID_VHID_BUS_INTERFACE_V1,
    0xd6cae915, 0x4f8c, 0x4e3d, 0xbf, 0x71, 0xb1, 0x5f, 0x4f, 0x9c, 0x3e, 0x03);

/*
 * Well-known hardware / compatible ID strings. The bus driver assigns
 * these via WdfPdoInit* APIs (see bus/pdo_ids.c). The HID minidriver's
 * INF declares matching entries under its [Manufacturer]/[Models] so
 * PnP binds vhidkm.sys on the PDO the bus driver creates.
 *
 * The REV suffix encodes bcdDevice and must mirror VHID_DEFAULT_REV.
 * Changing those values without updating the INF will break matching.
 */
#define VHID_HWID_PRIMARY_W      L"USB\\VID_1209&PID_BEEF&REV_0100"
#define VHID_HWID_GENERIC_W      L"USB\\VID_1209&PID_BEEF"
#define VHID_COMPATID_HID_W      L"USB\\Class_03&SubClass_00&Prot_00"
#define VHID_COMPATID_HID_GEN_W  L"USB\\Class_03"

/*
 * Pool tags. Read them as ASCII little-endian: 'diHV' -> "VHid".
 *   VHID_POOL_TAG_BUS  -> "VHbu"
 *   VHID_POOL_TAG_HID  -> "VHhi"
 * Each driver uses its own tag so `!poolused VHbu` / `!poolused VHhi`
 * cleanly attribute allocations in a dump.
 */
#define VHID_POOL_TAG_BUS   ((ULONG)'ubHV')
#define VHID_POOL_TAG_HID   ((ULONG)'ihHV')

#endif /* VHID_GUIDS_H_ */
