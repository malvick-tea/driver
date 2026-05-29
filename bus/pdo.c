/*
 * bus/pdo.c
 *
 * Virtual USB child PDO creation and PnP/power callbacks.
 *
 * Creation flow:
 *   1) WdfPdoInitAllocate - allocate the PDOINIT.
 *   2) Assign hardware ids / compat ids / instance id / device text
 *      (see pdo_ids.c for details).
 *   3) Register an IRP preprocess callback for IRP_MJ_PNP so we can
 *      intercept IRP_MN_QUERY_INTERFACE and return the bus IPC
 *      function table (pdo_iface.c).
 *   4) Create the WDFDEVICE.
 *   5) Assign raw device state (not used) and PnP caps (removable,
 *      surprise-remove OK).
 *
 * The PDO's I/O queues are minimal: a default queue that rejects
 * unrecognized IOCTLs. hidclass.sys talks to the HID minidriver
 * above the PDO; the PDO itself sees only the PnP / QueryInterface
 * traffic.
 */

#include "driver.h"
#include "pdo.h"
#include "pdo_ids.h"
#include "pdo_iface.h"
#include "trace.h"
#include "pdo.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VusbBusPdoCreate)
#pragma alloc_text(PAGE, VusbBusPdoEvtDeviceD0Entry)
#pragma alloc_text(PAGE, VusbBusPdoEvtDeviceD0Exit)
#pragma alloc_text(PAGE, VusbBusPdoEvtCleanup)
#endif

EVT_WDF_DEVICE_D0_ENTRY         VusbBusPdoEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT          VusbBusPdoEvtDeviceD0Exit;
EVT_WDF_OBJECT_CONTEXT_CLEANUP  VusbBusPdoEvtCleanup;

