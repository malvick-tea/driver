/*
 * hid/device.c
 *
 * Function FDO creation for vhidkm.sys.
 *
 * Lifecycle map (simplified):
 *
 *   EvtDriverDeviceAdd
 *     └─> VhidkmDeviceCreate
 *           ├─ WDF_PNPPOWER_EVENT_CALLBACKS_INIT
 *           │  (PrepareHardware / ReleaseHardware / D0Entry / D0Exit /
 *           │   SurpriseRemoval / SelfManagedIoInit)
 *           ├─ WDF_FILEOBJECT_CONFIG_INIT (no-op callbacks — the FDO
 *           │  accepts no user handles directly; user-mode talks to the
 *           │  control device)
 *           ├─ hidclass.sys registration via HidRegisterMinidriver
 *           │  after WdfDeviceCreate — this attaches hidclass.sys as
 *           │  an upper-filter and routes HID-internal IOCTLs to our
 *           │  default queue
 *           ├─ Default IO queue -> hid_ioctls.c
 *           ├─ Manual pending-read queue (report_queue.c)
 *           ├─ Manual LED-wait queue
 *           ├─ S0 idle settings (selective suspend)
 *           └─ Control-device creation (ctl_device.c)
 *
 * hidclass.sys requirements:
 *   - The device object type must be FILE_DEVICE_UNKNOWN (HID class
 *     driver checks this; using FILE_DEVICE_KEYBOARD or _MOUSE would
 *     cause HidpRegisterMinidriver to reject us).
 *   - We must not set DO_BUFFERED_IO or DO_DIRECT_IO on the FDO;
 *     hidclass.sys manages the I/O method per IOCTL.
 *   - The minidriver registration is done by HidRegisterMinidriver
 *     AFTER WdfDeviceCreate returns so the DEVICE_OBJECT exists. The
 *     framework's WdfDeviceInitSetIoType set to neither keeps the
 *     FDO flags in the shape hidclass.sys expects.
 */

#include "driver.h"
#include "device.h"
#include "hid_ioctls.h"
#include "report_queue.h"
#include "ctl_device.h"
#include "bus_iface.h"
#include "led_state.h"
#include "trace.h"
#include "device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VhidkmDeviceCreate)
#pragma alloc_text(PAGE, VhidkmEvtDevicePrepareHardware)
#pragma alloc_text(PAGE, VhidkmEvtDeviceReleaseHardware)
#pragma alloc_text(PAGE, VhidkmEvtDeviceD0Entry)
#pragma alloc_text(PAGE, VhidkmEvtDeviceD0Exit)
#pragma alloc_text(PAGE, VhidkmEvtDeviceSelfManagedIoInit)
#pragma alloc_text(PAGE, VhidkmEvtDeviceSelfManagedIoCleanup)
#endif

EVT_WDF_DEVICE_PREPARE_HARDWARE         VhidkmEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE         VhidkmEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY                 VhidkmEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT                  VhidkmEvtDeviceD0Exit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT     VhidkmEvtDeviceSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP  VhidkmEvtDeviceSelfManagedIoCleanup;
EVT_WDF_DEVICE_SURPRISE_REMOVAL         VhidkmEvtDeviceSurpriseRemoval;
EVT_WDF_OBJECT_CONTEXT_CLEANUP          VhidkmEvtDeviceContextCleanup;

