/*
 * hid/report_queue.h
 *
 * Interrupt-IN endpoint emulation.
 *
 * A real USB HID device delivers input reports over an interrupt-IN
 * endpoint that the host polls every bInterval milliseconds. In our
 * software-only model, hidclass.sys issues one outstanding
 * IOCTL_HID_READ_REPORT at a time, waits for completion, then issues
 * another. This file holds:
 *
 *   1. A manual KMDF IO queue that parks those pending reads until
 *      input-injection fills them.
 *
 *   2. A small lossy ring buffer that holds injected reports when no
 *      read IRP is pending (the class driver is momentarily between
 *      issuing one read and posting the next; any burst of injection
 *      greater than 1 report during that window would otherwise be
 *      lost). Overflow drops the oldest entry — matching the lossy
 *      behaviour of a real interrupt endpoint when the host host can
 *      not keep up.
 *
 * Concurrency:
 *   All queue/ring manipulation is serialised by DevCtx->ReportQueueLock
 *   (a WDFSPINLOCK). Callers may invoke at PASSIVE or DISPATCH level;
 *   the spinlock sidesteps the need for dedicated wait-lock vs spin
 *   lock branching in the caller.
 */

#pragma once
#ifndef VHID_HID_REPORT_QUEUE_H_
#define VHID_HID_REPORT_QUEUE_H_

#include "driver.h"

struct _VHIDKM_DEVICE_CONTEXT;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VhidkmReportQueueInitialize(
    _In_ struct _VHIDKM_DEVICE_CONTEXT* DevCtx
    );

/*
 * Park a pending IOCTL_HID_READ_REPORT. If the ring buffer has a
 * queued report, the IRP is completed immediately with that report;
 * otherwise it is forwarded to the manual queue.
 *
 * Returns STATUS_PENDING when the IRP was parked (the caller must
 * NOT complete it); any other status indicates the caller should
 * complete the IRP with the returned status.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
VhidkmReportQueueForwardRead(
    _In_ struct _VHIDKM_DEVICE_CONTEXT* DevCtx,
    _In_ WDFREQUEST                     Request
    );

/*
 * Post a freshly-built input report. If an IOCTL_HID_READ_REPORT is
 * pending, the report is handed straight to it and the IRP
 * completes. Otherwise the report is placed in the ring buffer;
 * overflow drops the oldest entry.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
VhidkmReportQueuePost(
    _In_ struct _VHIDKM_DEVICE_CONTEXT* DevCtx,
    _In_reads_bytes_(ReportLength) const UCHAR* ReportData,
    _In_ UCHAR ReportLength
    );

/*
 * Synchronous peek for IOCTL_HID_GET_INPUT_REPORT. Returns the most
 * recent report whose first byte equals ReportId. No IRP is pended;
 * if nothing matches, returns STATUS_NO_MORE_ENTRIES and Written = 0.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
VhidkmReportQueuePeek(
    _In_ struct _VHIDKM_DEVICE_CONTEXT* DevCtx,
    _In_ UCHAR ReportId,
    _Out_writes_bytes_to_(BufferLength, *Written) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG Written
    );

/*
 * Cancel every pending read IRP with STATUS_DEVICE_REMOVED. Safe to
 * call repeatedly. Invoked from ReleaseHardware, D3Final D0Exit,
 * and SurpriseRemoval to make sure the upper stack doesn't wait on
 * a dead device.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
VhidkmReportQueueDrain(
    _In_ struct _VHIDKM_DEVICE_CONTEXT* DevCtx
    );

#endif /* VHID_HID_REPORT_QUEUE_H_ */
