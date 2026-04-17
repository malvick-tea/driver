/*
 * hid/led_state.h
 *
 * Keyboard-LED baseline cache and wait-for-change waiter pool.
 *
 * Problem shape:
 *   hidclass.sys drives LED indicators (Num / Caps / Scroll / Compose /
 *   Kana) by sending a one-byte output report with ReportId
 *   VHID_REPORTID_KEYBOARD_INPUT to the HID minidriver. User mode
 *   wants two access paths to that byte:
 *
 *     - IOCTL_VHIDKM_GET_LED_STATE   — non-blocking snapshot read.
 *     - IOCTL_VHIDKM_WAIT_LED_CHANGE — park until the byte differs
 *                                      from a caller-supplied baseline.
 *
 *   The waiter path must support arbitrary concurrent callers
 *   (Caps-sync daemons, AHK-style tooling, instrumentation), survive
 *   cancellation (CancelIoEx, CloseHandle, process termination), and
 *   deliver the *current* byte at wake time — not the delta — so the
 *   caller never has to race a follow-up Get.
 *
 * Design:
 *   - One UCHAR baseline per FDO, guarded by DevCtx->LedLock (a
 *     WDFSPINLOCK, so updates from either the HID output-report path
 *     or a future DISPATCH_LEVEL timer tick are safe).
 *   - One manual KMDF queue per FDO (DevCtx->LedWaitQueue) holding
 *     parked WAIT_LED_CHANGE requests. Manual dispatch + power-managed
 *     = FALSE keeps the queue insensitive to selective-suspend
 *     idle/resume transitions.
 *   - Update compares old vs. new: if unchanged the queue is left
 *     alone; if changed the full waiter population is drained and
 *     each completion writes the new byte into the request's output
 *     buffer.
 *   - KMDF's built-in cancel logic on manual queues handles the
 *     cancellable-irp contract. No custom EvtRequestCancel routine is
 *     required because we never pull a request out of the queue
 *     without immediately completing it.
 *
 * Locking:
 *   Hot path acquires LedLock briefly (compare + conditional drain).
 *   The drain walks WdfIoQueueRetrieveNextRequest outside the lock —
 *   once a request is out of the queue it is owned by this thread and
 *   no further synchronization is needed for completion.
 */

#pragma once
#ifndef VHID_HID_LED_STATE_H_
#define VHID_HID_LED_STATE_H_

#include "driver.h"

struct _VHIDKM_DEVICE_CONTEXT;

/*
 * Create the LED wait queue and zero the baseline. Must run exactly
 * once per FDO, before any IOCTL path touches the module.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VhidkmLedStateInitialize(
    _Inout_ struct _VHIDKM_DEVICE_CONTEXT* DevCtx
    );

/*
 * Atomic snapshot read of the current LED baseline. Safe to invoke at
 * any IRQL <= DISPATCH_LEVEL.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
UCHAR
VhidkmLedStateRead(
    _In_ struct _VHIDKM_DEVICE_CONTEXT* DevCtx
    );

/*
 * Update the baseline in response to a HID output report. If the
 * new byte differs from the cached one, drain every pending waiter
 * and complete each with the new value. No-op when the byte matches
 * — idempotent SET_REPORTs do not wake callers spuriously.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
VhidkmLedStateUpdate(
    _Inout_ struct _VHIDKM_DEVICE_CONTEXT* DevCtx,
    _In_    UCHAR                          NewLedBits
    );

/*
 * Handler for IOCTL_VHIDKM_WAIT_LED_CHANGE. Validates the input
 * baseline, compares it against the cached byte, and either:
 *   - completes immediately with the current byte when they differ
 *     (return value is NT_SUCCESS; caller completes the request with
 *     Info = sizeof(UCHAR)), or
 *   - parks the request on the wait queue and returns STATUS_PENDING.
 *     The caller MUST NOT touch the request after a pending return.
 *
 * On error paths the request is untouched and the caller completes it
 * with the returned status.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VhidkmLedStateQueueWaiter(
    _In_ struct _VHIDKM_DEVICE_CONTEXT* DevCtx,
    _In_ WDFREQUEST                     Request,
    _In_ size_t                         InputBufferLength,
    _In_ size_t                         OutputBufferLength
    );

/*
 * Drain every parked waiter and complete each with the supplied
 * status (typically STATUS_DEVICE_REMOVED or STATUS_CANCELLED).
 * Invoked on surprise-remove, release-hardware, and driver unload.
 * Safe to call repeatedly; the queue tolerates empty drains.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
VhidkmLedStateCompleteAllWaiters(
    _Inout_ struct _VHIDKM_DEVICE_CONTEXT* DevCtx,
    _In_    NTSTATUS                       Status
    );

#endif /* VHID_HID_LED_STATE_H_ */
