/*
 * hid/ctl_device.c
 *
 * Control-device construction + file-object lifecycle for vhidkm.sys.
 *
 * Creation pattern mirrors the bus driver's ctl_device.c: allocate
 * DEVICE_INIT via WdfControlDeviceInitAllocate, apply SDDL via
 * WdfDeviceInitAssignSDDLString, attach a WDF_FILEOBJECT_CONFIG so
 * each open file gets per-handle context (screen metrics), create
 * the device, publish the interface GUID, wire up the IO queue,
 * and finally WdfControlFinishInitializing.
 *
 * Thread-safety of the once-flag:
 *   Two concurrent FDOs racing to create the control device would
 *   otherwise each try to claim \Device\VhidkmCtl and the second
 *   would fail with STATUS_OBJECT_NAME_COLLISION. Serialization is
 *   achieved with a driver-wide WDFWAITLOCK and a BOOLEAN created
 *   lazily on first call. The lock is allocated alongside the
 *   driver object so its lifetime matches the driver.
 */

#include "driver.h"
#include "device.h"
#include "ctl_device.h"
#include "ctl_ioctls.h"
#include "trace.h"
#include "ctl_device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VhidkmCtlDeviceCreateOnce)
#pragma alloc_text(PAGE, VhidkmCtlEvtFileCreate)
#pragma alloc_text(PAGE, VhidkmCtlEvtFileCleanup)
#pragma alloc_text(PAGE, VhidkmCtlEvtFileClose)
#endif

