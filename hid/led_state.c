/*
 * hid/led_state.c
 *
 * Implementation of the LED baseline cache and waiter pool. See
 * led_state.h for the design rationale.
 *
 * The waiter path deliberately does NOT use a KMDF timer, DPC, or
 * work item. The entire notification flow runs inline on the caller
 * thread (the HID output-report dispatcher), which is already at
 * PASSIVE_LEVEL but is annotated DISPATCH_LEVEL-safe so the module
 * can be reused from a future fast-path interrupt-style origin.
 *
 * Races considered and handled:
 *
 *   R1. Waiter arrives with a stale baseline.
 *       The WAIT_LED_CHANGE handler takes LedLock, compares, and
 *       either completes immediately or parks under the same lock.
 *       No window exists where an Update could fire between "compare
 *       == equal" and "park" because the queue move happens under
 *       LedLock.
 *
 *   R2. Update fires while multiple waiters are parked.
 *       Update drains the queue one request at a time via
 *       WdfIoQueueRetrieveNextRequest; each retrieval removes the
 *       request from the queue and hands ownership to this thread.
 *       Completions are therefore serialised from the queue's point
 *       of view while still allowing concurrent updates to drain
 *       further waiters on subsequent iterations.
 *
 *   R3. Cancellation of a parked waiter (CancelIoEx / CloseHandle).
 *       KMDF's manual queue honours cancellation natively as long as
 *       WdfRequestForwardToIoQueue is used (which it is). The cancel
 *       path completes the request with STATUS_CANCELLED without our
 *       involvement. We never retain a WDFREQUEST handle after
 *       forwarding, so there is no use-after-cancel risk.
 *
 *   R4. FDO teardown with waiters still parked.
 *       SurpriseRemoval / ReleaseHardware / SelfManagedIoCleanup all
 *       invoke VhidkmLedStateCompleteAllWaiters, which drains with
 *       STATUS_DEVICE_REMOVED. Idempotent.
 */

#include "driver.h"
#include "device.h"
#include "led_state.h"
#include "trace.h"
#include "led_state.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VhidkmLedStateInitialize)
#pragma alloc_text(PAGE, VhidkmLedStateQueueWaiter)
#endif

/*
 * Retrieve every request in the LED wait queue and complete each
 * with (status, led byte, info). The led-byte argument is written
 * into the request's output buffer for success completions; for
 * failure paths (teardown) the buffer is left alone — the caller
 * has no right to inspect output on a non-success status, per the
 * NTSTATUS contract.
 *
 * Must be called with LedLock NOT held, since WdfIoQueueRetrieveNextRequest
 * and WdfRequestComplete should not be invoked while holding a spinlock
 * the completion path could re-acquire. (Currently nothing in the
 * completion path re-enters LedLock, but the convention keeps us safe
 * against future refactors.)
 */
static
VOID
LedDrainAndComplete(
    _In_ PVHIDKM_DEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN                WriteByte,
    _In_ UCHAR                  LedByte,
    _In_ NTSTATUS               CompletionStatus
    )
{
    WDFREQUEST  request;
    NTSTATUS    status;

    for (;;) {
        status = WdfIoQueueRetrieveNextRequest(DevCtx->LedWaitQueue,
                                               &request);
        if (!NT_SUCCESS(status)) {
            /* Empty queue or cancellation race — nothing more to do. */
            break;
        }

        if (WriteByte && NT_SUCCESS(CompletionStatus)) {
            PUCHAR  out;
            size_t  outLen = 0;
            NTSTATUS outSt = WdfRequestRetrieveOutputBuffer(request,
                                                            sizeof(UCHAR),
                                                            (PVOID*)&out,
                                                            &outLen);
            if (NT_SUCCESS(outSt)) {
                *out = LedByte;
                WdfRequestCompleteWithInformation(request,
                                                  CompletionStatus,
                                                  sizeof(UCHAR));
                continue;
            }
            /*
             * A parked request whose output buffer cannot be retrieved
             * was already malformed when we took it. Complete with the
             * retrieval status so the caller sees a coherent error.
             */
            WdfRequestComplete(request, outSt);
            continue;
        }

        WdfRequestComplete(request, CompletionStatus);
    }
}

