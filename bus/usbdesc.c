/*
 * bus/usbdesc.c
 *
 * Canonical USB descriptor tables. Exposed through the bus IPC
 * interface to vhidkm.sys and to user-mode for diagnostic purposes
 * via IOCTL_VUSBBUS_GET_USB_DESCRIPTOR.
 *
 * Descriptor layout matches docs/ARCHITECTURE.md section 5. The table
 * is not transmitted over a wire (there is no wire) but it is the
 * single source of truth for fields the HID minidriver returns
 * through IOCTL_HID_GET_DEVICE_ATTRIBUTES / GET_STRING.
 *
 * The string-descriptor path has two entry points: VusbBusUsbDescCopy
 * serves the compile-time-constant strings (manufacturer, product, and
 * a fixed default serial) for the diagnostic IOCTL that has no device
 * context, while VusbBusUsbDescCopySerialString builds string index 3
 * from a per-instance serial supplied by the caller.
 */

#include "driver.h"
#include "usbdesc.h"
#include "trace.h"
#include "usbdesc.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VusbBusUsbDescCopy)
#pragma alloc_text(PAGE, VusbBusUsbDescCopySerialString)
#endif

/*
 * The report descriptor itself lives in hid/hid_descriptor.c; only its
 * size is referenced here (embedded in the HID class descriptor's
 * wReportDescLength). We keep the size as a compile-time constant so
 * both drivers agree without a build-time linker symbol exchange.
 */
#define VHID_REPORT_DESCRIPTOR_SIZE 175u

/*
 * Largest payload a USB string descriptor can carry: bLength is a
 * single byte, so total bytes <= 255, leaving 253 for the payload.
 * Round down to a whole number of UTF-16 code units so a truncated
 * descriptor never ends on half a WCHAR.
 */
#define USB_STRING_MAX_PAYLOAD_BYTES  (((0xFFu - 2u)) & ~1u)

#include <pshpack1.h>

/* Device descriptor (18 bytes). */
static const UCHAR g_DeviceDescriptor[] = {
    0x12,                       /* bLength            */
    0x01,                       /* bDescriptorType    = DEVICE */
    0x00, 0x02,                 /* bcdUSB             = 2.00   */
    0x00,                       /* bDeviceClass       (iface)  */
    0x00,                       /* bDeviceSubClass             */
    0x00,                       /* bDeviceProtocol             */
    0x40,                       /* bMaxPacketSize0    = 64     */
    0x09, 0x12,                 /* idVendor           = 0x1209 */
    0xEF, 0xBE,                 /* idProduct          = 0xBEEF */
    0x00, 0x01,                 /* bcdDevice          = 1.00   */
    0x01,                       /* iManufacturer      = 1      */
    0x02,                       /* iProduct           = 2      */
    0x03,                       /* iSerialNumber      = 3      */
    0x01                        /* bNumConfigurations = 1      */
};

/* Config + Interface + HID + Endpoint (34 bytes). */
static const UCHAR g_ConfigurationDescriptor[] = {
    /* Configuration */
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xA0, 0x32,
    /* Interface */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    /* HID class */
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
    (UCHAR)(VHID_REPORT_DESCRIPTOR_SIZE & 0xFF),
    (UCHAR)((VHID_REPORT_DESCRIPTOR_SIZE >> 8) & 0xFF),
    /* Endpoint (Interrupt IN 0x81, 64 bytes, 1 ms) */
    0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x01
};

/* Just the HID class descriptor (9 bytes). */
static const UCHAR g_HidClassDescriptor[] = {
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
    (UCHAR)(VHID_REPORT_DESCRIPTOR_SIZE & 0xFF),
    (UCHAR)((VHID_REPORT_DESCRIPTOR_SIZE >> 8) & 0xFF)
};

/* String 0 - LangID array (en-US, 0x0409). */
static const UCHAR g_String0[] = {
    0x04, 0x03, 0x09, 0x04
};

/*
 * Strings 1..3 are materialized on demand. The manufacturer and
 * product strings are compile-time constants; the serial string is
 * either a per-instance value or this fixed default. The helper
 * constructs the 2-byte header + UTF-16LE payload at copy time.
 */
static const WCHAR g_Str1[] = VHID_DEFAULT_MFG_STRING_W;
static const WCHAR g_Str2[] = VHID_DEFAULT_PRODUCT_STRING_W;
static const WCHAR g_Str3_default[] = L"000102030405060708090A0B0C0D0E0F";

#include <poppack.h>

static
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
CopyBytes(
    _In_reads_bytes_(SrcLength) const UCHAR* Src,
    _In_ ULONG  SrcLength,
    _Out_writes_bytes_to_(DstLength, *Returned) PVOID Dst,
    _In_ ULONG  DstLength,
    _Out_ PULONG Returned
    )
{
    ULONG toCopy;

    if (DstLength == 0) {
        *Returned = SrcLength;
        return STATUS_BUFFER_TOO_SMALL;
    }
    toCopy = (DstLength < SrcLength) ? DstLength : SrcLength;
    RtlCopyMemory(Dst, Src, toCopy);
    *Returned = toCopy;

    if (toCopy < SrcLength) {
        return STATUS_BUFFER_OVERFLOW;
    }
    return STATUS_SUCCESS;
}

