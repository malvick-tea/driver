/*
 * bus/public.h
 *
 * Published IPC interface exposed by vusbbus.sys to vhidkm.sys over
 * the IRP_MN_QUERY_INTERFACE mechanism. The interface GUID is
 * GUID_VHID_BUS_INTERFACE_V1 (see common/vhid_guids.h).
 *
 * The two drivers communicate exclusively through this table. The
 * HID minidriver queries for the interface during EvtDevicePrepareHardware
 * and uses the returned function pointers for:
 *
 *   - Descriptor lookup — the bus driver owns the canonical USB/HID
 *     descriptor data; the HID minidriver does not duplicate it.
 *
 *   - Function-ready signalling — lets the bus driver know the HID
 *     FDO is alive and can be reached for diagnostic callbacks.
 *
 *   - LED change propagation — when hidclass.sys delivers an output
 *     report, the HID minidriver parses the LED byte and forwards it
 *     to the bus driver's waiter list.
 *
 *   - Surprise-unplug notification — fires into the HID minidriver
 *     when the user issues IOCTL_VUSBBUS_UNPLUG so the HID side can
 *     drain pending reads before PnP pulls it.
 *
 * This header is part of the bus driver's public surface. It is
 * included by both drivers and must not contain any KMDF / WDF type
 * references that the consuming driver hasn't already pulled in.
 * The only WDK types referenced are from wdm.h / ntddk.h.
 */

#pragma once
#ifndef VHID_BUS_PUBLIC_H_
#define VHID_BUS_PUBLIC_H_

#include <ntddk.h>
#include "vhid_guids.h"

/*
 * Version cookie stamped into VHID_BUS_INTERFACE_V1.InterfaceHeader.Version.
 * Bump when a field is added or semantics change in a breaking way.
 * The minor byte is used for additive backwards-compatible changes.
 */
#define VHID_BUS_INTERFACE_VERSION_V1    0x0001

/*
 * Function-pointer table handed to the HID minidriver. All routines
 * run at PASSIVE_LEVEL on the caller's thread; none of them block on
 * work items that run at higher IRQL. Callers may invoke them from
 * any thread context provided they are at PASSIVE_LEVEL.
 *
 * Context pointer lifetime follows the INTERFACE_HEADER InterfaceReference /
 * InterfaceDereference contract: the caller takes a reference when it
 * receives the interface, drops it when it is done. Until the last
 * deref, Context points at a valid bus FDO extension.
 */

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(*VHID_BUS_GET_USB_DESCRIPTOR)(
    _In_ PVOID Context,
    _In_ UCHAR DescriptorType,
    _In_ UCHAR DescriptorIndex,
    _In_reads_bytes_(BufferLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG BytesReturned
    );

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(*VHID_BUS_NOTIFY_FUNCTION_READY)(
    _In_ PVOID Context,
    _In_ PDEVICE_OBJECT HidFdo
    );

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
(*VHID_BUS_NOTIFY_LED_CHANGE)(
    _In_ PVOID Context,
    _In_ UCHAR LedBits
    );

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(*VHID_BUS_ON_UNPLUG)(
    _In_ PVOID Context
    );

typedef struct _VHID_BUS_INTERFACE_V1 {
    INTERFACE                       InterfaceHeader;
    VHID_BUS_GET_USB_DESCRIPTOR     GetUsbDescriptor;
    VHID_BUS_NOTIFY_FUNCTION_READY  NotifyFunctionReady;
    VHID_BUS_NOTIFY_LED_CHANGE      NotifyLedChange;
    VHID_BUS_ON_UNPLUG              OnUnplug;
} VHID_BUS_INTERFACE_V1, *PVHID_BUS_INTERFACE_V1;

#endif /* VHID_BUS_PUBLIC_H_ */
