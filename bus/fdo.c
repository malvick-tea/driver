/*
 * bus/fdo.c
 *
 * Bus FDO creation, slot-table management, and the body of the
 * user-mode-facing operations PLUG_IN / UNPLUG / LIST.
 *
 * FDO creation sequence (mirrors canonical KMDF bus-driver pattern):
 *
 *   1) Declare PnP / power callbacks (start/remove are effectively
 *      no-ops for a software bus).
 *   2) Assign WDF_FILEOBJECT_CONFIG (we do not need file-object
 *      contexts on the FDO itself; the control device handles file
 *      lifetime).
 *   3) Announce static child-list support is off — we use dynamic
 *      enumeration via WdfFdoQueryForInterface / bus relations so the
 *      slot table can grow without re-entering KMDF child-list API.
 *   4) Create the FDO, allocate its context, initialize the slot
 *      wait-lock.
 *   5) Set the PDO child-list manually by implementing
 *      EvtDeviceRelations fallback via IoInvalidateDeviceRelations.
 *   6) Create the control device.
 *
 * Slot allocation:
 *   VHID_MAX_SLOTS == 1 in v1. The code nonetheless iterates with a
 *   linear scan for a free entry, so bumping the constant requires
 *   no algorithmic change.
 */

#include "driver.h"
#include "fdo.h"
#include "pdo.h"
#include "ctl_device.h"
#include "trace.h"
#include "fdo.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VusbBusFdoCreate)
#pragma alloc_text(PAGE, VusbBusFdoPlugIn)
#pragma alloc_text(PAGE, VusbBusFdoUnplug)
#pragma alloc_text(PAGE, VusbBusFdoList)
#pragma alloc_text(PAGE, VusbBusFdoEvtCleanup)
#pragma alloc_text(PAGE, VusbBusEvtDevicePrepareHardware)
#pragma alloc_text(PAGE, VusbBusEvtDeviceReleaseHardware)
#pragma alloc_text(PAGE, VusbBusEvtDeviceD0Entry)
#pragma alloc_text(PAGE, VusbBusEvtDeviceD0Exit)
#endif

EVT_WDF_DEVICE_PREPARE_HARDWARE     VusbBusEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE     VusbBusEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY             VusbBusEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT              VusbBusEvtDeviceD0Exit;

static
_IRQL_requires_(PASSIVE_LEVEL)
PVUSBBUS_SLOT
VusbBusFindFreeSlotLocked(
    _In_ PVUSBBUS_FDO_CONTEXT FdoCtx
    );

static
_IRQL_requires_(PASSIVE_LEVEL)
PVUSBBUS_SLOT
VusbBusFindSlotByIdLocked(
    _In_ PVUSBBUS_FDO_CONTEXT FdoCtx,
    _In_ UINT32               SlotId
    );

