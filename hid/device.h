/*
 * hid/device.h
 *
 * Per-device (FDO) context and public creation API for vhidkm.sys.
 *
 * Each WDFDEVICE owns:
 *
 *   - A cached copy of the published bus IPC interface obtained by
 *     IRP_MN_QUERY_INTERFACE during PrepareHardware. The interface
 *     carries a reference which we drop in ReleaseHardware.
 *
 *   - Two manual KMDF queues:
 *       InputReportQueue - holds the pending IOCTL_HID_READ_REPORT
 *                          IRPs that hidclass.sys has posted to us;
 *                          we dequeue and complete them each time
 *                          the control-device dispatcher produces
 *                          a new report.
 *       LedWaitQueue     - holds IOCTL_VHIDKM_WAIT_LED_CHANGE
 *                          waiters pending an LED-state change.
 *
 *   - A report ring buffer (see report_queue.c) that smooths bursts
 *     when the upper stack is momentarily not draining the queue.
 *
 *   - Current LED baseline cached for the non-blocking
 *     IOCTL_VHIDKM_GET_LED_STATE path and as the seed for cancellable
 *     wait-for-change requests.
 *
 *   - A handle to the vhidkm.sys control device; creation of the
 *     control device is gated on the first FDO so concurrent plugs
 *     (if VHID_MAX_SLOTS ever exceeds 1) reuse a single process-wide
 *     control surface.
 *
 * Lock hierarchy (declared here for discoverability; enforced in code
 * review and in the per-source lock-acquire documentation):
 *
 *   ReportQueueLock  -> LedLock
 *   Holding neither when calling into the bus IPC.
 */

#pragma once
#ifndef VHID_HID_DEVICE_H_
#define VHID_HID_DEVICE_H_

#include "driver.h"
#include "report_queue.h"

/*
 * Static-size ring buffer for reports queued when hidclass.sys is not
 * yet draining. Power-of-two capacity so the index arithmetic is a
 * single AND.
 */
typedef struct _VHID_REPORT_RING {
    UCHAR       Entries[VHID_REPORT_RING_CAPACITY][VHID_MAX_REPORT_SIZE];
    UCHAR       Lengths[VHID_REPORT_RING_CAPACITY];
    ULONG       Head;        /* next index to read */
    ULONG       Tail;        /* next index to write */
    ULONG       Count;       /* populated entries in the ring */
} VHID_REPORT_RING, *PVHID_REPORT_RING;

struct _VHIDKM_DEVICE_CONTEXT {
    WDFDEVICE           Device;         /* the function FDO */
    PDEVICE_OBJECT      WdmDeviceObject;/* cached for PDEVICE_OBJECT-taking APIs */
    WDFDEVICE           ControlDevice;  /* weak — control device is standalone */

    /*
     * Cached bus IPC interface. Reference/dereference follow the
     * INTERFACE_HEADER contract: acquired in PrepareHardware and
     * released in ReleaseHardware. IpcValid is set only after a
     * successful query so partially-initialized state is never used.
     */
    VHID_BUS_INTERFACE_V1 BusIpc;
    BOOLEAN             IpcValid;

    /*
     * Pending IOCTL_HID_READ_REPORT IRPs from hidclass.sys. Dispatch
     * mode is Manual; completions happen in report_queue.c when new
     * input reports arrive. Power-managed = FALSE because we want
     * cancellation handling to survive idle transitions.
     */
    WDFQUEUE            InputReportQueue;

    /*
     * Ring buffer backing the queue when the upper stack is not
     * draining. Protected by ReportQueueLock.
     */
    VHID_REPORT_RING    Ring;
    WDFSPINLOCK         ReportQueueLock;

    /*
     * LED state cache. Updated on every keyboard output report sent
     * down by hidclass.sys; protected by LedLock so concurrent
     * WAIT_LED_CHANGE completers observe a consistent value.
     */
    WDFSPINLOCK         LedLock;
    UCHAR               LedBaseline;

    /* Manual queue of waiters for LED change. */
    WDFQUEUE            LedWaitQueue;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VHIDKM_DEVICE_CONTEXT, VhidkmDeviceGetContext);

/*
 * Create the function FDO against the device-init passed by KMDF
 * (EvtDriverDeviceAdd). Registers PnP/power callbacks, allocates the
 * device context, sets idle settings, and kicks off creation of the
 * control device.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VhidkmDeviceCreate(
    _In_ PWDFDEVICE_INIT DeviceInit
    );

/*
 * Post a freshly-built input report to the pending-read queue or, if
 * no read is pending, stash it in the ring buffer. Called by the
 * control-device IOCTL handlers.
 *
 * Runs at PASSIVE_LEVEL because the caller path is always an IOCTL
 * dispatcher.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
VhidkmDevicePostInputReport(
    _In_ PVHIDKM_DEVICE_CONTEXT DevCtx,
    _In_reads_bytes_(ReportLength) const UCHAR* ReportData,
    _In_ UCHAR ReportLength
    );

/*
 * Update the cached LED state and wake every pending
 * IOCTL_VHIDKM_WAIT_LED_CHANGE waiter. Called from the HID output
 * report path.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
VhidkmDeviceUpdateLedState(
    _In_ PVHIDKM_DEVICE_CONTEXT DevCtx,
    _In_ UCHAR NewLedBits
    );

#endif /* VHID_HID_DEVICE_H_ */
