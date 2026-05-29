/*
 * hid/hid_ioctls.c
 *
 * EvtIoInternalDeviceControl for the HID minidriver.
 *
 * Contract with hidclass.sys:
 *
 *   IOCTL_HID_GET_DEVICE_DESCRIPTOR
 *     Return the 9-byte HID class descriptor exactly as published
 *     under USB (bcdHID / bCountryCode / bNumDescriptors /
 *     bReportDescriptorType / wReportDescriptorLength). The buffer
 *     is Irp->UserBuffer under METHOD_NEITHER; KMDF hands it back
 *     as the output memory.
 *
 *   IOCTL_HID_GET_REPORT_DESCRIPTOR
 *     Return the full report descriptor. Size is queried first (the
 *     class driver passes OutputBufferLength = 0) and then the
 *     buffer is allocated and the IOCTL re-issued.
 *
 *   IOCTL_HID_GET_DEVICE_ATTRIBUTES
 *     Fill HID_DEVICE_ATTRIBUTES { VendorID, ProductID, VersionNumber }
 *     from our canonical version constants.
 *
 *   IOCTL_HID_GET_STRING
 *     Caller-selectable string (manufacturer, product, serial). We
 *     emit UTF-16LE with no length prefix — hidclass copies it as-is
 *     into its own descriptor cache and exposes it through the HID
 *     descriptor-level APIs.
 *
 *   IOCTL_HID_READ_REPORT
 *     The class driver posts one outstanding read at a time. We
 *     forward every such IRP to the manual pending-read queue where
 *     it waits for an injection from the control device.
 *
 *   IOCTL_HID_WRITE_REPORT / IOCTL_HID_SET_OUTPUT_REPORT
 *     Output reports arrive here on the keyboard LED path. We parse
 *     the LED byte and update the cached LED baseline via
 *     VhidkmDeviceUpdateLedState; the bus driver is notified through
 *     the published IPC interface so its own LED waiters wake.
 *
 *   IOCTL_HID_GET_FEATURE / IOCTL_HID_SET_FEATURE
 *     No feature reports in v1. Return STATUS_NOT_SUPPORTED.
 *
 *   IOCTL_HID_GET_INPUT_REPORT
 *     Synchronous report pull. Not used by kbdhid/mouhid on the hot
 *     path; still answered by peeking at the ring buffer so a
 *     diagnostic caller can retrieve state without breaking the
 *     pending-read pattern.
 *
 *   IOCTL_HID_ACTIVATE_DEVICE / IOCTL_HID_DEACTIVATE_DEVICE
 *     No-ops that return STATUS_SUCCESS. The virtual device has no
 *     physical state to quiesce; hidclass.sys still requires a
 *     success status for these to finish its internal state machine.
 *
 *   IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST
 *     Answered per documented contract — we pend the request in a
 *     dedicated queue and complete it when the device genuinely
 *     idles (no pending reads for IdleTimeout). v1 simplifies this
 *     to STATUS_NOT_SUPPORTED, which hidclass.sys tolerates and
 *     falls back to the standard idle timer.
 */

#include "driver.h"
#include "device.h"
#include "hid_ioctls.h"
#include "hid_descriptor.h"
#include "report_queue.h"
#include "led_state.h"
#include "bus_iface.h"
#include "trace.h"
#include "hid_ioctls.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VhidkmHidIoctlInitialize)
#endif

EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL VhidkmEvtIoInternalDeviceControl;

static NTSTATUS HidGetDeviceDescriptor(_In_ WDFREQUEST Request,
                                       _Out_ PULONG_PTR Info);
static NTSTATUS HidGetReportDescriptor(_In_ WDFREQUEST Request,
                                       _Out_ PULONG_PTR Info);
static NTSTATUS HidGetDeviceAttributes(_In_ WDFREQUEST Request,
                                       _Out_ PULONG_PTR Info);
static NTSTATUS HidGetString(_In_ PVHIDKM_DEVICE_CONTEXT DevCtx,
                             _In_ WDFREQUEST Request,
                             _Out_ PULONG_PTR Info);
static NTSTATUS HidWriteOrSetOutputReport(_In_ PVHIDKM_DEVICE_CONTEXT DevCtx,
                                          _In_ WDFREQUEST Request);
static NTSTATUS HidGetInputReport(_In_ PVHIDKM_DEVICE_CONTEXT DevCtx,
                                  _In_ WDFREQUEST Request,
                                  _Out_ PULONG_PTR Info);

