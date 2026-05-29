/*
 * hid/ctl_device.c
 *
 * Control-device construction + file-object lifecycle for vhidkm.sys.
 *
 * The control device is a standalone WDFDEVICE (not tied to the PnP
 * tree) through which user-mode issues IOCTL_VHIDKM_* -- input
 * injection, LED readback, screen metrics. It carries an SDDL string
 * restricting open to SYSTEM and BUILTIN\Administrators and publishes
 * GUID_DEVINTERFACE_VHIDKM_CTL.
 *
 * Lifetime model:
 *   The control device is a driver-wide singleton created lazily by the
 *   first FDO to reach EvtDeviceAdd and torn down only when the driver
 *   unloads. Each FDO that comes up rebinds the control device's
 *   backpointer to its own context; each FDO that tears down clears that
 *   backpointer (VhidkmCtlDetachDevice) so a request arriving after the
 *   FDO is gone cannot dereference a freed context.
 *
 *   Because the control device can outlive the FDO whose context it
 *   points at, every reader resolves the backpointer under
 *   VHIDKM_CTL_DEV_CONTEXT.Lock and takes a reference on the FDO's
 *   WDFDEVICE for the duration of its work (VhidkmCtlAcquireDevice /
 *   VhidkmCtlReleaseDevice). Holding that reference keeps the FDO
 *   context and its child queues allocated even if a concurrent unplug
 *   removes the device mid-IOCTL.
 *
 * Initialization race:
 *   Construction is elected with a four-state flag (uninit / building /
 *   ready / failed). The single winning thread builds; losers either
 *   attach to the finished device (ready) or surface the builder's
 *   failure (failed) -- they never read g_CtlDevice while it is still
 *   NULL.
 */

#include "driver.h"
#include "device.h"
#include "ctl_device.h"
#include "ctl_ioctls.h"
#include "trace.h"
#include "ctl_device.tmh"

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS VhidkmCtlBuildAndPublish(
    _In_ WDFDEVICE AssociatedFdo,
    _In_ struct _VHIDKM_DEVICE_CONTEXT* DevCtx);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VhidkmCtlDeviceCreateOnce)
#pragma alloc_text(PAGE, VhidkmCtlBuildAndPublish)
#pragma alloc_text(PAGE, VhidkmCtlEvtFileCreate)
#pragma alloc_text(PAGE, VhidkmCtlEvtFileCleanup)
#pragma alloc_text(PAGE, VhidkmCtlEvtFileClose)
#endif

