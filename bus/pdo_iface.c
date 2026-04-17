/*
 * bus/pdo_iface.c
 *
 * Implementation of the published bus IPC interface plus the
 * IRP_MN_QUERY_INTERFACE preprocess path that hands the interface
 * to the HID minidriver.
 *
 * Callback implementations:
 *   VusbBusIpcGetUsbDescriptor      — defers to bus/usbdesc.c.
 *   VusbBusIpcNotifyFunctionReady   — stores the bound HID FDO,
 *                                     sets FunctionReady flag.
 *   VusbBusIpcNotifyLedChange       — updates cached LED state and
 *                                     wakes pending IOCTL waiters.
 *   VusbBusIpcOnUnplug              — no-op in v1; kept as a
 *                                     symmetric hook for future use.
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
     * Register a WDM preprocessor for IRP_MJ_PNP; we pass NULL for
     * the MinorFunctions pointer so the filter fires for every
     * minor code and we can inspect MinorFunction ourselves. KMDF
     * delivers control to our routine before dispatching to its
     * own PnP handler; we call IoSkipCurrentIrpStackLocation +
     * complete the IRP ourselves only for QUERY_INTERFACE, else we
     * forward to the framework.
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

    if (stack->MinorFunction != IRP_MN_QUERY_INTERFACE) {
        /*
         * Pass everything else through to KMDF. The framework
         * calls IoSkipCurrentIrpStackLocation itself when its
         * dispatcher returns, so we must not touch the IRP
         * beyond forwarding it.
         */
        IoSkipCurrentIrpStackLocation(Irp);
        return WdfDeviceWdmDispatchPreprocessedIrp(
            WdfWdmDeviceGetWdfDeviceHandle(DeviceObject),
            Irp);
    }

    if (!IsEqualGUID(stack->Parameters.QueryInterface.InterfaceType,
                     &GUID_VHID_BUS_INTERFACE_V1)) {
        IoSkipCurrentIrpStackLocation(Irp);
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
     * The PDO is parented to the WDFDRIVER; taking a WDFOBJECT
     * reference on the PDO handle keeps it alive past
     * surprise-remove until the HID minidriver releases its
     * interface handle.
     */
    if (pdoCtx != NULL && pdoCtx->Slot != NULL && pdoCtx->Slot->PdoDevice != NULL) {
        WdfObjectReference(pdoCtx->Slot->PdoDevice);
    }
}

static
VOID
VusbBusIpcDereference(
    PVOID Ctx
    )
{
    PVUSBBUS_PDO_CONTEXT pdoCtx = (PVUSBBUS_PDO_CONTEXT)Ctx;
    if (pdoCtx != NULL && pdoCtx->Slot != NULL && pdoCtx->Slot->PdoDevice != NULL) {
        WdfObjectDereference(pdoCtx->Slot->PdoDevice);
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
    UNREFERENCED_PARAMETER(Context);
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
     * Wake every pending waiter. Each waiter's output buffer holds
     * a single UCHAR; we fill it with the new baseline and complete
     * the request with STATUS_SUCCESS.
     *
     * This is safe to run at DISPATCH_LEVEL because
     * WdfIoQueueRetrieveNextRequest and WdfRequestComplete tolerate
     * it (the former uses a spinlock internally; the latter defers
     * user-mode completion via an APC from the framework).
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