_Use_decl_annotations_
NTSTATUS
VusbBusPdoCreate(
    PVUSBBUS_FDO_CONTEXT FdoCtx,
    PVUSBBUS_SLOT        Slot,
    WDFDEVICE*           OutDevice
    )
{
    PWDFDEVICE_INIT             pdoInit          = NULL;
    WDFDEVICE                   pdo              = NULL;
    PVUSBBUS_PDO_CONTEXT        pdoCtx;
    WDF_OBJECT_ATTRIBUTES       attr;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_DEVICE_PNP_CAPABILITIES pnpCaps;
    WDF_IO_QUEUE_CONFIG         queueCfg;
    NTSTATUS                    status;

    PAGED_CODE();

    *OutDevice = NULL;

    pdoInit = WdfPdoInitAllocate(FdoCtx->Fdo);
    if (pdoInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Assign USB device-class metadata. */
    WdfDeviceInitSetDeviceType(pdoInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(pdoInit, FILE_DEVICE_SECURE_OPEN, TRUE);

    /*
     * PnP / Power callbacks. PDO lives entirely in software; there
     * is no hardware to power up. We still register D0Entry/D0Exit
     * so the power state transitions are traceable.
     */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceD0Entry = VusbBusPdoEvtDeviceD0Entry;
    pnp.EvtDeviceD0Exit  = VusbBusPdoEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(pdoInit, &pnp);

    /*
     * Register the PnP IRP preprocessor for IRP_MN_QUERY_INTERFACE
     * so the HID minidriver can retrieve the published bus IPC
     * callbacks.
     */
    status = VusbBusPdoSetInterfaceDispatch(pdoInit);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    /* Assign hardware / instance / text ids. */
    status = VusbBusPdoAssignIds(pdoInit, Slot);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, VUSBBUS_PDO_CONTEXT);
    attr.EvtCleanupCallback = VusbBusPdoEvtCleanup;

    status = WdfDeviceCreate(&pdoInit, &attr, &pdo);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }
    pdoInit = NULL; /* WdfDeviceCreate consumed it */

    pdoCtx           = VusbBusPdoGetContext(pdo);
    pdoCtx->Pdo      = pdo;
    pdoCtx->Slot     = Slot;
    pdoCtx->FdoCtx   = FdoCtx;

    /*
     * Capture the serial into the PDO context so the IPC descriptor
     * path can answer iSerialNumber without touching the slot table.
     * Slot->Serial is not guaranteed null-terminated; record the
     * populated character count (stop at the first NUL, cap at the
     * array size). SerialChars == 0 means "fall back to the default
     * serial string".
     */
    {
        ULONG i;
        RtlCopyMemory(pdoCtx->Serial, Slot->Serial, sizeof(pdoCtx->Serial));
        pdoCtx->SerialChars = 0;
        for (i = 0; i < RTL_NUMBER_OF(pdoCtx->Serial); i++) {
            if (pdoCtx->Serial[i] == L'\0') {
                break;
            }
            pdoCtx->SerialChars++;
        }
    }

    {
        WDF_OBJECT_ATTRIBUTES lockAttr;
        WDF_OBJECT_ATTRIBUTES_INIT(&lockAttr);
        lockAttr.ParentObject = pdo;
        status = WdfSpinLockCreate(&lockAttr, &pdoCtx->IpcLock);
        if (!NT_SUCCESS(status)) {
            goto fail;
        }
    }

    /* LED-waiter manual queue. Consumed by IPC LED notifications. */
    WDF_IO_QUEUE_CONFIG_INIT(&queueCfg, WdfIoQueueDispatchManual);
    {
        WDF_OBJECT_ATTRIBUTES qAttr;
        WDF_OBJECT_ATTRIBUTES_INIT(&qAttr);
        qAttr.ParentObject = pdo;
        status = WdfIoQueueCreate(pdo, &queueCfg, &qAttr, &pdoCtx->LedWaitQueue);
        if (!NT_SUCCESS(status)) {
            goto fail;
        }
    }

    /* PnP capabilities: the device is removable and surprise-remove safe. */
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);
    pnpCaps.Removable           = WdfTrue;
    pnpCaps.SurpriseRemovalOK   = WdfTrue;
    pnpCaps.EjectSupported      = WdfFalse;
    pnpCaps.UniqueID            = WdfTrue;
    pnpCaps.NoDisplayInUI       = WdfFalse;
    pnpCaps.Address             = Slot->SlotId;
    pnpCaps.UINumber            = Slot->SlotId;
    WdfDeviceSetPnpCapabilities(pdo, &pnpCaps);

    /* Default queue: reject unknown IRPs with STATUS_INVALID_DEVICE_REQUEST. */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueCfg, WdfIoQueueDispatchParallel);
    status = WdfIoQueueCreate(pdo, &queueCfg, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    *OutDevice = pdo;
    TracePdo("PDO created for slot %u", Slot->SlotId);
    return STATUS_SUCCESS;

fail:
    if (pdoInit != NULL) {
        WdfDeviceInitFree(pdoInit);
    }
    if (pdo != NULL) {
        WdfObjectDelete(pdo);
    }
    TraceError("VusbBusPdoCreate failed %!STATUS!", status);
    return status;
}

_Use_decl_annotations_
NTSTATUS
VusbBusPdoEvtDeviceD0Entry(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    TracePwr("PDO D0Entry from %d", (int)PreviousState);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VusbBusPdoEvtDeviceD0Exit(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE TargetState
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    TracePwr("PDO D0Exit to %d", (int)TargetState);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
VusbBusPdoEvtCleanup(
    WDFOBJECT Object
    )
{
    PVUSBBUS_PDO_CONTEXT ctx = VusbBusPdoGetContext((WDFDEVICE)Object);

    PAGED_CODE();

    TracePdo("PDO cleanup");

    /*
     * If the FDO slot table still points to us, scrub the backpointer.
     * The owning FDO (and therefore SlotLock) outlives its child PDOs,
     * so the lock is valid here. Take it because a concurrent
     * PLUG_IN/UNPLUG can be mutating the same slot entry: only clear the
     * handle if it still refers to THIS device, so we never clobber a
     * slot that has already been recycled to a new PDO.
     */
    if (ctx->FdoCtx != NULL && ctx->FdoCtx->SlotLock != NULL &&
        ctx->Slot != NULL) {
        WdfWaitLockAcquire(ctx->FdoCtx->SlotLock, NULL);
        if (ctx->Slot->PdoDevice == (WDFDEVICE)Object) {
            ctx->Slot->PdoDevice = NULL;
        }
        WdfWaitLockRelease(ctx->FdoCtx->SlotLock);
    }
}