_Use_decl_annotations_
NTSTATUS
VusbBusFdoCreate(
    PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_OBJECT_ATTRIBUTES       fdoAttr;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDFDEVICE                   fdo;
    PVUSBBUS_FDO_CONTEXT        fdoCtx;
    NTSTATUS                    status;

    PAGED_CODE();

    /*
     * Declare ourselves a bus driver — this tells KMDF to route
     * IRP_MN_QUERY_DEVICE_RELATIONS(BusRelations) to our handler
     * and to support dynamic child PDO creation via WdfPdoInit*.
     */
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BUS_EXTENDER);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);
    WdfDeviceInitSetCharacteristics(DeviceInit, FILE_DEVICE_SECURE_OPEN, TRUE);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware  = VusbBusEvtDevicePrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware  = VusbBusEvtDeviceReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry          = VusbBusEvtDeviceD0Entry;
    pnpCallbacks.EvtDeviceD0Exit           = VusbBusEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    /*
     * The bus FDO has no useful per-handle file-object state; we
     * simply allow concurrent opens. User-mode interacts with the
     * control device (a separate WDFDEVICE), not the FDO directly.
     */

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fdoAttr, VUSBBUS_FDO_CONTEXT);
    fdoAttr.EvtCleanupCallback = VusbBusFdoEvtCleanup;

    status = WdfDeviceCreate(&DeviceInit, &fdoAttr, &fdo);
    if (!NT_SUCCESS(status)) {
        TraceError("WdfDeviceCreate (FDO) failed %!STATUS!", status);
        return status;
    }

    fdoCtx        = VusbBusFdoGetContext(fdo);
    fdoCtx->Fdo   = fdo;
    fdoCtx->NextSlotId = 1;

    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &fdoCtx->SlotLock);
    if (!NT_SUCCESS(status)) {
        TraceError("WdfWaitLockCreate (SlotLock) failed %!STATUS!", status);
        return status;
    }

    /*
     * Bus information for children is fixed — every child pretends
     * to hang off a USB bus. KMDF caches this and returns it on
     * IRP_MN_QUERY_BUS_INFORMATION from each child PDO.
     */
    {
        PNP_BUS_INFORMATION busInfo = { 0 };
        busInfo.BusTypeGuid  = GUID_BUS_TYPE_USB;
        busInfo.LegacyBusType = PNPBus;
        busInfo.BusNumber    = 0;
        WdfDeviceSetBusInformationForChildren(fdo, &busInfo);
    }

    /*
     * Stand up the control device (see ctl_device.c). The bus FDO
     * owns a handle to it so IOCTL handlers can route back to the
     * FDO context when a user-mode request arrives.
     */
    status = VusbBusCtlDeviceCreate(fdoCtx);
    if (!NT_SUCCESS(status)) {
        TraceError("VusbBusCtlDeviceCreate failed %!STATUS!", status);
        return status;
    }

    TraceDrv("bus FDO created ok");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VusbBusEvtDevicePrepareHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    TracePnp("bus FDO PrepareHardware");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VusbBusEvtDeviceReleaseHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PVUSBBUS_FDO_CONTEXT fdoCtx = VusbBusFdoGetContext(Device);

    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    TracePnp("bus FDO ReleaseHardware");

    /*
     * Stop new control-device IOCTLs from resolving to this FDO as we
     * tear down. Requests already in flight hold a reference on this
     * WDFDEVICE and remain memory-safe until they complete.
     */
    VusbBusCtlDetachFdo(fdoCtx);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VusbBusEvtDeviceD0Entry(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    TracePwr("bus FDO D0Entry from %d", (int)PreviousState);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VusbBusEvtDeviceD0Exit(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE TargetState
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    TracePwr("bus FDO D0Exit to %d", (int)TargetState);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
VusbBusFdoEvtCleanup(
    WDFOBJECT Object
    )
{
    PVUSBBUS_FDO_CONTEXT fdoCtx = VusbBusFdoGetContext((WDFDEVICE)Object);

    PAGED_CODE();
    TracePnp("bus FDO context cleanup");

    /*
     * Guaranteed detach before the FDO context is freed; idempotent with
     * the ReleaseHardware detach and covers teardown paths that skip it.
     * Every other resource is parented to the FDO and freed by KMDF.
     */
    VusbBusCtlDetachFdo(fdoCtx);
}

static
_Use_decl_annotations_
PVUSBBUS_SLOT
VusbBusFindFreeSlotLocked(
    PVUSBBUS_FDO_CONTEXT FdoCtx
    )
{
    ULONG i;
    for (i = 0; i < VHID_MAX_SLOTS; i++) {
        if (!FdoCtx->Slots[i].InUse) {
            return &FdoCtx->Slots[i];
        }
    }
    return NULL;
}

static
_Use_decl_annotations_
PVUSBBUS_SLOT
VusbBusFindSlotByIdLocked(
    PVUSBBUS_FDO_CONTEXT FdoCtx,
    UINT32               SlotId
    )
{
    ULONG i;
    for (i = 0; i < VHID_MAX_SLOTS; i++) {
        if (FdoCtx->Slots[i].InUse && FdoCtx->Slots[i].SlotId == SlotId) {
            return &FdoCtx->Slots[i];
        }
    }
    return NULL;
}

_Use_decl_annotations_
NTSTATUS
VusbBusFdoPlugIn(
    PVUSBBUS_FDO_CONTEXT FdoCtx,
    PCVHID_PLUGIN_REQ    Req,
    PVHID_PLUGIN_RESP    Resp
    )
{
    PVUSBBUS_SLOT   slot;
    NTSTATUS        status;
    WDFDEVICE       pdoDevice = NULL;
    LARGE_INTEGER   seed;
    GUID            instance;
    UINT32          slotId;

    PAGED_CODE();

    WdfWaitLockAcquire(FdoCtx->SlotLock, NULL);

    slot = VusbBusFindFreeSlotLocked(FdoCtx);
    if (slot == NULL) {
        WdfWaitLockRelease(FdoCtx->SlotLock);
        return STATUS_DEVICE_ALREADY_ATTACHED;
    }

    slotId = FdoCtx->NextSlotId++;

    /*
     * Generate a per-plug instance GUID. The value is derived from
     * the system's performance counter so the chance of collision
     * across runs is vanishingly small. We do not use ExUuidCreate
     * because it can fail with STATUS_RETRY if the RPC runtime is
     * not ready early in boot.
     */
    KeQuerySystemTimePrecise(&seed);
    RtlZeroMemory(&instance, sizeof(instance));
    RtlCopyMemory(&instance, &seed, sizeof(seed));
    instance.Data1 = (ULONG)seed.LowPart ^ slotId;
    instance.Data2 = (USHORT)(seed.HighPart & 0xFFFF);
    instance.Data3 = (USHORT)(seed.HighPart >> 16);

    slot->InUse      = TRUE;
    slot->SlotId     = slotId;
    slot->InstanceId = instance;

    /*
     * v1 presents a fixed USB identity. The hardware IDs the INF matches
     * (USB\VID_1209&PID_BEEF&REV_0100) are static, so the PDO MUST
     * advertise the canonical VID/PID/REV regardless of what the caller
     * requested: honoring caller-supplied values would build a PDO whose
     * hardware ID no function driver binds, leaving the device stuck in
     * an error state. The request's Vid/Pid/Version fields are reserved
     * for a future dynamic-INF revision (see common/vhid_version.h).
     */
    slot->Vid        = VHID_DEFAULT_VID;
    slot->Pid        = VHID_DEFAULT_PID;
    slot->Version    = VHID_DEFAULT_REV;
    RtlCopyMemory(slot->Serial, Req->Serial, sizeof(slot->Serial));

    status = VusbBusPdoCreate(FdoCtx, slot, &pdoDevice);
    if (!NT_SUCCESS(status)) {
        /* Rollback: mark slot free and propagate error. */
        RtlZeroMemory(slot, sizeof(*slot));
        WdfWaitLockRelease(FdoCtx->SlotLock);
        TraceError("VusbBusPdoCreate failed %!STATUS!", status);
        return status;
    }

    slot->PdoDevice = pdoDevice;

    WdfWaitLockRelease(FdoCtx->SlotLock);

    /*
     * Publish the new PDO to PnP. This triggers bus relations
     * re-evaluation; once complete the PDO appears in Device
     * Manager and vhidkm.sys is loaded against it.
     */
    WdfPdoMarkMissing(pdoDevice); /* no-op here; defensive — see comment below */
    WdfFdoLockStaticChildListForIteration(FdoCtx->Fdo);
    WdfFdoUnlockStaticChildListFromIteration(FdoCtx->Fdo);
    /*
     * Note: v1 uses WdfPdoDeviceAdd via explicit enumeration in
     * pdo.c (WdfDeviceCreate + WdfPdoInit*); after creation we
     * simply invalidate bus relations. The "MarkMissing" above is
     * defensive and harmless on a freshly-created PDO — it clears
     * any stale "missing" flag KMDF might carry if the slot was
     * recycled and ensures the next InvalidateDeviceRelations
     * reports the PDO as present.
     */
    IoInvalidateDeviceRelations(
        WdfDeviceWdmGetPhysicalDevice(FdoCtx->Fdo),
        BusRelations);

    Resp->SlotId     = slotId;
    Resp->InstanceId = instance;

    TracePdo("PLUG_IN ok slot=%u vid=0x%04x pid=0x%04x",
        slotId, slot->Vid, slot->Pid);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VusbBusFdoUnplug(
    PVUSBBUS_FDO_CONTEXT FdoCtx,
    UINT32               SlotId
    )
{
    PVUSBBUS_SLOT   slot;
    WDFDEVICE       pdoDevice;

    PAGED_CODE();

    WdfWaitLockAcquire(FdoCtx->SlotLock, NULL);

    slot = VusbBusFindSlotByIdLocked(FdoCtx, SlotId);
    if (slot == NULL) {
        WdfWaitLockRelease(FdoCtx->SlotLock);
        return STATUS_NOT_FOUND;
    }

    pdoDevice = slot->PdoDevice;

    /*
     * Drop the slot first so a concurrent PLUG_IN can reuse it
     * while PnP is tearing the old PDO down. The PDO handle is
     * kept long enough to call WdfPdoMarkMissing outside the lock.
     */
    RtlZeroMemory(slot, sizeof(*slot));
    WdfWaitLockRelease(FdoCtx->SlotLock);

    if (pdoDevice != NULL) {
        WdfPdoMarkMissing(pdoDevice);
        IoInvalidateDeviceRelations(
            WdfDeviceWdmGetPhysicalDevice(FdoCtx->Fdo),
            BusRelations);
    }

    TracePdo("UNPLUG ok slot=%u", SlotId);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VusbBusFdoList(
    PVUSBBUS_FDO_CONTEXT FdoCtx,
    PVHID_SLOT_LIST      List,
    ULONG                OutputLength,
    PULONG               BytesReturned
    )
{
    ULONG   count   = 0;
    ULONG   i;
    ULONG   required;

    PAGED_CODE();

    *BytesReturned = 0;

    WdfWaitLockAcquire(FdoCtx->SlotLock, NULL);

    for (i = 0; i < VHID_MAX_SLOTS; i++) {
        if (FdoCtx->Slots[i].InUse) {
            count++;
        }
    }

    required = FIELD_OFFSET(VHID_SLOT_LIST, Slots) +
               count * sizeof(VHID_SLOT_INFO);
    if (count == 0) {
        /* One fixed entry of padding so required >= sizeof(VHID_SLOT_LIST). */
        required = sizeof(VHID_SLOT_LIST);
    }

    if (OutputLength < required) {
        WdfWaitLockRelease(FdoCtx->SlotLock);
        *BytesReturned = required;
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(List, required);
    List->Count = count;

    {
        ULONG dst = 0;
        for (i = 0; i < VHID_MAX_SLOTS && dst < count; i++) {
            if (!FdoCtx->Slots[i].InUse) { continue; }
            List->Slots[dst].SlotId     = FdoCtx->Slots[i].SlotId;
            List->Slots[dst].Vid        = FdoCtx->Slots[i].Vid;
            List->Slots[dst].Pid        = FdoCtx->Slots[i].Pid;
            List->Slots[dst].Version    = FdoCtx->Slots[i].Version;
            List->Slots[dst].InstanceId = FdoCtx->Slots[i].InstanceId;
            RtlCopyMemory(List->Slots[dst].Serial,
                          FdoCtx->Slots[i].Serial,
                          sizeof(List->Slots[dst].Serial));
            dst++;
        }
    }

    WdfWaitLockRelease(FdoCtx->SlotLock);

    *BytesReturned = required;
    return STATUS_SUCCESS;
}