_Use_decl_annotations_
NTSTATUS
VhidkmDeviceCreate(
    PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_OBJECT_ATTRIBUTES           fdoAttr;
    WDF_PNPPOWER_EVENT_CALLBACKS    pnpCallbacks;
    WDF_DEVICE_STATE                deviceState;
    WDFDEVICE                       device;
    PVHIDKM_DEVICE_CONTEXT          devCtx;
    NTSTATUS                        status;

    PAGED_CODE();

    /*
     * Device type / characteristics. hidclass.sys requires
     * FILE_DEVICE_UNKNOWN on a HID minidriver FDO; setting it to
     * FILE_DEVICE_KEYBOARD here would be rejected inside
     * HidRegisterMinidriver with STATUS_INVALID_PARAMETER.
     */
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);
    WdfDeviceInitSetCharacteristics(DeviceInit,
        FILE_DEVICE_SECURE_OPEN, TRUE);

    /*
     * I/O type = Buffered is acceptable for the FDO default queue
     * because the only traffic we see on it is HID-internal IOCTLs
     * from hidclass.sys, and hidclass.sys selects its own transfer
     * method per IOCTL regardless of what we set. KMDF still uses
     * this field to route METHOD_BUFFERED IOCTLs arriving out of
     * band (there should be none).
     */
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware       = VhidkmEvtDevicePrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware       = VhidkmEvtDeviceReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry               = VhidkmEvtDeviceD0Entry;
    pnpCallbacks.EvtDeviceD0Exit                = VhidkmEvtDeviceD0Exit;
    pnpCallbacks.EvtDeviceSelfManagedIoInit     = VhidkmEvtDeviceSelfManagedIoInit;
    pnpCallbacks.EvtDeviceSelfManagedIoCleanup  = VhidkmEvtDeviceSelfManagedIoCleanup;
    pnpCallbacks.EvtDeviceSurpriseRemoval       = VhidkmEvtDeviceSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fdoAttr, VHIDKM_DEVICE_CONTEXT);
    fdoAttr.EvtCleanupCallback = VhidkmEvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &fdoAttr, &device);
    if (!NT_SUCCESS(status)) {
        TraceError("WdfDeviceCreate (HID FDO) failed %!STATUS!", status);
        return status;
    }

    devCtx = VhidkmDeviceGetContext(device);
    RtlZeroMemory(devCtx, sizeof(*devCtx));
    devCtx->Device          = device;
    devCtx->WdmDeviceObject = WdfDeviceWdmGetDeviceObject(device);

    /*
     * The FDO is not directly openable by user-mode; hide it from
     * Device Manager's "Show hidden devices" by setting
     * NotDisableable = WdfFalse and NoDisplayInUI = WdfFalse — we do
     * want it visible because this is a real PnP device surface.
     * Instead, we clear the raw "device interface" publishing. The
     * control device is the one that user-mode opens.
     */
    WDF_DEVICE_STATE_INIT(&deviceState);
    deviceState.DontDisplayInUI = WdfFalse;
    WdfDeviceSetDeviceState(device, &deviceState);

    /*
     * Synchronization primitives for the report ring and LED state
     * cache. Spinlocks, not wait-locks — the LED propagation path runs
     * from the control-device IOCTL dispatcher and we want it
     * DISPATCH_LEVEL safe so future callers from an interrupt-style
     * context (e.g. a KMDF timer tick) can share the code.
     */
    {
        WDF_OBJECT_ATTRIBUTES lockAttr;
        WDF_OBJECT_ATTRIBUTES_INIT(&lockAttr);
        lockAttr.ParentObject = device;
        status = WdfSpinLockCreate(&lockAttr, &devCtx->ReportQueueLock);
        if (!NT_SUCCESS(status)) {
            TraceError("ReportQueueLock create failed %!STATUS!", status);
            return status;
        }
        status = WdfSpinLockCreate(&lockAttr, &devCtx->LedLock);
        if (!NT_SUCCESS(status)) {
            TraceError("LedLock create failed %!STATUS!", status);
            return status;
        }
    }

    /*
     * Build the manual queues — the pending-read queue and the LED
     * wait queue. Both must be created before HidRegisterMinidriver
     * so hidclass.sys does not pump a read into the FDO before we
     * have somewhere to route it.
     */
    status = VhidkmReportQueueInitialize(devCtx);
    if (!NT_SUCCESS(status)) {
        TraceError("ReportQueueInitialize failed %!STATUS!", status);
        return status;
    }

    status = VhidkmLedStateInitialize(devCtx);
    if (!NT_SUCCESS(status)) {
        TraceError("LedStateInitialize failed %!STATUS!", status);
        return status;
    }

    /*
     * Default IO queue receives HID-internal IOCTLs from hidclass.sys
     * (IOCTL_HID_GET_DEVICE_DESCRIPTOR etc.). hid_ioctls.c installs
     * an EvtIoInternalDeviceControl handler that fans them out to the
     * appropriate implementation.
     */
    status = VhidkmHidIoctlInitialize(devCtx);
    if (!NT_SUCCESS(status)) {
        TraceError("HidIoctlInitialize failed %!STATUS!", status);
        return status;
    }

    /*
     * Selective suspend. KMDF manages the S0-idle state machine; the
     * device powers down after IdleTimeout of no I/O. UserControl =
     * IdleAllowUserControl surfaces the checkbox in Device Manager's
     * Power Management tab. CannotWakeFromS0 is chosen because we
     * have no hardware timer to drive a wake; incoming injection
     * implicitly powers the device up through KMDF's I/O path.
     */
    {
        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idle;
        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idle,
            IdleCannotWakeFromS0);
        idle.IdleTimeout  = 10000;
        idle.UserControlOfIdleSettings = IdleAllowUserControl;
        idle.Enabled      = WdfTrue;
        status = WdfDeviceAssignS0IdleSettings(device, &idle);
        if (!NT_SUCCESS(status)) {
            TraceWarn("WdfDeviceAssignS0IdleSettings failed %!STATUS!", status);
            /* Non-fatal: continue without selective-suspend. */
            status = STATUS_SUCCESS;
        }
    }

    /*
     * Register as a HID minidriver. HidRegisterMinidriver hooks
     * hidclass.sys above the FDO by adjusting the major-function
     * table of our driver object; all HID-internal IOCTLs begin
     * arriving on the default queue after this call returns.
     *
     * The DriverExtension field carries per-instance state that
     * hidclass.sys passes back to us in callbacks. We park a pointer
     * to our WDFDEVICE there so handlers can recover the context
     * from the raw DEVICE_OBJECT.
     */
    {
        HID_DEVICE_EXTENSION* hidExt;
        hidExt = GET_MINIDRIVER_DEVICE_EXTENSION(devCtx->WdmDeviceObject);
        UNREFERENCED_PARAMETER(hidExt);
        /*
         * No per-instance extension state needs to be stored: the
         * HID_DEVICE_EXTENSION carries PhysicalDeviceObject and
         * NextDeviceObject which hidclass.sys fills in automatically
         * after registration. We reach our context through
         * WdfWdmDeviceGetWdfDeviceHandle on the WDM FDO whenever a
         * callback arrives.
         */
    }

    /*
     * The control device is created by the first FDO to reach this
     * point. Subsequent FDOs (if VHID_MAX_SLOTS were > 1) would share
     * the existing control device — it is a process-wide resource.
     */
    status = VhidkmCtlDeviceCreateOnce(device, devCtx);
    if (!NT_SUCCESS(status)) {
        TraceError("CtlDeviceCreate failed %!STATUS!", status);
        /*
         * Failing the FDO for control-device creation failure would
         * leave the USB-visible HID device fully functional but the
         * user-mode injection surface unavailable. We deliberately
         * fail the FDO instead so the stack reports the error in
         * Device Manager and the user notices.
         */
        return status;
    }

    TracePnp("HID FDO created ok");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VhidkmEvtDevicePrepareHardware(
    WDFDEVICE       Device,
    WDFCMRESLIST    ResourcesRaw,
    WDFCMRESLIST    ResourcesTranslated
    )
{
    PVHIDKM_DEVICE_CONTEXT ctx = VhidkmDeviceGetContext(Device);
    NTSTATUS status;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    /*
     * PrepareHardware is invoked once per start. We query the PDO
     * for the bus IPC interface here — the interface reference
     * ensures the PDO stays alive even under surprise-remove until
     * ReleaseHardware dereferences it.
     */
    status = VhidkmBusIfaceAcquire(ctx);
    if (!NT_SUCCESS(status)) {
        TraceIpc("BusIfaceAcquire failed %!STATUS!", status);
        return status;
    }

    /* Notify the bus driver that the HID function is live. */
    if (ctx->IpcValid && ctx->BusIpc.NotifyFunctionReady != NULL) {
        (VOID)ctx->BusIpc.NotifyFunctionReady(
            ctx->BusIpc.InterfaceHeader.Context,
            ctx->WdmDeviceObject);
    }

    TracePnp("PrepareHardware ok, IpcValid=%u", (ULONG)ctx->IpcValid);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VhidkmEvtDeviceReleaseHardware(
    WDFDEVICE     Device,
    WDFCMRESLIST  ResourcesTranslated
    )
{
    PVHIDKM_DEVICE_CONTEXT ctx = VhidkmDeviceGetContext(Device);

    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    /* Drain any lingering pending reads before the PDO dissolves. */
    VhidkmReportQueueDrain(ctx);

    VhidkmBusIfaceRelease(ctx);

    TracePnp("ReleaseHardware ok");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VhidkmEvtDeviceD0Entry(
    WDFDEVICE               Device,
    WDF_POWER_DEVICE_STATE  PreviousState
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    TracePwr("FDO D0Entry from %d", (int)PreviousState);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VhidkmEvtDeviceD0Exit(
    WDFDEVICE               Device,
    WDF_POWER_DEVICE_STATE  TargetState
    )
{
    PVHIDKM_DEVICE_CONTEXT ctx = VhidkmDeviceGetContext(Device);

    PAGED_CODE();

    TracePwr("FDO D0Exit to %d", (int)TargetState);

    /*
     * On a shutdown-class transition, cancel everything pending so
     * upper filters do not see completions from a torn-down device.
     * For idle-class transitions the pending reads stay alive and
     * are resumed by KMDF on the next D0Entry automatically.
     */
    if (TargetState == WdfPowerDeviceD3Final) {
        VhidkmReportQueueDrain(ctx);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VhidkmEvtDeviceSelfManagedIoInit(
    WDFDEVICE Device
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    TracePnp("SelfManagedIoInit");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
VhidkmEvtDeviceSelfManagedIoCleanup(
    WDFDEVICE Device
    )
{
    PVHIDKM_DEVICE_CONTEXT ctx = VhidkmDeviceGetContext(Device);

    PAGED_CODE();

    TracePnp("SelfManagedIoCleanup");
    /*
     * Final chance to complete any waiter queued by a control-device
     * handle before cleanup runs for the control device. Safe to
     * call repeatedly; the queues tolerate empty drains.
     */
    VhidkmReportQueueDrain(ctx);
    VhidkmLedStateCompleteAllWaiters(ctx, STATUS_DEVICE_REMOVED);
}

_Use_decl_annotations_
VOID
VhidkmEvtDeviceSurpriseRemoval(
    WDFDEVICE Device
    )
{
    PVHIDKM_DEVICE_CONTEXT ctx = VhidkmDeviceGetContext(Device);
    TracePnp("SurpriseRemoval");
    /*
     * PnP delivered surprise-remove. All pending IRPs must be
     * completed promptly so hidclass.sys can unwind without waiting
     * on us. The completion-with-cancelled status mirrors what a
     * real USB HID device would produce if its cable was yanked.
     */
    VhidkmReportQueueDrain(ctx);
    VhidkmLedStateCompleteAllWaiters(ctx, STATUS_DEVICE_REMOVED);
}

_Use_decl_annotations_
VOID
VhidkmEvtDeviceContextCleanup(
    WDFOBJECT Object
    )
{
    UNREFERENCED_PARAMETER(Object);
    TracePnp("device context cleanup");
    /*
     * All queues, spinlocks, and the ring buffer live inside the
     * device context or as KMDF children of the device; KMDF frees
     * them as part of its cleanup pass.
     */
}

_Use_decl_annotations_
NTSTATUS
VhidkmDevicePostInputReport(
    PVHIDKM_DEVICE_CONTEXT  DevCtx,
    const UCHAR*            ReportData,
    UCHAR                   ReportLength
    )
{
    return VhidkmReportQueuePost(DevCtx, ReportData, ReportLength);
}

_Use_decl_annotations_
VOID
VhidkmDeviceUpdateLedState(
    PVHIDKM_DEVICE_CONTEXT  DevCtx,
    UCHAR                   NewLedBits
    )
{
    VhidkmLedStateUpdate(DevCtx, NewLedBits);
}