static
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
CopyStringDescriptor(
    _In_reads_(WideLength) const WCHAR* Wide,
    _In_ ULONG  WideLength, /* char count (excl. terminator) */
    _Out_writes_bytes_to_(DstLength, *Returned) PVOID Dst,
    _In_ ULONG  DstLength,
    _Out_ PULONG Returned
    )
{
    UCHAR   header[2];
    ULONG   payloadBytes = WideLength * sizeof(WCHAR);
    ULONG   totalBytes;
    PUCHAR  out          = (PUCHAR)Dst;

    *Returned = 0;

    if (payloadBytes > USB_STRING_MAX_PAYLOAD_BYTES) {
        /*
         * bLength is 8-bit; truncate, but keep the payload a whole
         * number of UTF-16 code units so we never emit half a WCHAR.
         */
        payloadBytes = USB_STRING_MAX_PAYLOAD_BYTES;
    }
    totalBytes = payloadBytes + sizeof(header);

    if (DstLength < totalBytes) {
        *Returned = totalBytes;
        return STATUS_BUFFER_TOO_SMALL;
    }

    header[0] = (UCHAR)totalBytes;
    header[1] = 0x03;       /* STRING */
    RtlCopyMemory(out, header, sizeof(header));
    RtlCopyMemory(out + sizeof(header), Wide, payloadBytes);
    *Returned = totalBytes;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
VusbBusUsbDescCopy(
    UCHAR  DescriptorType,
    UCHAR  DescriptorIndex,
    PVOID  Buffer,
    ULONG  BufferLength,
    PULONG BytesReturned
    )
{
    PAGED_CODE();

    if (Buffer == NULL || BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    switch (DescriptorType) {
    case USB_DESC_TYPE_DEVICE:
        return CopyBytes(g_DeviceDescriptor, sizeof(g_DeviceDescriptor),
                         Buffer, BufferLength, BytesReturned);

    case USB_DESC_TYPE_CONFIGURATION:
        return CopyBytes(g_ConfigurationDescriptor,
                         sizeof(g_ConfigurationDescriptor),
                         Buffer, BufferLength, BytesReturned);

    case USB_DESC_TYPE_HID:
        return CopyBytes(g_HidClassDescriptor, sizeof(g_HidClassDescriptor),
                         Buffer, BufferLength, BytesReturned);

    case USB_DESC_TYPE_STRING:
        switch (DescriptorIndex) {
        case 0:
            return CopyBytes(g_String0, sizeof(g_String0),
                             Buffer, BufferLength, BytesReturned);
        case 1:
            return CopyStringDescriptor(g_Str1,
                (ULONG)(RTL_NUMBER_OF(g_Str1) - 1),
                Buffer, BufferLength, BytesReturned);
        case 2:
            return CopyStringDescriptor(g_Str2,
                (ULONG)(RTL_NUMBER_OF(g_Str2) - 1),
                Buffer, BufferLength, BytesReturned);
        case 3:
            return CopyStringDescriptor(g_Str3_default,
                (ULONG)(RTL_NUMBER_OF(g_Str3_default) - 1),
                Buffer, BufferLength, BytesReturned);
        default:
            return STATUS_NOT_FOUND;
        }

    case USB_DESC_TYPE_REPORT:
        /*
         * The report descriptor lives with the HID minidriver. The bus
         * driver does not serve it directly; callers that need it go
         * through IOCTL_HID_GET_REPORT_DESCRIPTOR on the HID side. We
         * surface STATUS_NOT_SUPPORTED here rather than NOT_FOUND
         * because the descriptor exists, just not at this layer.
         */
        return STATUS_NOT_SUPPORTED;

    default:
        return STATUS_INVALID_PARAMETER;
    }
}

_Use_decl_annotations_
NTSTATUS
VusbBusUsbDescCopySerialString(
    const WCHAR* Serial,
    ULONG        SerialChars,
    PVOID        Buffer,
    ULONG        BufferLength,
    PULONG       BytesReturned
    )
{
    PAGED_CODE();

    if (Buffer == NULL || BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    /*
     * No per-instance serial: fall back to the fixed default so the
     * device always presents a non-empty iSerialNumber string.
     */
    if (Serial == NULL || SerialChars == 0) {
        return CopyStringDescriptor(g_Str3_default,
            (ULONG)(RTL_NUMBER_OF(g_Str3_default) - 1),
            Buffer, BufferLength, BytesReturned);
    }

    return CopyStringDescriptor(Serial, SerialChars,
                                Buffer, BufferLength, BytesReturned);
}
