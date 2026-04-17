/*
 * hid/ctl_ioctls.c
 *
 * EvtIoDeviceControl for the vhidkm.sys control device.
 *
 * Validation rules applied uniformly at the top of every handler:
 *   1. InputBufferLength / OutputBufferLength vs sizeof(struct).
 *   2. For request structs with a leading Size field: Size must equal
 *      sizeof(struct) exactly. This protects against a caller
 *      smuggling a v2 request into a v1 driver.
 *   3. Values outside documented ranges (e.g., HoldMs) are clamped
 *      at the boundary before use. Clamping rather than rejecting
 *      means a misbehaving caller cannot spam STATUS_INVALID_PARAMETER
 *      errors to probe the protocol.
 *   4. Report ID bytes are validated against the expected constant
 *      per IOCTL so a keyboard report cannot be delivered via the
 *      mouse path and vice versa.
 *
 * Every handler completes its request with
 * WdfRequestCompleteWithInformation so user-mode receives the correct
 * BytesReturned value through GetOverlappedResult / DeviceIoControl.
 * Pended IRPs — IOCTL_VHIDKM_WAIT_LED_CHANGE — use the dedicated
 * forward path in led_state.c and return STATUS_PENDING to the
 * dispatcher which then leaves the request alone.
 */

#include "driver.h"
#include "device.h"
#include "ctl_device.h"
#include "ctl_ioctls.h"
#include "led_state.h"
#include "report_queue.h"
#include "trace.h"
#include "ctl_ioctls.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VhidkmCtlIoctlInitialize)
#endif

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL VhidkmCtlEvtIoDeviceControl;

static NTSTATUS CtlGetVersion(_In_ WDFREQUEST Req, _Out_ PULONG_PTR Info);
static NTSTATUS CtlKeyboardReport(_In_ PVHIDKM_DEVICE_CONTEXT Dev,
                                  _In_ WDFREQUEST Req, _In_ size_t InLen);
static NTSTATUS CtlKeyboardKeys(_In_ PVHIDKM_DEVICE_CONTEXT Dev,
                                _In_ WDFREQUEST Req, _In_ size_t InLen);
static NTSTATUS CtlKeyStroke(_In_ PVHIDKM_DEVICE_CONTEXT Dev,
                             _In_ WDFREQUEST Req, _In_ size_t InLen);
static NTSTATUS CtlMouseRel(_In_ PVHIDKM_DEVICE_CONTEXT Dev,
                            _In_ WDFREQUEST Req, _In_ size_t InLen);
static NTSTATUS CtlMouseAbs(_In_ PVHIDKM_DEVICE_CONTEXT Dev,
                            _In_ WDFREQUEST Req, _In_ size_t InLen);
static NTSTATUS CtlMouseAbsPx(_In_ PVHIDKM_DEVICE_CONTEXT Dev,
                              _In_ WDFFILEOBJECT FileObject,
                              _In_ WDFREQUEST Req, _In_ size_t InLen);
static NTSTATUS CtlSetScreenMetrics(_In_ WDFFILEOBJECT FileObject,
                                    _In_ WDFREQUEST Req, _In_ size_t InLen);
static NTSTATUS CtlGetLedState(_In_ PVHIDKM_DEVICE_CONTEXT Dev,
                               _In_ WDFREQUEST Req, _In_ size_t OutLen,
                               _Out_ PULONG_PTR Info);
static NTSTATUS CtlReset(_In_ PVHIDKM_DEVICE_CONTEXT Dev);

_Use_decl_annotations_
NTSTATUS
VhidkmCtlIoctlInitialize(
    WDFDEVICE ControlDevice
    )
{
    WDF_IO_QUEUE_CONFIG cfg;
    NTSTATUS            status;

    PAGED_CODE();

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&cfg, WdfIoQueueDispatchParallel);
    cfg.EvtIoDeviceControl = VhidkmCtlEvtIoDeviceControl;
    cfg.PowerManaged       = WdfFalse; /* control device has no PnP state */

    status = WdfIoQueueCreate(ControlDevice, &cfg,
                              WDF_NO_OBJECT_ATTRIBUTES, NULL);
    return status;
}