_Use_decl_annotations_
NTSTATUS
VhidkmHidIoctlInitialize(
    PVHIDKM_DEVICE_CONTEXT DevCtx
    )
{
    WDF_IO_QUEUE_CONFIG cfg;
    NTSTATUS            status;

    PAGED_CODE();

    /*
     * Default queue dispatches parallel — HID-internal IOCTLs are
     * mostly stateless reads (descriptors, attributes, strings) and
     * the pending-read path moves the IRP into a separate manual
     * queue before any work happens, so parallelism is safe.
     */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&cfg, WdfIoQueueDispatchParallel);
    cfg.EvtIoInternalDeviceControl = VhidkmEvtIoInternalDeviceControl;

    /*
     * Power-managed queue: holds requests while the device is idle.
     * hidclass.sys expects the HID stack to queue pending reads
     * during low-power states rather than fail them.
     */
    cfg.PowerManaged = WdfTrue;

    status = WdfIoQueueCreate(DevCtx->Device, &cfg,
                              WDF_NO_OBJECT_ATTRIBUTES, NULL);
    return status;
}

_Use_decl_annotations_
VOID
VhidkmEvtIoInternalDeviceControl(
    WDFQUEUE    Queue,
    WDFREQUEST  Request,
    size_t      OutputBufferLength,
    size_t      InputBufferLength,
    ULONG       IoControlCode
    )
{
    WDFDEVICE               device = WdfIoQueueGetDevice(Queue);
    PVHIDKM_DEVICE_CONTEXT  ctx    = VhidkmDeviceGetContext(device);
    NTSTATUS                status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR               info   = 0;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    switch (IoControlCode) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = HidGetDeviceDescriptor(Request, &info);
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = HidGetReportDescriptor(Request, &info);
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        status = HidGetDeviceAttributes(Request, &info);
        break;

    case IOCTL_HID_GET_STRING:
        status = HidGetString(ctx, Request, &info);
        break;

    case IOCTL_HID_READ_REPORT:
        /*
         * Route the pending read into the manual queue. The
         * forwarding call does not complete the request — that
         * happens when injection arrives or the queue is torn down.
         */
        status = VhidkmReportQueueForwardRead(ctx, Request);
        if (status == STATUS_PENDING) {
            return;
        }
        break;

    case IOCTL_HID_WRITE_REPORT:
    case IOCTL_HID_SET_OUTPUT_REPORT:
        status = HidWriteOrSetOutputReport(ctx, Request);
        break;

    case IOCTL_HID_GET_INPUT_REPORT:
        status = HidGetInputReport(ctx, Request, &info);
        break;

    case IOCTL_HID_GET_FEATURE:
    case IOCTL_HID_SET_FEATURE:
        /* No feature reports declared in the descriptor. */
        status = STATUS_NOT_SUPPORTED;
        break;

    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        /*
         * hidclass.sys issues these to quiesce the device on power
         * transitions. No hardware to quiesce; succeed silently.
         */
        status = STATUS_SUCCESS;
        break;

    case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
        /*
         * Returning NOT_SUPPORTED is the documented fallback; the
         * class driver then relies on its own idle timer which we
         * already feed via the S0 idle settings on the FDO.
         */
        status = STATUS_NOT_SUPPORTED;
        break;

    default:
        TraceHid("unknown HID IOCTL 0x%08x", IoControlCode);
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
}

static
NTSTATUS
HidGetDeviceDescriptor(
    _In_ WDFREQUEST  Request,
    _Out_ PULONG_PTR Info
    )
{
    PHID_DESCRIPTOR out;
    size_t          outLen = 0;
    NTSTATUS        status;

    *Info = 0;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(HID_DESCRIPTOR),
                                            (PVOID*)&out, &outLen);
    if (!NT_SUCCESS(status)) {
        TraceHid("GetDeviceDescriptor: buffer retrieve failed %!STATUS!", status);
        return status;
    }

    VhidkmHidGetClassDescriptor(out);
    *Info = sizeof(HID_DESCRIPTOR);
    return STATUS_SUCCESS;
}

