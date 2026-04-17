/*
 * bus/ctl_device.c
 *
 * Control device factory for vusbbus.sys.
 *
 * The control device is a standalone WDFDEVICE that the bus FDO
 * creates in its EvtDeviceAdd path. It:
 *
 *   - Declares a device-interface GUID (GUID_DEVINTERFACE_VUSBBUS_CTL)
 *     so user-mode can enumerate it with SetupDiGetClassDevs.
 *   - Applies an SDDL string restricting open to SYSTEM and the
 *     Administrators group.
 *   - Hosts a default IO queue that routes IOCTLs to queue.c.
 *
 * A KMDF "control device" is technically a standalone WDFDEVICE
 * with ControlDeviceInit; it is not parented to the FDO because the
 * framework requires it to live as a sibling of the driver, not as
 * a child of any PnP device. We keep a handle to it on the FDO
 * context so the IOCTL dispatcher in queue.c can route back.
 */

#include "driver.h"
#include "ctl_device.h"
#include "queue.h"
#include "trace.h"
#include "ctl_device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VusbBusCtlDeviceCreate)
#endif

/*
 * Name of the control device under \Device. Not exposed to user-
 * mode code (that uses the interface GUID); kept stable for
 * debugger-side discovery.
 */
DECLARE_CONST_UNICODE_STRING(g_CtlDeviceName, L"\\Device\\VUsbBusCtl");

/* SDDL: SYSTEM and Administrators get full access; all others denied. */
DECLARE_CONST_UNICODE_STRING(g_CtlDeviceSddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

_Use_decl_annotations_
NTSTATUS
VusbBusCtlDeviceCreate(
    PVUSBBUS_FDO_CONTEXT FdoCtx
    )
{
    PWDFDEVICE_INIT         devInit  = NULL;
    WDFDEVICE               ctlDevice = NULL;
    WDF_OBJECT_ATTRIBUTES   fileAttr;
    WDF_FILEOBJECT_CONFIG   fileCfg;
    NTSTATUS                status;

    PAGED_CODE();

    devInit = WdfControlDeviceInitAllocate(
        WdfDeviceGetDriver(FdoCtx->Fdo),
        &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (devInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /*
     * Override the default SDDL with our stricter one. The
     * SDDL_DEVOBJ_SYS_ALL_ADM_ALL alias above is semantically
     * equivalent but explicit literals are easier to audit.
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

    /* Publish the device interface for user-mode discovery. */
    status = WdfDeviceCreateDeviceInterface(
        ctlDevice,
        &GUID_DEVINTERFACE_VUSBBUS_CTL,
        NULL);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(ctlDevice);
        return status;
    }

    /* Bind the bus FDO context to the control device via a property
     * table. Rather than allocating a dedicated WDF context on the
     * control device, we stash the pointer in the device info so the
     * IOCTL dispatcher can retrieve it without a context lookup. */
    status = VusbBusQueueInitialize(ctlDevice, FdoCtx);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(ctlDevice);
        return status;
    }

    /*
     * Control-device creation is a two-phase operation: the init
     * structures go through WdfDeviceCreate, and then KMDF requires
     * an explicit WdfControlFinishInitializing call to open the
     * device for I/O. Skipping this call leaves the device in a
     * state where CreateFile can open it but the IOCTL path will
     * return STATUS_INVALID_DEVICE_STATE.
     */
    WdfControlFinishInitializing(ctlDevice);

    FdoCtx->ControlDevice = ctlDevice;

    TracePnp("control device created and interface published");
    return STATUS_SUCCESS;
}