_Use_decl_annotations_
NTSTATUS
VhidkmLedStateInitialize(
    struct _VHIDKM_DEVICE_CONTEXT* DevCtx
    )
{
    WDF_IO_QUEUE_CONFIG     qcfg;
    WDF_OBJECT_ATTRIBUTES   qattr;
    NTSTATUS                status;

    PAGED_CODE();

    /*
     * Manual dispatch: requests live in the queue until we pull them
     * out explicitly. No EvtIo* callbacks are wired up because the
     * LED wait protocol never dispatches — it parks and later
     * drains on an external trigger.
     *
     * PowerManaged = WdfFalse: the idle/D-state machine must not
     * stall completions of WAIT_LED_CHANGE simply because the HID
     * device is selectively suspended. Keyboard LED updates originate
     * from hidclass.sys independent of the FDO's power state.
     */
    WDF_IO_QUEUE_CONFIG_INIT(&qcfg, WdfIoQueueDispatchManual);
    qcfg.PowerManaged = WdfFalse;

    WDF_OBJECT_ATTRIBUTES_INIT(&qattr);
    qattr.ParentObject = DevCtx->Device;

    status = WdfIoQueueCreate(DevCtx->Device, &qcfg, &qattr,
                              &DevCtx->LedWaitQueue);
    if (!NT_SUCCESS(status)) {
        TraceError("LedWaitQueue create failed %!STATUS!", status);
        return status;
    }

    DevCtx->LedBaseline = 0;
    TraceLed("LED state module initialized");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
UCHAR
VhidkmLedStateRead(
    struct _VHIDKM_DEVICE_CONTEXT* DevCtx
    )
{
    UCHAR value;

    /*
     * A single-byte read is trivially atomic on x86/x64/ARM64, but the
     * lock also establishes a memory fence against a concurrent
     * Update on a different CPU so the observed byte is never a torn
     * intermediate. The cost is a single cache-line round-trip.
     */
    WdfSpinLockAcquire(DevCtx->LedLock);
    value = DevCtx->LedBaseline;
    WdfSpinLockRelease(DevCtx->LedLock);
    return value;
}

_Use_decl_annotations_
VOID
VhidkmLedStateUpdate(
    struct _VHIDKM_DEVICE_CONTEXT* DevCtx,
    UCHAR                           NewLedBits
    )
{
    UCHAR   previous;
    BOOLEAN changed;

    WdfSpinLockAcquire(DevCtx->LedLock);
    previous = DevCtx->LedBaseline;
    changed  = (previous != NewLedBits);
    if (changed) {
        DevCtx->LedBaseline = NewLedBits;
    }
    WdfSpinLockRelease(DevCtx->LedLock);

    if (!changed) {
        return;
    }

    TraceLed("LED change 0x%02x -> 0x%02x", (ULONG)previous, (ULONG)NewLedBits);

    /*
     * A state change has been committed to the baseline. Every parked
     * waiter gets the NEW byte (not the old one) because a waiter
     * arriving one instant after this update would also see the new
     * byte via Read; returning the same value to both populations
     * preserves sequential consistency.
     */
    LedDrainAndComplete(DevCtx,
                        /*WriteByte*/  TRUE,
                        /*LedByte*/    NewLedBits,
                        STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS
VhidkmLedStateQueueWaiter(
    struct _VHIDKM_DEVICE_CONTEXT* DevCtx,
    WDFREQUEST                     Request,
    size_t                         InputBufferLength,
    size_t                         OutputBufferLength
    )
{
    PUCHAR      inBaseline;
    PUCHAR      outBuffer;
    size_t      inLen = 0;
    size_t      outLen = 0;
    UCHAR       baseline;
    UCHAR       current;
    NTSTATUS    status;

    PAGED_CODE();

    /*
     * Both buffers must exist and be at least UCHAR-sized. The IOCTL
     * is METHOD_BUFFERED so KMDF has already validated user-mode
     * addresses and copied the input; short buffers surface here as
     * STATUS_BUFFER_TOO_SMALL from the retrieve helpers.
     */
    if (InputBufferLength < sizeof(UCHAR) ||
        OutputBufferLength < sizeof(UCHAR)) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(UCHAR),
                                           (PVOID*)&inBaseline, &inLen);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    baseline = *inBaseline;

    /* Output retrieval validated now so the park path is infallible. */
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(UCHAR),
                                            (PVOID*)&outBuffer, &outLen);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * Compare + park under the same lock. If compare-vs-baseline says
     * "already different" we complete inline with the current byte;
     * otherwise we forward into the manual queue while still holding
     * LedLock so no concurrent Update can wedge its drain between our
     * observation and our park.
     *
     * WdfRequestForwardToIoQueue is safe to call at DISPATCH_LEVEL on
     * a queue with PowerManaged = FALSE and the target queue is owned
     * by the same device, per KMDF documentation.
     */
    WdfSpinLockAcquire(DevCtx->LedLock);
    current = DevCtx->LedBaseline;

    if (current != baseline) {
        WdfSpinLockRelease(DevCtx->LedLock);
        *outBuffer = current;
        TraceLed("WAIT_LED_CHANGE immediate: baseline=0x%02x current=0x%02x",
                 (ULONG)baseline, (ULONG)current);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                          sizeof(UCHAR));
        /*
         * Returning STATUS_SUCCESS here would cause the dispatcher to
         * double-complete the request. Return STATUS_PENDING so the
         * caller treats it as already-handled.
         */
        return STATUS_PENDING;
    }

    status = WdfRequestForwardToIoQueue(Request, DevCtx->LedWaitQueue);
    WdfSpinLockRelease(DevCtx->LedLock);

    if (!NT_SUCCESS(status)) {
        TraceError("WdfRequestForwardToIoQueue(LedWaitQueue) %!STATUS!", status);
        return status;
    }

    TraceLed("WAIT_LED_CHANGE parked (baseline=0x%02x)", (ULONG)baseline);
    return STATUS_PENDING;
}

_Use_decl_annotations_
VOID
VhidkmLedStateCompleteAllWaiters(
    struct _VHIDKM_DEVICE_CONTEXT* DevCtx,
    NTSTATUS                       Status
    )
{
    /*
     * Teardown path: no byte to write because the caller will see a
     * failure status and is contractually required not to inspect the
     * output buffer.
     */
    if (DevCtx->LedWaitQueue == NULL) {
        return;
    }
    TraceLed("draining LED waiters with %!STATUS!", Status);
    LedDrainAndComplete(DevCtx,
                        /*WriteByte*/ FALSE,
                        /*LedByte*/   0,
                        Status);
}
