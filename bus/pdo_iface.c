/*
 * bus/pdo_iface.c
 *
 * Implementation of the published bus IPC interface plus the
 * IRP_MN_QUERY_INTERFACE preprocess path that hands the interface to
 * the HID minidriver.
 *
 * Callback implementations:
 *   VusbBusIpcGetUsbDescriptor      - defers to bus/usbdesc.c, and uses
 *                                     the per-instance serial captured
 *                                     in the PDO context for index 3.
 *   VusbBusIpcNotifyFunctionReady   - stores the bound HID FDO, sets
 *                                     the FunctionReady flag.
 *   VusbBusIpcNotifyLedChange       - updates cached LED state and wakes
 *                                     pending IOCTL waiters.
 *   VusbBusIpcOnUnplug              - no-op in v1; kept as a symmetric
 *                                     hook for future use.
 */

#include "driver.h"
#include "pdo.h"
#include "pdo_iface.h"
#include "usbdesc.h"
#include "trace.h"
#include "pdo_iface.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VusbBusPdoSetInterfaceDispatch)
#endif

static VOID VusbBusIpcReference(_In_ PVOID Ctx);
static VOID VusbBusIpcDereference(_In_ PVOID Ctx);

static
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
VusbBusIpcGetUsbDescriptor(
    _In_ PVOID Context,
    _In_ UCHAR DescriptorType,
    _In_ UCHAR DescriptorIndex,
    _In_reads_bytes_(BufferLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG BytesReturned
    );

static
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
VusbBusIpcNotifyFunctionReady(
    _In_ PVOID Context,
    _In_ PDEVICE_OBJECT HidFdo
    );

static
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
VusbBusIpcNotifyLedChange(
    _In_ PVOID Context,
    _In_ UCHAR LedBits
    );

static
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
VusbBusIpcOnUnplug(
    _In_ PVOID Context
    );

/* WDM preprocess entry point for IRP_MJ_PNP on the PDO. */
static DRIVER_DISPATCH VusbBusPdoPnpPreprocess;

_Use_decl_annotations_
NTSTATUS
VusbBusPdoSetInterfaceDispatch(
    PWDFDEVICE_INIT DeviceInit
    )
{
    PAGED_CODE();

    /*
     * Register a WDM preprocessor for IRP_MJ_PNP; we pass NULL for the
     * MinorFunctions pointer so the filter fires for every minor code
     * and we can inspect MinorFunction ourselves. KMDF delivers control
     * to our routine before its own PnP handling; for QUERY_INTERFACE of
     * our GUID we complete the IRP ourselves, otherwise we hand it back
     * to the framework unchanged with WdfDeviceWdmDispatchPreprocessedIrp.
     */
    return WdfDeviceInitAssignWdmIrpPreprocessCallback(
        DeviceInit,
        VusbBusPdoPnpPreprocess,
        IRP_MJ_PNP,
        NULL,
        0);
}

static
_Use_decl_annotations_
NTSTATUS
VusbBusPdoPnpPreprocess(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    PIO_STACK_LOCATION  stack = IoGetCurrentIrpStackLocation(Irp);
    WDFDEVICE           wdfPdo;
    PVUSBBUS_PDO_CONTEXT ctx;
    NTSTATUS            status;

    /*
     * Anything that is not the interface query we answer is handed back
     * to the framework as-is. We must NOT advance the IRP stack location
     * before doing so: WdfDeviceWdmDispatchPreprocessedIrp expects the
     * IRP positioned exactly where KMDF gave it to us. Calling
     * IoSkipCurrentIrpStackLocation here would corrupt the PnP/power
     * dispatch on this PDO.
     */
    if (stack->MinorFunction != IRP_MN_QUERY_INTERFACE) {
        return WdfDeviceWdmDispatchPreprocessedIrp(
            WdfWdmDeviceGetWdfDeviceHandle(DeviceObject),
            Irp);
    }

    if (!IsEqualGUID(stack->Parameters.QueryInterface.InterfaceType,
                     &GUID_VHID_BUS_INTERFACE_V1)) {
        return WdfDeviceWdmDispatchPreprocessedIrp(
            WdfWdmDeviceGetWdfDeviceHandle(DeviceObject),
            Irp);
    }

    if (stack->Parameters.QueryInterface.Size < sizeof(VHID_BUS_INTERFACE_V1) ||
        stack->Parameters.QueryInterface.Version != VHID_BUS_INTERFACE_VERSION_V1) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    wdfPdo = WdfWdmDeviceGetWdfDeviceHandle(DeviceObject);
    ctx    = VusbBusPdoGetContext(wdfPdo);

    {
        PVHID_BUS_INTERFACE_V1 iface =
            (PVHID_BUS_INTERFACE_V1)stack->Parameters.QueryInterface.Interface;

        RtlZeroMemory(iface, sizeof(*iface));
        iface->InterfaceHeader.Size     = sizeof(VHID_BUS_INTERFACE_V1);
        iface->InterfaceHeader.Version  = VHID_BUS_INTERFACE_VERSION_V1;
        iface->InterfaceHeader.Context  = ctx;
        iface->InterfaceHeader.InterfaceReference   = VusbBusIpcReference;
        iface->InterfaceHeader.InterfaceDereference = VusbBusIpcDereference;

        iface->GetUsbDescriptor     = VusbBusIpcGetUsbDescriptor;
        iface->NotifyFunctionReady  = VusbBusIpcNotifyFunctionReady;
        iface->NotifyLedChange      = VusbBusIpcNotifyLedChange;
        iface->OnUnplug             = VusbBusIpcOnUnplug;

        /* Take a reference on behalf of the caller. */
        VusbBusIpcReference(ctx);
    }

    status = STATUS_SUCCESS;
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static
VOID
VusbBusIpcReference(
    PVOID Ctx
    )
{
    PVUSBBUS_PDO_CONTEXT pdoCtx = (PVUSBBUS_PDO_CONTEXT)Ctx;
    /*
     * Reference the PDO's own framework handle, captured at creation.
     * This keeps the PDO (and its context) alive for as long as the HID
     * minidriver holds the interface, independent of the slot table,
     * which UNPLUG zeroes and a later PLUG_IN may recycle to a different
     * device.
     */
    if (pdoCtx != NULL && pdoCtx->Pdo != NULL) {
        WdfObjectReference(pdoCtx->Pdo);
    }
}

static
VOID
VusbBusIpcDereference(
    PVOID Ctx
    )
{
    PVUSBBUS_PDO_CONTEXT pdoCtx = (PVUSBBUS_PDO_CONTEXT)Ctx;
    if (pdoCtx != NULL && pdoCtx->Pdo != NULL) {
        WdfObjectDereference(pdoCtx->Pdo);
    }
}

static
_Use_decl_annotations_
NTSTATUS
VusbBusIpcGetUsbDescriptor(
    PVOID Context,
    UCHAR DescriptorType,
    UCHAR DescriptorIndex,
    PVOID Buffer,
    ULONG BufferLength,
    PULONG BytesReturned
    )
{
    PVUSBBUS_PDO_CONTEXT pdoCtx = (PVUSBBUS_PDO_CONTEXT)Context;

    /*
     * iSerialNumber (string index 3) is per-instance: serve it from the
     * serial captured in the PDO context so HidD_GetSerialNumberString
     * reflects the value the caller passed to PLUG_IN. Everything else
     * is a fixed table.
     */
    if (DescriptorType == USB_DESC_TYPE_STRING && DescriptorIndex == 3 &&
        pdoCtx != NULL) {
        return VusbBusUsbDescCopySerialString(
            pdoCtx->Serial, pdoCtx->SerialChars,
            Buffer, BufferLength, BytesReturned);
    }

    return VusbBusUsbDescCopy(
        DescriptorType, DescriptorIndex,
        Buffer, BufferLength, BytesReturned);
}

static
_Use_decl_annotations_
NTSTATUS
VusbBusIpcNotifyFunctionReady(
    PVOID Context,
    PDEVICE_OBJECT HidFdo
    )
{
    PVUSBBUS_PDO_CONTEXT ctx = (PVUSBBUS_PDO_CONTEXT)Context;

    WdfSpinLockAcquire(ctx->IpcLock);
    ctx->HidFdo         = HidFdo;
    ctx->FunctionReady  = TRUE;
    WdfSpinLockRelease(ctx->IpcLock);

    TraceIpc("IPC: function ready, HidFdo=%p", HidFdo);
    return STATUS_SUCCESS;
}

static
_Use_decl_annotations_
NTSTATUS
VusbBusIpcNotifyLedChange(
    PVOID Context,
    UCHAR LedBits
    )
{
    PVUSBBUS_PDO_CONTEXT ctx = (PVUSBBUS_PDO_CONTEXT)Context;
    WDFREQUEST          req;
    NTSTATUS            status;

    WdfSpinLockAcquire(ctx->IpcLock);
    ctx->LedBaseline = LedBits;
    WdfSpinLockRelease(ctx->IpcLock);

    /*
     * Wake every pending waiter. Each waiter's output buffer holds a
     * single UCHAR; we fill it with the new baseline and complete the
     * request with STATUS_SUCCESS.
     *
     * This is safe to run at DISPATCH_LEVEL because
     * WdfIoQueueRetrieveNextRequest and WdfRequestComplete tolerate it.
     */
    for (;;) {
        PUCHAR  outBuf  = NULL;
        size_t  outLen  = 0;

        status = WdfIoQueueRetrieveNextRequest(ctx->LedWaitQueue, &req);
        if (!NT_SUCCESS(status)) {
            break;
        }
        status = WdfRequestRetrieveOutputBuffer(req, sizeof(UCHAR),
            (PVOID*)&outBuf, &outLen);
        if (NT_SUCCESS(status) && outBuf != NULL && outLen >= sizeof(UCHAR)) {
            *outBuf = LedBits;
            WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, sizeof(UCHAR));
        } else {
            WdfRequestComplete(req, status);
        }
    }

    return STATUS_SUCCESS;
}

static
_Use_decl_annotations_
VOID
VusbBusIpcOnUnplug(
    PVOID Context
    )
{
    UNREFERENCED_PARAMETER(Context);
    TraceIpc("IPC: unplug hook invoked");
}