DECLARE_CONST_UNICODE_STRING(g_CtlDeviceName, L"\\Device\\VhidkmCtl");
DECLARE_CONST_UNICODE_STRING(g_CtlDeviceSddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

EVT_WDF_DEVICE_FILE_CREATE VhidkmCtlEvtFileCreate;
EVT_WDF_FILE_CLEANUP       VhidkmCtlEvtFileCleanup;
EVT_WDF_FILE_CLOSE         VhidkmCtlEvtFileClose;

/* Construction-election states for g_CtlInitState. */
#define CTL_STATE_UNINIT    0
#define CTL_STATE_BUILDING  1
#define CTL_STATE_READY     2
#define CTL_STATE_FAILED    3

/*
 * Driver-lifetime singleton state. g_CtlDevice is published only after
 * g_CtlInitState transitions to CTL_STATE_READY; readers must observe
 * READY before touching g_CtlDevice.
 */
static volatile LONG    g_CtlInitState = CTL_STATE_UNINIT;
static WDFDEVICE        g_CtlDevice    = NULL;

/*
 * Atomic read of the init state that never mutates a non-zero value.
 * (CompareExchange of UNINIT->UNINIT is a no-op write when the value is
 * already UNINIT and leaves any other value untouched.)
 */
static LONG VhidkmCtlReadState(void)
{
    return InterlockedCompareExchange(&g_CtlInitState,
                                      CTL_STATE_UNINIT, CTL_STATE_UNINIT);
}

/* Rebind the singleton control device to a (re)started FDO. */
_IRQL_requires_max_(DISPATCH_LEVEL)
static NTSTATUS VhidkmCtlAttachDevice(_In_ struct _VHIDKM_DEVICE_CONTEXT* DevCtx)
{
    PVHIDKM_CTL_DEV_CONTEXT devCtxOnCtl = VhidkmCtlDevGetContext(g_CtlDevice);

    WdfSpinLockAcquire(devCtxOnCtl->Lock);
    devCtxOnCtl->DevCtx = DevCtx;
    WdfSpinLockRelease(devCtxOnCtl->Lock);

    DevCtx->ControlDevice = g_CtlDevice;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VhidkmCtlDeviceCreateOnce(
    WDFDEVICE                        AssociatedFdo,
    struct _VHIDKM_DEVICE_CONTEXT*   DevCtx
    )
{
    PAGED_CODE();

    for (;;) {
        LONG prev = InterlockedCompareExchange(&g_CtlInitState,
                                               CTL_STATE_BUILDING,
                                               CTL_STATE_UNINIT);

        if (prev == CTL_STATE_FAILED) {
            /*
             * A previous attempt failed. Try to claim a fresh build; if
             * we win, proceed as the builder, otherwise fall through and
             * wait for whoever did.
             */
            if (InterlockedCompareExchange(&g_CtlInitState,
                                           CTL_STATE_BUILDING,
                                           CTL_STATE_FAILED) == CTL_STATE_FAILED) {
                prev = CTL_STATE_UNINIT;
            } else {
                prev = CTL_STATE_BUILDING;
            }
        }

        if (prev == CTL_STATE_UNINIT) {
            NTSTATUS status = VhidkmCtlBuildAndPublish(AssociatedFdo, DevCtx);
            InterlockedExchange(&g_CtlInitState,
                                NT_SUCCESS(status) ? CTL_STATE_READY
                                                   : CTL_STATE_FAILED);
            if (NT_SUCCESS(status)) {
                TracePnp("control device created, interface published");
            } else {
                TraceError("control device creation failed %!STATUS!", status);
            }
            return status;
        }

        if (prev == CTL_STATE_READY) {
            return VhidkmCtlAttachDevice(DevCtx);
        }

        /*
         * prev == CTL_STATE_BUILDING: another thread is constructing the
         * device. Poll until it settles, then loop to act on the result.
         * Construction is short and this path is only reachable when more
         * than one FDO races (VHID_MAX_SLOTS > 1).
         */
        {
            LARGE_INTEGER interval;
            interval.QuadPart = -10 * 1000; /* 1 ms, relative */
            while (VhidkmCtlReadState() == CTL_STATE_BUILDING) {
                (VOID)KeDelayExecutionThread(KernelMode, FALSE, &interval);
            }
        }
    }
}

_Use_decl_annotations_
static NTSTATUS
VhidkmCtlBuildAndPublish(
    WDFDEVICE                        AssociatedFdo,
    struct _VHIDKM_DEVICE_CONTEXT*   DevCtx
    )
{
    PWDFDEVICE_INIT         devInit = NULL;
    WDFDEVICE               ctlDevice = NULL;
    WDF_OBJECT_ATTRIBUTES   fileAttr;
    WDF_FILEOBJECT_CONFIG   fileCfg;
    WDF_OBJECT_ATTRIBUTES   devAttr;
    WDF_OBJECT_ATTRIBUTES   lockAttr;
    PVHIDKM_CTL_DEV_CONTEXT devCtxOnCtl;
    NTSTATUS                status;

    PAGED_CODE();

    devInit = WdfControlDeviceInitAllocate(
        WdfDeviceGetDriver(AssociatedFdo),
        &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (devInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

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

    /* Per-handle file object context. */
    WDF_FILEOBJECT_CONFIG_INIT(&fileCfg,
        VhidkmCtlEvtFileCreate,
        VhidkmCtlEvtFileClose,
        VhidkmCtlEvtFileCleanup);
    fileCfg.FileObjectClass = WdfFileObjectWdfCanUseFsContext;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttr, VHIDKM_CTL_FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(devInit, &fileCfg, &fileAttr);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttr, VHIDKM_CTL_DEV_CONTEXT);
    status = WdfDeviceCreate(&devInit, &devAttr, &ctlDevice);
    if (!NT_SUCCESS(status)) {
        if (devInit != NULL) { WdfDeviceInitFree(devInit); }
        return status;
    }

    devCtxOnCtl = VhidkmCtlDevGetContext(ctlDevice);
    devCtxOnCtl->DevCtx = NULL;

    /* Lock guarding the DevCtx backpointer for the lifetime of the device. */
    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttr);
    lockAttr.ParentObject = ctlDevice;
    status = WdfSpinLockCreate(&lockAttr, &devCtxOnCtl->Lock);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(ctlDevice);
        return status;
    }

    /*
     * Bind before publishing the interface and finishing initialization
     * so the very first open already resolves to this FDO. No reader can
     * race us here: the device interface is not yet enabled.
     */
    devCtxOnCtl->DevCtx = DevCtx;

    status = WdfDeviceCreateDeviceInterface(
        ctlDevice, &GUID_DEVINTERFACE_VHIDKM_CTL, NULL);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(ctlDevice);
        return status;
    }

    status = VhidkmCtlIoctlInitialize(ctlDevice);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(ctlDevice);
        return status;
    }

    WdfControlFinishInitializing(ctlDevice);

    g_CtlDevice           = ctlDevice;
    DevCtx->ControlDevice = ctlDevice;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
