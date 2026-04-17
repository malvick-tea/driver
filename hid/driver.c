/*
 * hid/driver.c
 *
 * DriverEntry and EvtDriverDeviceAdd for vhidkm.sys.
 *
 * This is a HID minidriver, so:
 *
 *   - WdfDriverCreate declares us with the regular KMDF driver
 *     configuration. HID minidriver status is expressed by the INF
 *     (Class = HIDClass) and by responding to HID-class internal
 *     IOCTLs on the FDO's default queue.
 *
 *   - The EvtDriverDeviceAdd callback fires once per PDO — in v1
 *     there is at most one PDO at a time because the bus is single-
 *     slot. The binding happens in device.c.
 */

#include "driver.h"
#include "device.h"
#include "trace.h"
#include "driver.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, VhidkmEvtDriverDeviceAdd)
#pragma alloc_text(PAGE, VhidkmEvtDriverContextCleanup)
#endif

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT  DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG     config;
    WDF_OBJECT_ATTRIBUTES attr;
    NTSTATUS              status;

    WPP_INIT_TRACING(DriverObject, RegistryPath);

    TraceDrv("vhidkm: DriverEntry (build %s, api %u)",
             VHID_VERSION_STR_A, VHID_API_LEVEL);

    WDF_DRIVER_CONFIG_INIT(&config, VhidkmEvtDriverDeviceAdd);
    config.DriverPoolTag = VHID_POOL_TAG_HID;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.EvtCleanupCallback = VhidkmEvtDriverContextCleanup;

    status = WdfDriverCreate(DriverObject, RegistryPath,
                             &attr, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        TraceError("WdfDriverCreate failed %!STATUS!", status);
        WPP_CLEANUP(DriverObject);
        return status;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VhidkmEvtDriverDeviceAdd(
    WDFDRIVER       Driver,
    PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS status;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Driver);

    TracePnp("VhidkmEvtDriverDeviceAdd");
    status = VhidkmDeviceCreate(DeviceInit);
    if (!NT_SUCCESS(status)) {
        TraceError("VhidkmDeviceCreate failed %!STATUS!", status);
    }
    return status;
}

_Use_decl_annotations_
VOID
VhidkmEvtDriverContextCleanup(
    WDFOBJECT DriverObject
    )
{
    PAGED_CODE();
    TraceDrv("vhidkm: driver context cleanup");
    WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER)DriverObject));
}