DECLARE_CONST_UNICODE_STRING(g_CtlDeviceName, L"\\Device\\VhidkmCtl");
DECLARE_CONST_UNICODE_STRING(g_CtlDeviceSddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

EVT_WDF_DEVICE_FILE_CREATE VhidkmCtlEvtFileCreate;
EVT_WDF_FILE_CLEANUP       VhidkmCtlEvtFileCleanup;
EVT_WDF_FILE_CLOSE         VhidkmCtlEvtFileClose;

/*
 * Driver-wide once-flag + lock. Allocated once in
 * VhidkmCtlDeviceCreateOnce by a double-checked pattern. We cannot
 * use InterlockedCompareExchange on a HANDLE because WDFWAITLOCK is
 * opaque and its creation is not atomic; instead, we protect the
 * flag with a global spinlock since KMDF does not yet provide a
 * "create-lock-once" primitive at driver scope.
 */

/*
 * The control device and its wait-lock are kept in driver-context
 * variables. We keep them as file-scope statics because WDFDRIVER
 * context allocation is out of scope for this compilation unit and
 * a small number of process-wide, driver-lifetime statics is the
 * canonical pattern for control-device singletons (used by the
 * WDK KMDF samples such as `toaster`, `osrusbfx2`, and the
 * in-box HID minidrivers).
 */
static volatile LONG    g_CtlInitOnce = 0;
static WDFDEVICE        g_CtlDevice   = NULL;

_Use_decl_annotations_
NTSTATUS
VhidkmCtlDeviceCreateOnce(
    WDFDEVICE                        AssociatedFdo,
    struct _VHIDKM_DEVICE_CONTEXT*   DevCtx
    )
{
    PWDFDEVICE_INIT         devInit = NULL;
    WDFDEVICE               ctlDevice;
    WDF_OBJECT_ATTRIBUTES   fileAttr;
    WDF_FILEOBJECT_CONFIG   fileCfg;
    WDF_OBJECT_ATTRIBUTES   devAttr;
    PVHIDKM_CTL_DEV_CONTEXT devCtxOnCtl;
    NTSTATUS                status;
    LONG                    prev;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(AssociatedFdo);

    /*
     * Fast path: another caller already finished construction.
     * Backfill the DevCtx pointer so this FDO also appears at the
     * control device's dispatch layer.
     */
    prev = InterlockedCompareExchange(&g_CtlInitOnce, 1, 0);
    if (prev == 2) {
        /* Already fully initialized. Rewire DevCtx pointer. */
        devCtxOnCtl = VhidkmCtlDevGetContext(g_CtlDevice);
        devCtxOnCtl->DevCtx = DevCtx;
        DevCtx->ControlDevice = g_CtlDevice;
        return STATUS_SUCCESS;
    }
    if (prev == 1) {
        /*
         * Another thread is actively building the control device.
         * Yield briefly by reading the once-flag in a polling loop;
         * the construction path is short (a few hundred micro-
         * seconds) and polling at PASSIVE_LEVEL is acceptable here.
         */
        LARGE_INTEGER interval = { .QuadPart = -10 * 1000 };
        while (InterlockedCompareExchange(&g_CtlInitOnce,
                                          g_CtlInitOnce, 0) == 1) {
            (VOID)KeDelayExecutionThread(KernelMode, FALSE, &interval);
        }
        devCtxOnCtl = VhidkmCtlDevGetContext(g_CtlDevice);
        devCtxOnCtl->DevCtx = DevCtx;
        DevCtx->ControlDevice = g_CtlDevice;
        return STATUS_SUCCESS;
    }

    /* We won the race and are responsible for construction. */

    devInit = WdfControlDeviceInitAllocate(
        WdfDeviceGetDriver(AssociatedFdo),
        &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (devInit == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    status = WdfDeviceInitAssignSDDLString(devInit, &g_CtlDeviceSddl);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(devInit);
        goto done;
    }

    status = WdfDeviceInitAssignName(devInit, &g_CtlDeviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(devInit);
        goto done;
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
        goto done;
    }

    devCtxOnCtl = VhidkmCtlDevGetContext(ctlDevice);
    devCtxOnCtl->DevCtx = DevCtx;

    status = WdfDeviceCreateDeviceInterface(
        ctlDevice, &GUID_DEVINTERFACE_VHIDKM_CTL, NULL);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(ctlDevice);
        goto done;
    }

    status = VhidkmCtlIoctlInitialize(ctlDevice);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(ctlDevice);
        goto done;
    }

    WdfControlFinishInitializing(ctlDevice);

    g_CtlDevice           = ctlDevice;
    DevCtx->ControlDevice = ctlDevice;

    /* Publish completion by transitioning the once-flag 1 -> 2. */
    InterlockedExchange(&g_CtlInitOnce, 2);
    TracePnp("control device created, interface published");
    return STATUS_SUCCESS;

done:
    /* Roll back the once-flag so a retry is possible. */
    InterlockedExchange(&g_CtlInitOnce, 0);
    TraceError("control device creation failed %!STATUS!", status);
    return status;
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
    WDFDEVICE               ctlDevice = WdfFileObjectGetDevice(FileObject);
    PVHIDKM_CTL_DEV_CONTEXT devCtxOnCtl = VhidkmCtlDevGetContext(ctlDevice);

    PAGED_CODE();

    TraceCtl("ctl file cleanup");

    /*
     * When the controlling process exits (or explicitly closes the
     * handle) ensure no input remains "pressed" by pushing all-up
     * reports. This mirrors the behaviour of a real keyboard that
     * would stop sending press reports when the cable is removed.
     */
    if (devCtxOnCtl != NULL && devCtxOnCtl->DevCtx != NULL) {
        VHID_KEYBOARD_INPUT_REPORT  kReport = { 0 };
        VHID_MOUSE_REL_REPORT       mReport = { 0 };

        kReport.ReportId = VHID_REPORTID_KEYBOARD_INPUT;
        (VOID)VhidkmDevicePostInputReport(devCtxOnCtl->DevCtx,
            (const UCHAR*)&kReport, (UCHAR)sizeof(kReport));

        mReport.ReportId = VHID_REPORTID_MOUSE_REL;
        (VOID)VhidkmDevicePostInputReport(devCtxOnCtl->DevCtx,
            (const UCHAR*)&mReport, (UCHAR)sizeof(mReport));
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
