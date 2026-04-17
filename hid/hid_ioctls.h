/*
 * hid/hid_ioctls.h
 *
 * EvtIoInternalDeviceControl dispatcher bindings for the HID
 * minidriver. These IOCTLs come exclusively from hidclass.sys — the
 * PnP manager attaches hidclass as an upper class filter and the
 * class driver sends IOCTL_HID_* on the driver's default queue to
 * retrieve descriptors, read reports, and forward output/feature
 * reports.
 *
 * Every IOCTL handler here is a thin adapter that:
 *
 *   1. Validates buffer lengths (short buffers -> STATUS_BUFFER_TOO_SMALL
 *      with Information = required bytes, matching the contract
 *      hidclass.sys expects so it can re-issue with a larger buffer).
 *
 *   2. Retrieves the output buffer via WdfRequestRetrieveOutputBuffer
 *      (or input for SET_* operations). Under METHOD_NEITHER — which
 *      several HID-internal IOCTLs use — the framework still returns
 *      the kernel-side pointer because hidclass.sys hands us a
 *      SystemBuffer.
 *
 *   3. Delegates to a targeted helper (descriptor copy, queue pend,
 *      LED update) rather than inlining logic here.
 */

#pragma once
#ifndef VHID_HID_IOCTLS_H_
#define VHID_HID_IOCTLS_H_

#include "driver.h"
#include "device.h"

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VhidkmHidIoctlInitialize(
    _In_ PVHIDKM_DEVICE_CONTEXT DevCtx
    );

#endif /* VHID_HID_IOCTLS_H_ */
