/*
 * bus/queue.c
 *
 * Default IO queue for the bus control device and the
 * EvtIoDeviceControl handler that dispatches IOCTL_VUSBBUS_*
 * requests.
 *
 * Validation rules (applied uniformly per docs/ARCHITECTURE.md §7.4):
 *
 *   - InputBufferLength and OutputBufferLength checked against
 *     sizeof(struct) before any field access. Short buffers are
 *     failed with STATUS_INVALID_BUFFER_SIZE. Under METHOD_BUFFERED
 *     the framework has already copied the caller's input into
 *     kernel-owned memory, so the pointer itself is safe to read.
 *
 *   - Request structs with a leading Size field MUST have
 *     Size == sizeof(struct). Rejects "buffer too large"
 *     version-smuggling attempts.
 *
 *   - Request data that maps to numeric fields with bounded ranges
 *     is range-checked at this layer before being passed to FDO
 *     helpers.
 */

#include "driver.h"
#include "queue.h"
#include "fdo.h"
#include "usbdesc.h"
#include "trace.h"
#include "queue.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VusbBusQueueInitialize)
#pragma alloc_text(PAGE, VusbBusEvtIoDeviceControl)
#endif

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL  VusbBusEvtIoDeviceControl;

_Use_decl_annotations_
NTSTATUS
VusbBusQueueInitialize(
    WDFDEVICE            ControlDevice,
    PVUSBBUS_FDO_CONTEXT FdoCtx
    )
{
    WDF_IO_QUEUE_CONFIG     cfg;
    WDF_OBJECT_ATTRIBUTES   attr;
    PVUSBBUS_CTL_DEV_CONTEXT ctx;
    NTSTATUS                status;

    PAGED_CODE();

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, VUSBBUS_CTL_DEV_CONTEXT);
    status = WdfObjectAllocateContext(ControlDevice, &attr, (PVOID*)&ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx->FdoCtx = FdoCtx;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&cfg, WdfIoQueueDispatchParallel);
    cfg.EvtIoDeviceControl = VusbBusEvtIoDeviceControl;
    /*
     * Power-managed = FALSE because the control device is not a
     * PnP device and has no power state; setting it to TRUE would
     * cause KMDF to wait on a nonexistent PnP transition at start.
     */
    cfg.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(ControlDevice, &cfg, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    return status;
}

static
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
BusCtlGetVersion(
    _In_  WDFREQUEST Request,
    _In_  size_t     OutputLen,
    _Out_ PULONG_PTR BytesReturned
    )
{
    NTSTATUS        status;
    PVHID_VERSION   out;

    *BytesReturned = 0;

    if (OutputLen < sizeof(VHID_VERSION)) {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(VHID_VERSION),
                                            (PVOID*)&out, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    out->Major    = VHID_VERSION_MAJOR;
    out->Minor    = VHID_VERSION_MINOR;
    out->Build    = VHID_VERSION_BUILD;
    out->ApiLevel = VHID_API_LEVEL;
    *BytesReturned = sizeof(VHID_VERSION);
    return STATUS_SUCCESS;
}

static
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
BusCtlPlugIn(
    _In_  PVUSBBUS_FDO_CONTEXT FdoCtx,
    _In_  WDFREQUEST           Request,
    _In_  size_t               InputLen,
    _In_  size_t               OutputLen,
    _Out_ PULONG_PTR           BytesReturned
    )
{
    NTSTATUS            status;
    PVHID_PLUGIN_REQ    in;
    PVHID_PLUGIN_RESP   out;

    *BytesReturned = 0;

    if (InputLen  < sizeof(VHID_PLUGIN_REQ))  return STATUS_INVALID_BUFFER_SIZE;
    if (OutputLen < sizeof(VHID_PLUGIN_RESP)) return STATUS_INVALID_BUFFER_SIZE;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(VHID_PLUGIN_REQ),
                                           (PVOID*)&in, NULL);
    if (!NT_SUCCESS(status)) return status;
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(VHID_PLUGIN_RESP),
                                            (PVOID*)&out, NULL);
    if (!NT_SUCCESS(status)) return status;

    if (in->Size != sizeof(VHID_PLUGIN_REQ)) return STATUS_INVALID_PARAMETER;

    status = VusbBusFdoPlugIn(FdoCtx, in, out);
    if (NT_SUCCESS(status)) {
        *BytesReturned = sizeof(VHID_PLUGIN_RESP);
    }
    return status;
}

