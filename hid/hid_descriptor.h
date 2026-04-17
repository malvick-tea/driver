/*
 * hid/hid_descriptor.h
 *
 * Canonical HID class descriptor and report descriptor for vhidkm.sys.
 *
 * The report descriptor is reproduced verbatim from
 * docs/ARCHITECTURE.md §6. Consumers read it through three entry
 * points so the raw bytes are never exported and so a future
 * switch-to-dynamic descriptor (e.g., emit different collections
 * based on plug-in options) can be performed without changing
 * call sites:
 *
 *   VhidkmHidGetClassDescriptor — fills the 9-byte HID class
 *     descriptor structure returned by IOCTL_HID_GET_DEVICE_DESCRIPTOR.
 *
 *   VhidkmHidGetReportDescriptorSize — returns the report descriptor
 *     size in bytes (used to size the user-mode buffer in two-phase
 *     IOCTL_HID_GET_REPORT_DESCRIPTOR flows).
 *
 *   VhidkmHidCopyReportDescriptor — copies up to BufferLength bytes
 *     into Buffer.
 */

#pragma once
#ifndef VHID_HID_DESCRIPTOR_H_
#define VHID_HID_DESCRIPTOR_H_

#include "driver.h"

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
VhidkmHidGetClassDescriptor(
    _Out_ PHID_DESCRIPTOR Descriptor
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
ULONG
VhidkmHidGetReportDescriptorSize(
    VOID
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
VhidkmHidCopyReportDescriptor(
    _Out_writes_bytes_(BufferLength) PVOID Buffer,
    _In_ ULONG BufferLength
    );

#endif /* VHID_HID_DESCRIPTOR_H_ */