_Use_decl_annotations_
VOID
VhidkmCtlEvtIoDeviceControl(
    WDFQUEUE    Queue,
    WDFREQUEST  Request,
    size_t      OutputBufferLength,
    size_t      InputBufferLength,
    ULONG       IoControlCode
    )
{
    WDFDEVICE               ctlDevice = WdfIoQueueGetDevice(Queue);
    PVHIDKM_CTL_DEV_CONTEXT devCtxOnCtl = VhidkmCtlDevGetContext(ctlDevice);
    PVHIDKM_DEVICE_CONTEXT  dev = devCtxOnCtl ? devCtxOnCtl->DevCtx : NULL;
    WDFFILEOBJECT           fileObj = WdfRequestGetFileObject(Request);
    NTSTATUS                status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR               info = 0;

    if (dev == NULL) {
        /*
         * Control device exists but no FDO is currently backing the
         * virtual USB instance. Injection is impossible; fail with
         * a status user-mode can act on (re-plug).
         */
        WdfRequestCompleteWithInformation(Request, STATUS_DEVICE_NOT_READY, 0);
        return;
    }

    switch (IoControlCode) {
    case IOCTL_VHIDKM_GET_VERSION:
        status = CtlGetVersion(Request, &info);
        break;

    case IOCTL_VHIDKM_KEYBOARD_REPORT:
        status = CtlKeyboardReport(dev, Request, InputBufferLength);
        break;

    case IOCTL_VHIDKM_KEYBOARD_KEYS:
        status = CtlKeyboardKeys(dev, Request, InputBufferLength);
        break;

    case IOCTL_VHIDKM_KEY_STROKE:
        status = CtlKeyStroke(dev, Request, InputBufferLength);
        break;

    case IOCTL_VHIDKM_MOUSE_REL:
        status = CtlMouseRel(dev, Request, InputBufferLength);
        break;

    case IOCTL_VHIDKM_MOUSE_ABS:
        status = CtlMouseAbs(dev, Request, InputBufferLength);
        break;

    case IOCTL_VHIDKM_MOUSE_ABS_PX:
        status = CtlMouseAbsPx(dev, fileObj, Request, InputBufferLength);
        break;

    case IOCTL_VHIDKM_SET_SCREEN_METRICS:
        status = CtlSetScreenMetrics(fileObj, Request, InputBufferLength);
        break;

    case IOCTL_VHIDKM_GET_LED_STATE:
        status = CtlGetLedState(dev, Request, OutputBufferLength, &info);
        break;

    case IOCTL_VHIDKM_WAIT_LED_CHANGE:
        /*
         * Pended inside LED state handler; it either completes the
         * IRP immediately (if state already differs from baseline)
         * or parks it in the waiter queue and returns PENDING.
         */
        status = VhidkmLedStateQueueWaiter(dev, Request,
                                           InputBufferLength,
                                           OutputBufferLength);
        if (status == STATUS_PENDING) {
            return;
        }
        break;

    case IOCTL_VHIDKM_RESET:
        status = CtlReset(dev);
        break;

    default:
        TraceCtl("unknown IOCTL 0x%08x", IoControlCode);
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
}

/* ------------------------------------------------------------------
 * Individual handlers
 * ------------------------------------------------------------------ */

static
NTSTATUS
CtlGetVersion(
    _In_  WDFREQUEST   Req,
    _Out_ PULONG_PTR    Info
    )
{
    PVHID_VERSION   out;
    size_t          outLen = 0;
    NTSTATUS        status;

    *Info = 0;

    status = WdfRequestRetrieveOutputBuffer(Req, sizeof(VHID_VERSION),
                                            (PVOID*)&out, &outLen);
    if (!NT_SUCCESS(status)) return status;

    out->Major    = VHID_VERSION_MAJOR;
    out->Minor    = VHID_VERSION_MINOR;
    out->Build    = VHID_VERSION_BUILD;
    out->ApiLevel = VHID_API_LEVEL;
    *Info = sizeof(VHID_VERSION);
    return STATUS_SUCCESS;
}

static
NTSTATUS
CtlKeyboardReport(
    _In_ PVHIDKM_DEVICE_CONTEXT Dev,
    _In_ WDFREQUEST             Req,
    _In_ size_t                 InLen
    )
{
    PVHID_KEYBOARD_INPUT_REPORT in;
    NTSTATUS                    status;

    if (InLen < sizeof(*in)) return STATUS_INVALID_BUFFER_SIZE;

    status = WdfRequestRetrieveInputBuffer(Req, sizeof(*in), (PVOID*)&in, NULL);
    if (!NT_SUCCESS(status)) return status;

    if (in->ReportId != VHID_REPORTID_KEYBOARD_INPUT) {
        return STATUS_INVALID_PARAMETER;
    }

    return VhidkmDevicePostInputReport(Dev,
        (const UCHAR*)in, (UCHAR)sizeof(*in));
}

static
NTSTATUS
CtlKeyboardKeys(
    _In_ PVHIDKM_DEVICE_CONTEXT Dev,
    _In_ WDFREQUEST             Req,
    _In_ size_t                 InLen
    )
{
    PVHID_KBD_KEYS_REQ          in;
    VHID_KEYBOARD_INPUT_REPORT  report;
    NTSTATUS                    status;

    if (InLen < sizeof(*in)) return STATUS_INVALID_BUFFER_SIZE;

    status = WdfRequestRetrieveInputBuffer(Req, sizeof(*in), (PVOID*)&in, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (in->Size != sizeof(*in)) return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(&report, sizeof(report));
    report.ReportId  = VHID_REPORTID_KEYBOARD_INPUT;
    report.Modifiers = in->Modifiers;
    report.Reserved  = 0;
    RtlCopyMemory(report.Keys, in->Keys, sizeof(report.Keys));

    return VhidkmDevicePostInputReport(Dev,
        (const UCHAR*)&report, (UCHAR)sizeof(report));
}

static
NTSTATUS
CtlKeyStroke(
    _In_ PVHIDKM_DEVICE_CONTEXT Dev,
    _In_ WDFREQUEST             Req,
    _In_ size_t                 InLen
    )
{
    PVHID_KEYSTROKE_REQ         in;
    VHID_KEYBOARD_INPUT_REPORT  press;
    VHID_KEYBOARD_INPUT_REPORT  release;
    NTSTATUS                    status;
    ULONG                       holdMs;

    if (InLen < sizeof(*in)) return STATUS_INVALID_BUFFER_SIZE;
    status = WdfRequestRetrieveInputBuffer(Req, sizeof(*in), (PVOID*)&in, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (in->Size != sizeof(*in)) return STATUS_INVALID_PARAMETER;

    /*
     * Clamp rather than reject out-of-range HoldMs. A caller cannot
     * meaningfully distinguish "I asked for 60s and got 5s" from
     * "the IOCTL failed"; silently clamping is the friendlier
     * contract and the documented behaviour.
     */
    holdMs = in->HoldMs;
    if (holdMs > VHID_KEYSTROKE_HOLD_MAX_MS) {
        holdMs = VHID_KEYSTROKE_HOLD_MAX_MS;
    }

    RtlZeroMemory(&press, sizeof(press));
    press.ReportId  = VHID_REPORTID_KEYBOARD_INPUT;
    press.Modifiers = in->Modifiers;
    press.Keys[0]   = in->Usage;

    RtlZeroMemory(&release, sizeof(release));
    release.ReportId = VHID_REPORTID_KEYBOARD_INPUT;

    status = VhidkmDevicePostInputReport(Dev,
        (const UCHAR*)&press, (UCHAR)sizeof(press));
    if (!NT_SUCCESS(status)) return status;

    /*
     * Blocking KeDelayExecutionThread at PASSIVE_LEVEL is acceptable
     * here because the caller's thread is what drives the IOCTL; by
     * documentation the call returns synchronously after the release
     * report has been enqueued. HoldMs <= 5000 makes the worst-case
     * block bounded.
     */
    if (holdMs > 0) {
        LARGE_INTEGER delay;
        delay.QuadPart = -((LONGLONG)holdMs) * 10 * 1000;
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

    return VhidkmDevicePostInputReport(Dev,
        (const UCHAR*)&release, (UCHAR)sizeof(release));
}

static
NTSTATUS
CtlMouseRel(
    _In_ PVHIDKM_DEVICE_CONTEXT Dev,
    _In_ WDFREQUEST             Req,
    _In_ size_t                 InLen
    )
{
    PVHID_MOUSE_REL_REPORT in;
    NTSTATUS               status;

    if (InLen < sizeof(*in)) return STATUS_INVALID_BUFFER_SIZE;
    status = WdfRequestRetrieveInputBuffer(Req, sizeof(*in), (PVOID*)&in, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (in->ReportId != VHID_REPORTID_MOUSE_REL) return STATUS_INVALID_PARAMETER;

    return VhidkmDevicePostInputReport(Dev,
        (const UCHAR*)in, (UCHAR)sizeof(*in));
}

static
NTSTATUS
CtlMouseAbs(
    _In_ PVHIDKM_DEVICE_CONTEXT Dev,
    _In_ WDFREQUEST             Req,
    _In_ size_t                 InLen
    )
{
    PVHID_MOUSE_ABS_REPORT in;
    NTSTATUS               status;

    if (InLen < sizeof(*in)) return STATUS_INVALID_BUFFER_SIZE;
    status = WdfRequestRetrieveInputBuffer(Req, sizeof(*in), (PVOID*)&in, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (in->ReportId != VHID_REPORTID_MOUSE_ABS) return STATUS_INVALID_PARAMETER;

    /* Descriptor caps absolute X/Y to VHID_MOUSE_ABS_MAX; saturate. */
    if (in->X > VHID_MOUSE_ABS_MAX) in->X = VHID_MOUSE_ABS_MAX;
    if (in->Y > VHID_MOUSE_ABS_MAX) in->Y = VHID_MOUSE_ABS_MAX;

    return VhidkmDevicePostInputReport(Dev,
        (const UCHAR*)in, (UCHAR)sizeof(*in));
}

static
NTSTATUS
CtlMouseAbsPx(
    _In_ PVHIDKM_DEVICE_CONTEXT Dev,
    _In_ WDFFILEOBJECT          FileObject,
    _In_ WDFREQUEST             Req,
    _In_ size_t                 InLen
    )
{
    PVHID_MOUSE_ABS_PX_REQ     in;
    VHID_MOUSE_ABS_REPORT      report;
    PVHIDKM_CTL_FILE_CONTEXT   fileCtx = VhidkmCtlFileGetContext(FileObject);
    NTSTATUS                   status;
    INT64                      denomX;
    INT64                      denomY;
    INT64                      numerX;
    INT64                      numerY;
    INT64                      logicalX;
    INT64                      logicalY;

    if (InLen < sizeof(*in)) return STATUS_INVALID_BUFFER_SIZE;
    status = WdfRequestRetrieveInputBuffer(Req, sizeof(*in), (PVOID*)&in, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (in->Size != sizeof(*in)) return STATUS_INVALID_PARAMETER;

    if (!fileCtx->Inner.HasMetrics) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    /*
     * Pixel -> logical mapping. Logical range is [0, VHID_MOUSE_ABS_MAX].
     * VirtualWidth / VirtualHeight are guaranteed > 0 by
     * CtlSetScreenMetrics.
     */
    denomX = fileCtx->Inner.VirtualWidth;
    denomY = fileCtx->Inner.VirtualHeight;

    numerX = ((INT64)in->XPx - fileCtx->Inner.VirtualX) * VHID_MOUSE_ABS_MAX;
    numerY = ((INT64)in->YPx - fileCtx->Inner.VirtualY) * VHID_MOUSE_ABS_MAX;

    logicalX = numerX / denomX;
    logicalY = numerY / denomY;

    if (logicalX < 0) logicalX = 0;
    if (logicalY < 0) logicalY = 0;
    if (logicalX > VHID_MOUSE_ABS_MAX) logicalX = VHID_MOUSE_ABS_MAX;
    if (logicalY > VHID_MOUSE_ABS_MAX) logicalY = VHID_MOUSE_ABS_MAX;

    RtlZeroMemory(&report, sizeof(report));
    report.ReportId = VHID_REPORTID_MOUSE_ABS;
    report.Buttons  = in->Buttons & VHID_MOUSE_BTN_MASK;
    report.X        = (USHORT)logicalX;
    report.Y        = (USHORT)logicalY;
    report.Wheel    = in->Wheel;
    report.HWheel   = in->HWheel;

    return VhidkmDevicePostInputReport(Dev,
        (const UCHAR*)&report, (UCHAR)sizeof(report));
}

static
NTSTATUS
CtlSetScreenMetrics(
    _In_ WDFFILEOBJECT  FileObject,
    _In_ WDFREQUEST     Req,
    _In_ size_t         InLen
    )
{
    PVHID_SCREEN_METRICS        in;
    PVHIDKM_CTL_FILE_CONTEXT    fileCtx = VhidkmCtlFileGetContext(FileObject);
    NTSTATUS                    status;

    if (InLen < sizeof(*in)) return STATUS_INVALID_BUFFER_SIZE;
    status = WdfRequestRetrieveInputBuffer(Req, sizeof(*in), (PVOID*)&in, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (in->Size != sizeof(*in)) return STATUS_INVALID_PARAMETER;

    if (in->VirtualWidth  < VHID_SCREEN_METRIC_MIN_EXTENT ||
        in->VirtualHeight < VHID_SCREEN_METRIC_MIN_EXTENT ||
        in->VirtualWidth  > VHID_SCREEN_METRIC_MAX_EXTENT ||
        in->VirtualHeight > VHID_SCREEN_METRIC_MAX_EXTENT) {
        return STATUS_INVALID_PARAMETER;
    }

    fileCtx->Inner.VirtualX      = in->VirtualX;
    fileCtx->Inner.VirtualY      = in->VirtualY;
    fileCtx->Inner.VirtualWidth  = in->VirtualWidth;
    fileCtx->Inner.VirtualHeight = in->VirtualHeight;
    fileCtx->Inner.HasMetrics    = TRUE;
    return STATUS_SUCCESS;
}

static
NTSTATUS
CtlGetLedState(
    _In_  PVHIDKM_DEVICE_CONTEXT Dev,
    _In_  WDFREQUEST             Req,
    _In_  size_t                 OutLen,
    _Out_ PULONG_PTR              Info
    )
{
    PUCHAR   out;
    size_t   olen;
    NTSTATUS status;

    *Info = 0;

    if (OutLen < sizeof(UCHAR)) return STATUS_INVALID_BUFFER_SIZE;

    status = WdfRequestRetrieveOutputBuffer(Req, sizeof(UCHAR),
                                            (PVOID*)&out, &olen);
    if (!NT_SUCCESS(status)) return status;

    *out = VhidkmLedStateRead(Dev);
    *Info = sizeof(UCHAR);
    return STATUS_SUCCESS;
}

static
NTSTATUS
CtlReset(
    _In_ PVHIDKM_DEVICE_CONTEXT Dev
    )
{
    VHID_KEYBOARD_INPUT_REPORT  kReport = { 0 };
    VHID_MOUSE_REL_REPORT       mReport = { 0 };
    NTSTATUS                    status;

    kReport.ReportId = VHID_REPORTID_KEYBOARD_INPUT;
    status = VhidkmDevicePostInputReport(Dev,
        (const UCHAR*)&kReport, (UCHAR)sizeof(kReport));
    if (!NT_SUCCESS(status)) return status;

    mReport.ReportId = VHID_REPORTID_MOUSE_REL;
    return VhidkmDevicePostInputReport(Dev,
        (const UCHAR*)&mReport, (UCHAR)sizeof(mReport));
}
