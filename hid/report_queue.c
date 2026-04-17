/*
 * hid/report_queue.c
 *
 * Implementation of the manual pending-read queue + ring buffer for
 * input reports.
 *
 * Design:
 *
 *   - Manual KMDF queue (WdfIoQueueDispatchManual) holds one pending
 *     IOCTL_HID_READ_REPORT at a time. hidclass.sys never issues
 *     more than one in parallel, but the queue is sized to accept
 *     arbitrary depth just in case — it is bounded by the class
 *     driver's behaviour, not by our declaration.
 *
 *   - Ring buffer is a fixed-size power-of-two array of reports with
 *     per-entry length. Head / Tail / Count are plain ULONGs
 *     protected by DevCtx->ReportQueueLock.
 *
 *   - Completion of a pended IRP uses
 *     WdfRequestCompleteWithInformation so hidclass.sys reads the
 *     exact number of bytes written to the output buffer. The IRP's
 *     output buffer is retrieved via WdfRequestRetrieveOutputBuffer;
 *     on IOCTL_HID_READ_REPORT, hidclass passes us its SystemBuffer
 *     large enough for the longest report declared in the report
 *     descriptor — VHID_MAX_REPORT_SIZE bytes.
 *
 *   - Cancellation: WdfIoQueueConfigureRequestDispatching lets us
 *     forward the IRP without registering a cancel routine, because
 *     the manual queue's built-in cancellation-via-queue-dispose is
 *     sufficient: calling WdfIoQueuePurge or WdfIoQueueDrain
 *     completes every outstanding IRP with STATUS_CANCELLED.
 *
 * Locking:
 *   All ring / queue manipulation acquires DevCtx->ReportQueueLock
 *   (a spinlock). The only operations run inside the lock are short,
 *   bounded memcopies and pointer arithmetic; no framework calls
 *   that would raise IRQL are nested (WdfIoQueueRetrieveNextRequest
 *   tolerates DISPATCH_LEVEL so it is safe to call under the lock,
 *   but we deliberately drop the lock before the request-complete
 *   call to keep the critical section tight).
 */

#include "driver.h"
#include "device.h"
#include "report_queue.h"
#include "trace.h"
#include "report_queue.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VhidkmReportQueueInitialize)
#endif

static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
RingReset(
    _Inout_ PVHID_REPORT_RING Ring
    );

static
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
RingPush(
    _Inout_ PVHID_REPORT_RING Ring,
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ UCHAR Length
    );

static
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
RingPop(
    _Inout_ PVHID_REPORT_RING Ring,
    _Out_writes_bytes_to_(MaxLength, *OutLength) PUCHAR OutData,
    _In_ UCHAR MaxLength,
    _Out_ PUCHAR OutLength
    );

static
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
RingFindLast(
    _In_ const VHID_REPORT_RING* Ring,
    _In_ UCHAR ReportId,
    _Out_writes_bytes_to_(MaxLength, *OutLength) PUCHAR OutData,
    _In_ UCHAR MaxLength,
    _Out_ PUCHAR OutLength
    );

