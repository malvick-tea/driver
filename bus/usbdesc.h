/*
 * bus/usbdesc.h
 *
 * Canonical USB descriptor tables served through the bus IPC
 * interface and the diagnostic IOCTL_VUSBBUS_GET_USB_DESCRIPTOR.
 *
 * The bytes live in read-only data in usbdesc.c and are wrapped by
 * a single copy helper so callers cannot accidentally mutate them
 * or over-read past the descriptor boundary.
 */

#pragma once
#ifndef VHID_BUS_USBDESC_H_
#define VHID_BUS_USBDESC_H_

#include "driver.h"

/* Descriptor type constants used by the copy helper. */
#define USB_DESC_TYPE_DEVICE         0x01
#define USB_DESC_TYPE_CONFIGURATION  0x02
#define USB_DESC_TYPE_STRING         0x03
#define USB_DESC_TYPE_INTERFACE      0x04
#define USB_DESC_TYPE_ENDPOINT       0x05
#define USB_DESC_TYPE_HID            0x21
#define USB_DESC_TYPE_REPORT         0x22

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
VusbBusUsbDescCopy(
    _In_  UCHAR DescriptorType,
    _In_  UCHAR DescriptorIndex,
    _Out_writes_bytes_to_(BufferLength, *BytesReturned) PVOID Buffer,
    _In_  ULONG BufferLength,
    _Out_ PULONG BytesReturned
    );

#endif /* VHID_BUS_USBDESC_H_ */
