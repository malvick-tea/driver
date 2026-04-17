/*
 * bus/driver.c
 *
 * DriverEntry and EvtDriverDeviceAdd for vusbbus.sys.
 *
 * Lifecycle:
 *   - DriverEntry wires up WPP, configures the KMDF WDFDRIVER with
 *     WdfDriverCreate, and exits.
 *   - EvtDriverDeviceAdd creates the bus FDO (see fdo.c) and a
 *     control device (see ctl_device.c) bound to the FDO. The
 *     control device is the user-mode-facing endpoint used by
 *     IOCTL_VUSBBUS_* traffic.
 *   - EvtDriverContextCleanup tears down WPP. KMDF cleans up the
 *     driver objects before this fires.
 *
 * There is no global mutable state in this driver — everything
 * hangs off the WDFDRIVER context or the FDO context. That keeps
 * Driver Verifier's pool tracking clean and avoids class-global
 * locks.
 */

#include "driver.h"
#include "fdo.h"
#include "trace.h"
#include "driver.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, VusbBusEvtDriverDeviceAdd)
#pragma alloc_text(PAGE, VusbBusEvtDriverContextCleanup)
#endif

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT  DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG       config;
    WDF_OBJECT_ATTRIBUTES   attributes;
    NTSTATUS                status;

    /*
     * Start WPP as early as possible so failures in WdfDriverCreate
     * below are traced. WPP_CLEANUP is called from
     * VusbBusEvtDriverContextCleanup; it is safe to call WPP_CLEANUP
     * only if WPP_INIT_TRACING ran successfully, so the cleanup
     * routine is wired up only after this call returns.
     */
    WPP_INIT_TRACING(DriverObject, RegistryPath);

    TraceDrv("vusbbus: DriverEntry (build %s, api %u)",
        VHID_VERSION_STR_A, VHID_API_LEVEL);

    WDF_DRIVER_CONFIG_INIT(&config, VusbBusEvtDriverDeviceAdd);
    config.DriverPoolTag = VHID_POOL_TAG_BUS;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = VusbBusEvtDriverContextCleanup;

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        &attributes,
        &config,
        WDF_NO_HANDLE);

    if (!NT_SUCCESS(status)) {
        TraceError("WdfDriverCreate failed %!STATUS!", status);
        WPP_CLEANUP(DriverObject);
        return status;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VusbBusEvtDriverDeviceAdd(
    WDFDRIVER       Driver,
    PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(Driver);

    TracePnp("VusbBusEvtDriverDeviceAdd entered");

    /*
     * All the real setup lives in fdo.c. Keeping driver.c tiny
     * makes DriverEntry easy to audit against SDV / PREfast rules.
     */
    status = VusbBusFdoCreate(DeviceInit);

    if (!NT_SUCCESS(status)) {
        TraceError("VusbBusFdoCreate failed %!STATUS!", status);
    }

    return status;
}

_Use_decl_annotations_
VOID
VusbBusEvtDriverContextCleanup(
    WDFOBJECT DriverObject
    )
{
    PAGED_CODE();

    TraceDrv("vusbbus: driver context cleanup");

    /*
     * WdfDriverWdmGetDriverObject on a WDFDRIVER returns the
     * DRIVER_OBJECT originally passed to DriverEntry. WPP_CLEANUP
     * requires the raw DRIVER_OBJECT, not the WDFDRIVER handle.
     */
    WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER)DriverObject));
}
