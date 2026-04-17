/*
 * hid/bus_iface.c
 *
 * Acquire / release the published bus IPC interface via
 * WdfFdoQueryForInterface.
 *
 * WdfFdoQueryForInterface handles the IRP_MN_QUERY_INTERFACE
 * plumbing: it allocates the IRP, initializes the stack location
 * with the interface GUID and version, sends it down the PDO's
 * driver stack, and copies the result into a caller-supplied
 * buffer. The bus driver's WDM preprocessor (bus/pdo_iface.c)
 * intercepts the IRP, fills in the function table, calls our
 * reference hook on the context pointer, and completes the IRP.
 *
 * After acquisition the bus-side reference keeps the PDO alive;
 * release drops it. If we skip release (e.g., the driver unloads
 * without ReleaseHardware), KMDF's cleanup pass still completes the
 * dereference because we parent the interface-holding context to
 * the FDO and cleanup invokes it automatically.
 */

#include "driver.h"
#include "device.h"
#include "bus_iface.h"
#include "trace.h"
#include "bus_iface.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VhidkmBusIfaceAcquire)
#pragma alloc_text(PAGE, VhidkmBusIfaceRelease)
#endif

_Use_decl_annotations_
NTSTATUS
VhidkmBusIfaceAcquire(
    PVHIDKM_DEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS status;

    PAGED_CODE();

    if (DevCtx->IpcValid) {
        /* Already acquired — idempotent. */
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&DevCtx->BusIpc, sizeof(DevCtx->BusIpc));

    status = WdfFdoQueryForInterface(
        DevCtx->Device,
        &GUID_VHID_BUS_INTERFACE_V1,
        (PINTERFACE)&DevCtx->BusIpc,
        sizeof(VHID_BUS_INTERFACE_V1),
        VHID_BUS_INTERFACE_VERSION_V1,
        NULL);
    if (!NT_SUCCESS(status)) {
        TraceIpc("WdfFdoQueryForInterface failed %!STATUS!", status);
        return status;
    }

    /*
     * WdfFdoQueryForInterface already invoked InterfaceReference on
     * our behalf. We must match it with exactly one call to
     * InterfaceDereference in Release. Nulling the dereference
     * function would leak the reference — double-check the table.
     */
    if (DevCtx->BusIpc.InterfaceHeader.InterfaceDereference == NULL ||
        DevCtx->BusIpc.InterfaceHeader.InterfaceReference == NULL) {
        TraceError("Bus IPC interface missing ref/deref routines");
        RtlZeroMemory(&DevCtx->BusIpc, sizeof(DevCtx->BusIpc));
        return STATUS_INVALID_DEVICE_STATE;
    }

    DevCtx->IpcValid = TRUE;
    TraceIpc("bus IPC interface acquired (version 0x%04x)",
             (ULONG)DevCtx->BusIpc.InterfaceHeader.Version);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
VhidkmBusIfaceRelease(
    PVHIDKM_DEVICE_CONTEXT DevCtx
    )
{
    PINTERFACE_DEREFERENCE deref;

    PAGED_CODE();

    if (!DevCtx->IpcValid) {
        return;
    }

    deref = DevCtx->BusIpc.InterfaceHeader.InterfaceDereference;
    if (deref != NULL) {
        deref(DevCtx->BusIpc.InterfaceHeader.Context);
    }

    /*
     * Zero the table before clearing the valid flag. Any racing
     * reader (there shouldn't be any at ReleaseHardware, but defend
     * in depth) sees either the populated valid table or the empty
     * invalid one, never a half-populated transitional state.
     */
    RtlZeroMemory(&DevCtx->BusIpc, sizeof(DevCtx->BusIpc));
    DevCtx->IpcValid = FALSE;
    TraceIpc("bus IPC interface released");
}