static
NTSTATUS
HidGetReportDescriptor(
    _In_ WDFREQUEST  Request,
    _Out_ PULONG_PTR Info
    )
{
    PVOID    out;
    size_t   outLen = 0;
    ULONG    descSize = VhidkmHidGetReportDescriptorSize();
    NTSTATUS status;

    *Info = 0;

    status = WdfRequestRetrieveOutputBuffer(Request, descSize,
                                            &out, &outLen);
    if (!NT_SUCCESS(status)) {
        TraceHid("GetReportDescriptor: buffer retrieve failed %!STATUS!", status);
        return status;
    }
    if (outLen < descSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    VhidkmHidCopyReportDescriptor(out, (ULONG)outLen);
    *Info = descSize;
    return STATUS_SUCCESS;
}

static
NTSTATUS
HidGetDeviceAttributes(
    _In_ WDFREQUEST  Request,
    _Out_ PULONG_PTR Info
    )
{
    PHID_DEVICE_ATTRIBUTES  out;
    size_t                  outLen;
    NTSTATUS                status;

    *Info = 0;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(HID_DEVICE_ATTRIBUTES),
                                            (PVOID*)&out, &outLen);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(out, sizeof(*out));
    out->Size          = sizeof(HID_DEVICE_ATTRIBUTES);
    out->VendorID      = VHID_DEFAULT_VID;
    out->ProductID     = VHID_DEFAULT_PID;
    out->VersionNumber = VHID_DEFAULT_REV;
    *Info = sizeof(HID_DEVICE_ATTRIBUTES);
    return STATUS_SUCCESS;
}