_Use_decl_annotations_
NTSTATUS
VhidkmReportQueueInitialize(
    struct _VHIDKM_DEVICE_CONTEXT* DevCtx
    )
{
    WDF_IO_QUEUE_CONFIG     cfg;
    WDF_OBJECT_ATTRIBUTES   attr;
    NTSTATUS                status;

    PAGED_CODE();

    /*
     * Manual dispatch: the queue only holds requests until we
     * dequeue them explicitly via WdfIoQueueRetrieveNextRequest.
     * Power-managed = TRUE so KMDF pauses the queue during D3 and
     * cancels the parked IRP on Dx exit into final-remove; the
     * framework guarantees that we never retrieve a request on a
     * device about to be removed, and our Drain code covers the
     * last-chance case.
     */
    WDF_IO_QUEUE_CONFIG_INIT(&cfg, WdfIoQueueDispatchManual);
    cfg.PowerManaged = WdfTrue;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = DevCtx->Device;

    status = WdfIoQueueCreate(DevCtx->Device, &cfg, &attr,
                              &DevCtx->InputReportQueue);
    if (!NT_SUCCESS(status)) {
        TraceError("InputReportQueue create failed %!STATUS!", status);
        return status;
    }

    RingReset(&DevCtx->Ring);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VhidkmReportQueueForwardRead(
    struct _VHIDKM_DEVICE_CONTEXT* DevCtx,
    WDFREQUEST                     Request
    )
{
    UCHAR       buf[VHID_MAX_REPORT_SIZE];
    UCHAR       len = 0;
    BOOLEAN     haveReport;
    NTSTATUS    status;

    WdfSpinLockAcquire(DevCtx->ReportQueueLock);
    haveReport = RingPop(&DevCtx->Ring, buf, (UCHAR)sizeof(buf), &len);
    WdfSpinLockRelease(DevCtx->ReportQueueLock);

    if (haveReport) {
        PVOID   out    = NULL;
        size_t  outLen = 0;
        status = WdfRequestRetrieveOutputBuffer(Request, len, &out, &outLen);
        if (!NT_SUCCESS(status)) {
            TraceQue("forward-read buffer too small: %!STATUS!", status);
            return status;
        }
        if (outLen < len) {
            /*
             * hidclass.sys always hands us a buffer sized to the
             * maximum declared report. Defensive check in case a
             * diagnostic caller posts a shorter one.
             */
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlCopyMemory(out, buf, len);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, len);
        return STATUS_SUCCESS;
    }

    /* No backlog; park the IRP. */
    status = WdfRequestForwardToIoQueue(Request, DevCtx->InputReportQueue);
    if (!NT_SUCCESS(status)) {
        TraceError("ForwardToIoQueue failed %!STATUS!", status);
        return status;
    }
    return STATUS_PENDING;
}

_Use_decl_annotations_
NTSTATUS
VhidkmReportQueuePost(
    struct _VHIDKM_DEVICE_CONTEXT* DevCtx,
    const UCHAR*                   ReportData,
    UCHAR                          ReportLength
    )
{
    WDFREQUEST  req = NULL;
    NTSTATUS    status;

    if (ReportLength == 0 || ReportLength > VHID_MAX_REPORT_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfSpinLockAcquire(DevCtx->ReportQueueLock);

    status = WdfIoQueueRetrieveNextRequest(DevCtx->InputReportQueue, &req);
    if (!NT_SUCCESS(status) || req == NULL) {
        /* No waiter — stash in the ring. */
        (VOID)RingPush(&DevCtx->Ring, ReportData, ReportLength);
        WdfSpinLockRelease(DevCtx->ReportQueueLock);
        return STATUS_SUCCESS;
    }

    WdfSpinLockRelease(DevCtx->ReportQueueLock);

    {
        PVOID   out    = NULL;
        size_t  outLen = 0;
        status = WdfRequestRetrieveOutputBuffer(req, ReportLength,
                                                &out, &outLen);
        if (!NT_SUCCESS(status) || outLen < ReportLength) {
            TraceQue("retrieved request has bad output buffer %!STATUS!", status);
            WdfRequestComplete(req, NT_SUCCESS(status)
                               ? STATUS_BUFFER_TOO_SMALL : status);
            return status;
        }
        RtlCopyMemory(out, ReportData, ReportLength);
        WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, ReportLength);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VhidkmReportQueuePeek(
    struct _VHIDKM_DEVICE_CONTEXT* DevCtx,
    UCHAR                          ReportId,
    PUCHAR                         Buffer,
    ULONG                          BufferLength,
    PULONG                         Written
    )
{
    UCHAR       tmp[VHID_MAX_REPORT_SIZE];
    UCHAR       len = 0;
    BOOLEAN     found;

    *Written = 0;

    if (BufferLength > 0xFF) {
        BufferLength = 0xFF;
    }

    WdfSpinLockAcquire(DevCtx->ReportQueueLock);
    found = RingFindLast(&DevCtx->Ring, ReportId, tmp,
                         (UCHAR)sizeof(tmp), &len);
    WdfSpinLockRelease(DevCtx->ReportQueueLock);

    if (!found) {
        return STATUS_NO_MORE_ENTRIES;
    }

    if (BufferLength < len) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(Buffer, tmp, len);
    *Written = len;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
VhidkmReportQueueDrain(
    struct _VHIDKM_DEVICE_CONTEXT* DevCtx
    )
{
    WDFREQUEST  req;
    NTSTATUS    s;

    if (DevCtx->InputReportQueue == NULL) {
        return;
    }

    /*
     * WdfIoQueuePurge would complete every parked request with
     * STATUS_CANCELLED and then stop accepting new ones. We do not
     * want to stop accepting — the queue may resume after a
     * surprise-remove that is later rolled back during restart —
     * so iterate and complete one by one.
     */
    for (;;) {
        WdfSpinLockAcquire(DevCtx->ReportQueueLock);
        s = WdfIoQueueRetrieveNextRequest(DevCtx->InputReportQueue, &req);
        WdfSpinLockRelease(DevCtx->ReportQueueLock);
        if (!NT_SUCCESS(s) || req == NULL) {
            break;
        }
        WdfRequestComplete(req, STATUS_DEVICE_REMOVED);
    }

    /* Also clear any backlog so a re-plug starts with an empty ring. */
    WdfSpinLockAcquire(DevCtx->ReportQueueLock);
    RingReset(&DevCtx->Ring);
    WdfSpinLockRelease(DevCtx->ReportQueueLock);
}

/* ------------------------------------------------------------------
 * Ring buffer primitives.
 * ------------------------------------------------------------------ */

#define RING_MASK  ((ULONG)(VHID_REPORT_RING_CAPACITY - 1))

_Use_decl_annotations_
static
VOID
RingReset(
    PVHID_REPORT_RING Ring
    )
{
    Ring->Head  = 0;
    Ring->Tail  = 0;
    Ring->Count = 0;
    RtlZeroMemory(Ring->Lengths, sizeof(Ring->Lengths));
}

_Use_decl_annotations_
static
BOOLEAN
RingPush(
    PVHID_REPORT_RING   Ring,
    const UCHAR*        Data,
    UCHAR               Length
    )
{
    if (Length == 0 || Length > VHID_MAX_REPORT_SIZE) {
        return FALSE;
    }

    if (Ring->Count == VHID_REPORT_RING_CAPACITY) {
        /*
         * Ring is full — drop the oldest entry to make room. This
         * mirrors the lossy behaviour of a real interrupt-IN endpoint
         * when the host cannot keep up: the device overwrites the
         * stale report rather than blocking.
         */
        Ring->Head = (Ring->Head + 1) & RING_MASK;
        Ring->Count--;
    }

    RtlCopyMemory(Ring->Entries[Ring->Tail], Data, Length);
    Ring->Lengths[Ring->Tail] = Length;
    Ring->Tail  = (Ring->Tail + 1) & RING_MASK;
    Ring->Count++;
    return TRUE;
}

_Use_decl_annotations_
static
BOOLEAN
RingPop(
    PVHID_REPORT_RING   Ring,
    PUCHAR              OutData,
    UCHAR               MaxLength,
    PUCHAR              OutLength
    )
{
    UCHAR len;

    if (Ring->Count == 0) {
        *OutLength = 0;
        return FALSE;
    }

    len = Ring->Lengths[Ring->Head];
    if (len > MaxLength) {
        /* Caller gave a buffer smaller than the stored report. */
        *OutLength = 0;
        return FALSE;
    }

    RtlCopyMemory(OutData, Ring->Entries[Ring->Head], len);
    *OutLength = len;
    Ring->Head = (Ring->Head + 1) & RING_MASK;
    Ring->Count--;
    return TRUE;
}

_Use_decl_annotations_
static
BOOLEAN
RingFindLast(
    const VHID_REPORT_RING* Ring,
    UCHAR                   ReportId,
    PUCHAR                  OutData,
    UCHAR                   MaxLength,
    PUCHAR                  OutLength
    )
{
    ULONG   i;
    ULONG   idx;

    *OutLength = 0;

    /*
     * Walk backward from the most recent entry looking for a match.
     * The loop is bounded by VHID_REPORT_RING_CAPACITY and Count so
     * it terminates quickly.
     */
    for (i = 0; i < Ring->Count; i++) {
        idx = (Ring->Tail + (VHID_REPORT_RING_CAPACITY - 1) - i) & RING_MASK;
        if (Ring->Lengths[idx] >= 1 &&
            Ring->Entries[idx][0] == ReportId) {

            UCHAR len = Ring->Lengths[idx];
            if (len > MaxLength) {
                return FALSE;
            }
            RtlCopyMemory(OutData, Ring->Entries[idx], len);
            *OutLength = len;
            return TRUE;
        }
    }
    return FALSE;
}