static
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
BusCtlUnplug(
    _In_ PVUSBBUS_FDO_CONTEXT FdoCtx,
    _In_ WDFREQUEST           Request,
    _In_ size_t               InputLen
    )
{
    NTSTATUS          status;
    PVHID_UNPLUG_REQ  in;

    if (InputLen < sizeof(VHID_UNPLUG_REQ)) return STATUS_INVALID_BUFFER_SIZE;
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(VHID_UNPLUG_REQ),
                                           (PVOID*)&in, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (in->Size != sizeof(VHID_UNPLUG_REQ)) return STATUS_INVALID_PARAMETER;

    return VusbBusFdoUnplug(FdoCtx, in->SlotId);
}

static
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
BusCtlList(
    _In_  PVUSBBUS_FDO_CONTEXT FdoCtx,
    _In_  WDFREQUEST           Request,
    _In_  size_t               OutputLen,
    _Out_ PULONG_PTR           BytesReturned
    )
{
    NTSTATUS         status;
    PVHID_SLOT_LIST  out;
    ULONG            wrote = 0;

    *BytesReturned = 0;

    if (OutputLen < sizeof(VHID_SLOT_LIST)) return STATUS_INVALID_BUFFER_SIZE;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(VHID_SLOT_LIST),
                                            (PVOID*)&out, NULL);
    if (!NT_SUCCESS(status)) return status;

    status = VusbBusFdoList(FdoCtx, out, (ULONG)OutputLen, &wrote);
    *BytesReturned = wrote;
    return status;
}

static
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
BusCtlGetUsbDescriptor(
    _In_  WDFREQUEST Request,
    _In_  size_t     InputLen,
    _In_  size_t     OutputLen,
    _Out_ PULONG_PTR BytesReturned
    )
{
    NTSTATUS       status;
    PVHID_DESC_REQ in;
    PVOID          out;
    ULONG          wrote = 0;

    *BytesReturned = 0;

    if (InputLen < sizeof(VHID_DESC_REQ)) return STATUS_INVALID_BUFFER_SIZE;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(VHID_DESC_REQ),
                                           (PVOID*)&in, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (in->Size != sizeof(VHID_DESC_REQ)) return STATUS_INVALID_PARAMETER;

    if (OutputLen == 0) return STATUS_INVALID_BUFFER_SIZE;

    status = WdfRequestRetrieveOutputBuffer(Request, 1, &out, NULL);
    if (!NT_SUCCESS(status)) return status;

    status = VusbBusUsbDescCopy(in->Type, in->Index, out,
                                (ULONG)OutputLen, &wrote);
    *BytesReturned = wrote;
    return status;
}

_Use_decl_annotations_
VOID
VusbBusEvtIoDeviceControl(
    WDFQUEUE   Queue,
    WDFREQUEST Request,
    size_t     OutputBufferLength,
    size_t     InputBufferLength,
    ULONG      IoControlCode
    )
{
    WDFDEVICE               ctlDevice = WdfIoQueueGetDevice(Queue);
    PVUSBBUS_CTL_DEV_CONTEXT ctx       = VusbBusCtlDevGetContext(ctlDevice);
    NTSTATUS                status    = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR               info      = 0;

    PAGED_CODE();

    switch (IoControlCode) {
    case IOCTL_VUSBBUS_GET_VERSION:
        status = BusCtlGetVersion(Request, OutputBufferLength, &info);
        break;

    case IOCTL_VUSBBUS_PLUG_IN:
        status = BusCtlPlugIn(ctx->FdoCtx, Request,
                              InputBufferLength, OutputBufferLength, &info);
        break;

    case IOCTL_VUSBBUS_UNPLUG:
        status = BusCtlUnplug(ctx->FdoCtx, Request, InputBufferLength);
        break;

    case IOCTL_VUSBBUS_LIST:
        status = BusCtlList(ctx->FdoCtx, Request, OutputBufferLength, &info);
        break;

    case IOCTL_VUSBBUS_GET_USB_DESCRIPTOR:
        status = BusCtlGetUsbDescriptor(Request,
                                        InputBufferLength, OutputBufferLength,
                                        &info);
        break;

    default:
        TraceIoctl("unknown IoControlCode 0x%08x", IoControlCode);
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
}