static
NTSTATUS
HidGetString(
    _In_  PVHIDKM_DEVICE_CONTEXT DevCtx,
    _In_  WDFREQUEST             Request,
    _Out_ PULONG_PTR              Info
    )
{
    /*
     * IOCTL_HID_GET_STRING carries the string ID in the low word of
     * Parameters.DeviceIoControl.Type3InputBuffer (it is a METHOD_NEITHER
     * IOCTL). WDF_REQUEST_PARAMETERS exposes it through
     * Parameters.DeviceIoControl.Type3InputBuffer cast to ULONG_PTR.
     *
     * Indices we answer:
     *   HID_STRING_ID_IMANUFACTURER (1) -> "Virtual HID Systems"
     *   HID_STRING_ID_IPRODUCT      (2) -> "Virtual HID Keyboard + Mouse"
     *   HID_STRING_ID_ISERIALNUMBER (3) -> serial baked by the bus
     *                                       driver's descriptor data
     */
    WDF_REQUEST_PARAMETERS  params;
    PVOID                   out;
    size_t                  outLen = 0;
    NTSTATUS                status;
    ULONG                   stringId;
    const WCHAR*            src   = NULL;
    size_t                  srcBytes = 0;

    *Info = 0;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    stringId = (ULONG)(ULONG_PTR)params.Parameters.DeviceIoControl.Type3InputBuffer;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(WCHAR),
                                            &out, &outLen);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    switch (stringId & 0xFFFF) {
    case HID_STRING_ID_IMANUFACTURER:
        src = VHID_DEFAULT_MFG_STRING_W;
        break;
    case HID_STRING_ID_IPRODUCT:
        src = VHID_DEFAULT_PRODUCT_STRING_W;
        break;
    case HID_STRING_ID_ISERIALNUMBER:
        /*
         * The serial is generated by the bus driver per plug. We
         * reach it via the published IPC interface if available;
         * otherwise fall back to a stable default so the IOCTL does
         * not fail during the brief window between PrepareHardware
         * starting and the bus interface being acquired.
         */
        if (DevCtx->IpcValid && DevCtx->BusIpc.GetUsbDescriptor != NULL) {
            UCHAR  buf[256];
            ULONG  wrote = 0;
            NTSTATUS s = DevCtx->BusIpc.GetUsbDescriptor(
                DevCtx->BusIpc.InterfaceHeader.Context,
                0x03 /* STRING */, 3, buf, sizeof(buf), &wrote);
            if (NT_SUCCESS(s) && wrote >= 2) {
                /*
                 * The bus returns a USB string descriptor: a 2-byte
                 * header (bLength, bDescriptorType) followed by a
                 * UTF-16LE payload with no terminator. hidclass expects
                 * a null-terminated wide string, so strip the header,
                 * copy whole WCHARs only, and append a terminator. We
                 * never truncate mid-WCHAR or hand back an unterminated
                 * buffer.
                 */
                const WCHAR* payload = (const WCHAR*)(buf + 2);
                size_t payloadBytes = (size_t)wrote - 2;
                size_t needBytes;

                payloadBytes &= ~(size_t)(sizeof(WCHAR) - 1);
                needBytes = payloadBytes + sizeof(WCHAR);

                if (outLen < needBytes) {
                    *Info = needBytes;
                    return STATUS_BUFFER_TOO_SMALL;
                }
                if (payloadBytes > 0) {
                    RtlCopyMemory(out, payload, payloadBytes);
                }
                ((WCHAR*)out)[payloadBytes / sizeof(WCHAR)] = L'\0';
                *Info = needBytes;
                return STATUS_SUCCESS;
            }
        }
        src = L"000102030405060708090A0B0C0D0E0F";
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    srcBytes = (wcslen(src) + 1) * sizeof(WCHAR);
    if (outLen < srcBytes) {
        *Info = srcBytes;
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(out, src, srcBytes);
    *Info = srcBytes;
    return STATUS_SUCCESS;
}

static
NTSTATUS
HidWriteOrSetOutputReport(
    _In_ PVHIDKM_DEVICE_CONTEXT DevCtx,
    _In_ WDFREQUEST             Request
    )
{
    /*
     * Parse the output report. The LED byte is at offset 1 (after
     * the ReportID). Any other report IDs are unknown on this path
     * and silently accepted — hidclass.sys occasionally sends zero-
     * sized writes during device startup; rejecting them would
     * trigger spurious error paths in the class driver.
     */
    WDF_REQUEST_PARAMETERS  params;
    PVOID                   buf     = NULL;
    size_t                  bufLen  = 0;
    ULONG                   length  = 0;
    UCHAR                   reportId = 0;
    UCHAR                   ledBits  = 0;
    HID_XFER_PACKET*        packet;
    NTSTATUS                status;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    /*
     * hidclass.sys passes output/feature reports as a HID_XFER_PACKET
     * via METHOD_NEITHER. WdfRequestRetrieveOutputBuffer will not
     * find it; the packet sits in Parameters.Others.Arg1 for the
     * SET paths. Extract it defensively: fall back to
     * WdfRequestRetrieveInputBuffer when Others.Arg1 is NULL (the
     * older IOCTL_HID_WRITE_REPORT route).
     */
    packet = (HID_XFER_PACKET*)params.Parameters.Others.Arg1;

    if (packet != NULL && packet->reportBuffer != NULL && packet->reportBufferLen > 0) {
        buf    = packet->reportBuffer;
        bufLen = packet->reportBufferLen;
    } else {
        status = WdfRequestRetrieveInputBuffer(Request, 1, &buf, &bufLen);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    if (bufLen < 1) {
        return STATUS_INVALID_PARAMETER;
    }

    reportId = ((PUCHAR)buf)[0];
    length   = (ULONG)bufLen;

    if (reportId == VHID_REPORTID_KEYBOARD_OUTPUT) {
        if (length < sizeof(VHID_KEYBOARD_OUTPUT_REPORT)) {
            return STATUS_INVALID_PARAMETER;
        }
        ledBits = ((PUCHAR)buf)[1] & VHID_KBD_LED_MASK;
        VhidkmDeviceUpdateLedState(DevCtx, ledBits);

        /*
         * Forward the LED change to the bus driver so its own
         * waiters (IOCTL on the bus control device, if ever added)
         * can react. Errors are swallowed — the local state update
         * has already happened and must not be reverted by an IPC
         * failure.
         */
        if (DevCtx->IpcValid && DevCtx->BusIpc.NotifyLedChange != NULL) {
            (VOID)DevCtx->BusIpc.NotifyLedChange(
                DevCtx->BusIpc.InterfaceHeader.Context, ledBits);
        }
        TraceLed("output report ID=%u leds=0x%02x", reportId, ledBits);
        return STATUS_SUCCESS;
    }

    /* Unknown report id on output — treat as accepted no-op. */
    TraceLed("ignored output report ID=%u len=%u", reportId, length);
    return STATUS_SUCCESS;
}

static
NTSTATUS
HidGetInputReport(
    _In_  PVHIDKM_DEVICE_CONTEXT DevCtx,
    _In_  WDFREQUEST             Request,
    _Out_ PULONG_PTR              Info
    )
{
    WDF_REQUEST_PARAMETERS params;
    HID_XFER_PACKET*       packet;
    PUCHAR                 buf      = NULL;
    ULONG                  bufLen   = 0;
    UCHAR                  reportId = 0;
    NTSTATUS               status;

    *Info = 0;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    packet = (HID_XFER_PACKET*)params.Parameters.Others.Arg1;
    if (packet == NULL || packet->reportBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    buf       = packet->reportBuffer;
    bufLen    = packet->reportBufferLen;
    reportId  = packet->reportId;

    status = VhidkmReportQueuePeek(DevCtx, reportId, buf, bufLen, (PULONG)Info);
    return status;
}
