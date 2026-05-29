/*
 * bus/ctl_device.c
 *
 * Control device factory for vusbbus.sys.
 *
 * The control device is a standalone WDFDEVICE through which user-mode
 * issues IOCTL_VUSBBUS_* (plug / unplug / list / descriptor). It:
 *
 *   - Declares GUID_DEVINTERFACE_VUSBBUS_CTL for SetupDi discovery.
 *   - Applies an SDDL string restricting open to SYSTEM and the
 *     Administrators group.
 *   - Hosts a default IO queue that routes IOCTLs to queue.c.
 *
 * Lifetime model:
 *   The control device is a driver-lifetime singleton. The bus FDO is
 *   root-enumerated and normally permanent, but it can be disabled and
 *   re-enabled, which removes and re-adds the FDO. Recreating a second
 *   control device with the same name would collide, so the device is
 *   built once and each (re)started FDO simply rebinds the backpointer.
 *
 *   Because the control device can outlive the FDO context it points
 *   at, the backpointer is guarded by VUSBBUS_CTL_DEV_CONTEXT.Lock and
 *   cleared on FDO teardown (VusbBusCtlDetachFdo). Readers resolve it
 *   through VusbBusCtlAcquireFdo, which references the FDO's WDFDEVICE
 *   for the duration of the call so the context cannot be freed
 *   underneath an in-flight IOCTL.
 */

#include "driver.h"
#include "ctl_device.h"
#include "queue.h"
#include "trace.h"
#include "ctl_device.tmh"

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS VusbBusCtlBuildAndPublish(_In_ PVUSBBUS_FDO_CONTEXT FdoCtx);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VusbBusCtlDeviceCreate)
#pragma alloc_text(PAGE, VusbBusCtlBuildAndPublish)
#endif

/*
 * Name of the control device under \Device. Not exposed to user-mode
 * code (that uses the interface GUID); kept stable for debugger-side
 * discovery.
 */
DECLARE_CONST_UNICODE_STRING(g_CtlDeviceName, L"\\Device\\VUsbBusCtl");