VhidkmCtlDetachDevice(
    struct _VHIDKM_DEVICE_CONTEXT* DevCtx
    )
{
    WDFDEVICE               ctlDevice;
    PVHIDKM_CTL_DEV_CONTEXT devCtxOnCtl;

    if (DevCtx == NULL) {
        return;
    }
    ctlDevice = DevCtx->ControlDevice;
    if (ctlDevice == NULL) {
        return;
    }
    devCtxOnCtl = VhidkmCtlDevGetContext(ctlDevice);
    if (devCtxOnCtl == NULL || devCtxOnCtl->Lock == NULL) {
        return;
    }

    WdfSpinLockAcquire(devCtxOnCtl->Lock);
    if (devCtxOnCtl->DevCtx == DevCtx) {
        devCtxOnCtl->DevCtx = NULL;
    }
    WdfSpinLockRelease(devCtxOnCtl->Lock);
}

_Use_decl_annotations_
struct _VHIDKM_DEVICE_CONTEXT*
VhidkmCtlAcquireDevice(
    WDFDEVICE ControlDevice
    )
{
    PVHIDKM_CTL_DEV_CONTEXT        devCtxOnCtl = VhidkmCtlDevGetContext(ControlDevice);
    struct _VHIDKM_DEVICE_CONTEXT* dev = NULL;

    if (devCtxOnCtl == NULL || devCtxOnCtl->Lock == NULL) {
        return NULL;
    }

    WdfSpinLockAcquire(devCtxOnCtl->Lock);
    dev = devCtxOnCtl->DevCtx;
    if (dev != NULL) {
        /*
         * Keep the FDO (and therefore its context and child queues)
         * alive until VhidkmCtlReleaseDevice, even if an unplug removes
         * the device while this request is in flight.
         */
        WdfObjectReference(dev->Device);
    }
    WdfSpinLockRelease(devCtxOnCtl->Lock);
    return dev;
}

_Use_decl_annotations_
VOID
VhidkmCtlReleaseDevice(
    struct _VHIDKM_DEVICE_CONTEXT* DevCtx
    )
{
    if (DevCtx != NULL) {
        WdfObjectDereference(DevCtx->Device);
    }
}

_Use_decl_annotations_
VOID
VhidkmCtlEvtFileCreate(
    WDFDEVICE       Device,
    WDFREQUEST      Request,
    WDFFILEOBJECT   FileObject
    )
{
    PVHIDKM_CTL_FILE_CONTEXT fileCtx = VhidkmCtlFileGetContext(FileObject);

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);

    RtlZeroMemory(fileCtx, sizeof(*fileCtx));
    /* Screen metrics remain unset until the caller sends the IOCTL. */

    TraceCtl("ctl file create");
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID
VhidkmCtlEvtFileCleanup(
    WDFFILEOBJECT FileObject
    )
{
    WDFDEVICE                      ctlDevice = WdfFileObjectGetDevice(FileObject);
    struct _VHIDKM_DEVICE_CONTEXT* dev;

    PAGED_CODE();

    TraceCtl("ctl file cleanup");

    /*
     * When the controlling process exits (or explicitly closes the
     * handle) ensure no input remains "pressed" by pushing all-up
     * reports. Resolve the FDO through the reference-guarded accessor so
     * a concurrent unplug cannot free the context underneath us.
     */
    dev = VhidkmCtlAcquireDevice(ctlDevice);
    if (dev != NULL) {
        VHID_KEYBOARD_INPUT_REPORT  kReport = { 0 };
        VHID_MOUSE_REL_REPORT       mReport = { 0 };

        kReport.ReportId = VHID_REPORTID_KEYBOARD_INPUT;
        (VOID)VhidkmDevicePostInputReport(dev,
            (const UCHAR*)&kReport, (UCHAR)sizeof(kReport));

        mReport.ReportId = VHID_REPORTID_MOUSE_REL;
        (VOID)VhidkmDevicePostInputReport(dev,
            (const UCHAR*)&mReport, (UCHAR)sizeof(mReport));

        VhidkmCtlReleaseDevice(dev);
    }
}

_Use_decl_annotations_
VOID
VhidkmCtlEvtFileClose(
    WDFFILEOBJECT FileObject
    )
{
    UNREFERENCED_PARAMETER(FileObject);
    PAGED_CODE();
    TraceCtl("ctl file close");
}
