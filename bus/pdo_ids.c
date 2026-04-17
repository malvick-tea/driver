/*
 * bus/pdo_ids.c
 *
 * Formats and assigns the PDO's PnP identity:
 *
 *   DeviceId     — USB\VID_xxxx&PID_yyyy&REV_zzzz
 *   HardwareIDs  — same, plus USB\VID_xxxx&PID_yyyy
 *   CompatIDs    — USB\Class_03&SubClass_00&Prot_00, USB\Class_03
 *   InstanceID   — the per-install serial, or a fallback derived
 *                  from the slot id if the caller didn't provide one.
 *   DeviceText   — "Virtual HID Keyboard + Mouse" (DEVPKEY_Device_FriendlyName)
 *   Locale       — 0x0409 (en-US)
 *
 * Every Unicode buffer written here is backed by a local array with
 * explicit bounds; WdfPdoInitAssign* / WdfPdoInitAdd* copy the
 * string into framework-owned storage, so the scratch buffers are
 * safe to stack-allocate.
 */

#include "driver.h"
#include "pdo_ids.h"
#include "trace.h"
#include "pdo_ids.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VusbBusPdoAssignIds)
#endif

_Use_decl_annotations_
NTSTATUS
VusbBusPdoAssignIds(
    PWDFDEVICE_INIT DeviceInit,
    PCVUSBBUS_SLOT  Slot
    )
{
    NTSTATUS        status;
    WCHAR           buffer[96];
    UNICODE_STRING  unicodeStr;
    UNICODE_STRING  friendly;

    PAGED_CODE();

    /* ---- Device ID (most specific hardware id) ---- */
    status = RtlStringCchPrintfW(
        buffer, RTL_NUMBER_OF(buffer),
        L"USB\\VID_%04X&PID_%04X&REV_%04X",
        Slot->Vid, Slot->Pid, Slot->Version);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    RtlInitUnicodeString(&unicodeStr, buffer);
    status = WdfPdoInitAssignDeviceID(DeviceInit, &unicodeStr);
    if (!NT_SUCCESS(status)) {
        TraceError("WdfPdoInitAssignDeviceID failed %!STATUS!", status);
        return status;
    }
    status = WdfPdoInitAddHardwareID(DeviceInit, &unicodeStr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* ---- Secondary hardware id (no REV) ---- */
    status = RtlStringCchPrintfW(
        buffer, RTL_NUMBER_OF(buffer),
        L"USB\\VID_%04X&PID_%04X",
        Slot->Vid, Slot->Pid);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    RtlInitUnicodeString(&unicodeStr, buffer);
    status = WdfPdoInitAddHardwareID(DeviceInit, &unicodeStr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* ---- Compatible ids ---- */
    RtlInitUnicodeString(&unicodeStr, VHID_COMPATID_HID_W);
    status = WdfPdoInitAddCompatibleID(DeviceInit, &unicodeStr);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    RtlInitUnicodeString(&unicodeStr, VHID_COMPATID_HID_GEN_W);
    status = WdfPdoInitAddCompatibleID(DeviceInit, &unicodeStr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * Instance ID. Prefer the caller-supplied serial if it contains
     * non-null bytes, otherwise synthesize one from the slot id so
     * PnP can treat instances distinctly.
     */
    {
        BOOLEAN haveSerial = FALSE;
        ULONG   i;
        for (i = 0; i < RTL_NUMBER_OF(Slot->Serial); i++) {
            if (Slot->Serial[i] != L'\0') { haveSerial = TRUE; break; }
        }
        if (haveSerial) {
            RtlZeroMemory(buffer, sizeof(buffer));
            /* Copy at most 32 chars and null-terminate. */
            RtlCopyMemory(buffer, Slot->Serial,
                min(sizeof(Slot->Serial), sizeof(buffer) - sizeof(WCHAR)));
            buffer[RTL_NUMBER_OF(Slot->Serial)] = L'\0';
            RtlInitUnicodeString(&unicodeStr, buffer);
        } else {
            status = RtlStringCchPrintfW(
                buffer, RTL_NUMBER_OF(buffer),
                L"VHID-%08X", Slot->SlotId);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            RtlInitUnicodeString(&unicodeStr, buffer);
        }
        status = WdfPdoInitAssignInstanceID(DeviceInit, &unicodeStr);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    /* Friendly text — visible in Device Manager under Properties. */
    RtlInitUnicodeString(&friendly, VHID_DEFAULT_PRODUCT_STRING_W);
    status = WdfPdoInitAddDeviceText(
        DeviceInit,
        &friendly,
        &friendly,          /* locale-specific and neutral share text */
        0x0409);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WdfPdoInitSetDefaultLocale(DeviceInit, 0x0409);

    TracePdo("PDO ids assigned for slot %u", Slot->SlotId);
    return STATUS_SUCCESS;
}