/* SDDL: SYSTEM and Administrators get full access; all others denied. */
DECLARE_CONST_UNICODE_STRING(g_CtlDeviceSddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

/* Construction-election states for g_CtlInitState. */
#define BUSCTL_STATE_UNINIT    0
#define BUSCTL_STATE_BUILDING  1
#define BUSCTL_STATE_READY     2
#define BUSCTL_STATE_FAILED    3

static volatile LONG    g_CtlInitState = BUSCTL_STATE_UNINIT;
static WDFDEVICE        g_CtlDevice    = NULL;

/* Atomic read that never mutates a non-zero value. */
static LONG VusbBusCtlReadState(void)
{
    return InterlockedCompareExchange(&g_CtlInitState,
                                      BUSCTL_STATE_UNINIT, BUSCTL_STATE_UNINIT);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static NTSTATUS VusbBusCtlAttachFdo(_In_ PVUSBBUS_FDO_CONTEXT FdoCtx)
{
    PVUSBBUS_CTL_DEV_CONTEXT ctx = VusbBusCtlDevGetContext(g_CtlDevice);

    WdfSpinLockAcquire(ctx->Lock);
    ctx->FdoCtx = FdoCtx;
    WdfSpinLockRelease(ctx->Lock);

    FdoCtx->ControlDevice = g_CtlDevice;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VusbBusCtlDeviceCreate(
    PVUSBBUS_FDO_CONTEXT FdoCtx
    )
{
    PAGED_CODE();

    for (;;) {
        LONG prev = InterlockedCompareExchange(&g_CtlInitState,
                                               BUSCTL_STATE_BUILDING,
                                               BUSCTL_STATE_UNINIT);

        if (prev == BUSCTL_STATE_FAILED) {
            if (InterlockedCompareExchange(&g_CtlInitState,
                                           BUSCTL_STATE_BUILDING,
                                           BUSCTL_STATE_FAILED) == BUSCTL_STATE_FAILED) {
                prev = BUSCTL_STATE_UNINIT;
            } else {
                prev = BUSCTL_STATE_BUILDING;
            }
        }

        if (prev == BUSCTL_STATE_UNINIT) {
            NTSTATUS status = VusbBusCtlBuildAndPublish(FdoCtx);
            InterlockedExchange(&g_CtlInitState,
                                NT_SUCCESS(status) ? BUSCTL_STATE_READY
                                                   : BUSCTL_STATE_FAILED);
            if (NT_SUCCESS(status)) {
                TracePnp("control device created and interface published");
            } else {
                TraceError("control device creation failed %!STATUS!", status);
            }
            return status;
        }

        if (prev == BUSCTL_STATE_READY) {
            return VusbBusCtlAttachFdo(FdoCtx);
        }

        /* prev == BUSCTL_STATE_BUILDING: wait for the builder to settle. */
        {
            LARGE_INTEGER interval;
            interval.QuadPart = -10 * 1000; /* 1 ms, relative */
            while (VusbBusCtlReadState() == BUSCTL_STATE_BUILDING) {
                (VOID)KeDelayExecutionThread(KernelMode, FALSE, &interval);
            }
        }
    }
}

_Use_decl_annotations_
static NTSTATUS
VusbBusCtlBuildAndPublish(
    PVUSBBUS_FDO_CONTEXT FdoCtx
    )
{
    PWDFDEVICE_INIT          devInit  = NULL;
    WDFDEVICE                ctlDevice = NULL;
    WDF_OBJECT_ATTRIBUTES    fileAttr;
    WDF_FILEOBJECT_CONFIG    fileCfg;
    WDF_OBJECT_ATTRIBUTES    ctxAttr;
    WDF_OBJECT_ATTRIBUTES    lockAttr;
    PVUSBBUS_CTL_DEV_CONTEXT ctx;
    NTSTATUS                 status;

    PAGED_CODE();

    devInit = WdfControlDeviceInitAllocate(
        WdfDeviceGetDriver(FdoCtx->Fdo),
        &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (devInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /*
     * Override the default SDDL with our stricter, explicit literal.
     */
    status = WdfDeviceInitAssignSDDLString(devInit, &g_CtlDeviceSddl);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(devInit);
        return status;
    }

    status = WdfDeviceInitAssignName(devInit, &g_CtlDeviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(devInit);
        return status;
    }

    WdfDeviceInitSetDeviceType(devInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(devInit,
        FILE_DEVICE_SECURE_OPEN | FILE_CHARACTERISTIC_PNP_DEVICE, TRUE);

    WDF_FILEOBJECT_CONFIG_INIT(&fileCfg,
        WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttr, VUSBBUS_CTL_FILE_CONTEXT_T);
    WdfDeviceInitSetFileObjectConfig(devInit, &fileCfg, &fileAttr);

    {
        WDF_OBJECT_ATTRIBUTES devAttr;
        WDF_OBJECT_ATTRIBUTES_INIT(&devAttr);
        status = WdfDeviceCreate(&devInit, &devAttr, &ctlDevice);
    }
    if (!NT_SUCCESS(status)) {
        if (devInit != NULL) { WdfDeviceInitFree(devInit); }
        return status;
    }

    /*
     * Allocate the control-device context and its backpointer lock
     * before publishing the interface, so the first open already
     * resolves to FdoCtx and the lock exists for every reader.
     */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&ctxAttr, VUSBBUS_CTL_DEV_CONTEXT);
    status = WdfObjectAllocateContext(ctlDevice, &ctxAttr, (PVOID*)&ctx);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(ctlDevice);
        return status;
    }
    ctx->FdoCtx = NULL;

    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttr);
    lockAttr.ParentObject = ctlDevice;
    status = WdfSpinLockCreate(&lockAttr, &ctx->Lock);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(ctlDevice);
        return status;
    }
    ctx->FdoCtx = FdoCtx;

    status = WdfDeviceCreateDeviceInterface(
        ctlDevice, &GUID_DEVINTERFACE_VUSBBUS_CTL, NULL);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(ctlDevice);
        return status;
    }

    status = VusbBusQueueInitialize(ctlDevice);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(ctlDevice);
        return status;
    }

    WdfControlFinishInitializing(ctlDevice);

    g_CtlDevice           = ctlDevice;
    FdoCtx->ControlDevice = ctlDevice;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
VusbBusCtlDetachFdo(
    PVUSBBUS_FDO_CONTEXT FdoCtx
    )
{
    WDFDEVICE                ctlDevice;
    PVUSBBUS_CTL_DEV_CONTEXT ctx;

    if (FdoCtx == NULL) {
        return;
    }
    ctlDevice = FdoCtx->ControlDevice;
    if (ctlDevice == NULL) {
        return;
    }
    ctx = VusbBusCtlDevGetContext(ctlDevice);
    if (ctx == NULL || ctx->Lock == NULL) {
        return;
    }

    WdfSpinLockAcquire(ctx->Lock);
    if (ctx->FdoCtx == FdoCtx) {
        ctx->FdoCtx = NULL;
    }
    WdfSpinLockRelease(ctx->Lock);
}

_Use_decl_annotations_
PVUSBBUS_FDO_CONTEXT
VusbBusCtlAcquireFdo(
    WDFDEVICE ControlDevice
    )
{
    PVUSBBUS_CTL_DEV_CONTEXT ctx = VusbBusCtlDevGetContext(ControlDevice);
    PVUSBBUS_FDO_CONTEXT     fdoCtx = NULL;

    if (ctx == NULL || ctx->Lock == NULL) {
        return NULL;
    }

    WdfSpinLockAcquire(ctx->Lock);
    fdoCtx = ctx->FdoCtx;
    if (fdoCtx != NULL) {
        WdfObjectReference(fdoCtx->Fdo);
    }
    WdfSpinLockRelease(ctx->Lock);
    return fdoCtx;
}

_Use_decl_annotations_
VOID
VusbBusCtlReleaseFdo(
    PVUSBBUS_FDO_CONTEXT FdoCtx
    )
{
    if (FdoCtx != NULL) {
        WdfObjectDereference(FdoCtx->Fdo);
    }
}
