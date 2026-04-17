/*
 * hid/hid_descriptor.c
 *
 * HID class descriptor and report descriptor storage + accessors.
 *
 * Design choices worth noting:
 *
 *   - Report descriptor lives in a single static const UCHAR array.
 *     Its size is a compile-time constant that the bus driver
 *     also hard-codes (VHID_REPORT_DESCRIPTOR_SIZE in bus/usbdesc.c)
 *     so both drivers agree on the bytes advertised in the USB HID
 *     class descriptor without a runtime handshake. A C_ASSERT at
 *     the bottom of this file guarantees the sizes stay synchronized.
 *
 *   - The HID class descriptor is materialized at runtime (not a
 *     static table) because PHID_DESCRIPTOR is the exact struct
 *     hidclass.sys hands us, with its own field widths; writing
 *     through the pointer avoids relying on struct-packing rules
 *     the WDK's hidclass.h applies to HID_DESCRIPTOR.
 *
 *   - The report descriptor is reproduced byte-for-byte from the
 *     architecture document. Every byte is annotated with the HID
 *     tag/value. Developers tweaking it should run the bytes through
 *     the Microsoft HID Descriptor Tool (or HIDAPI's descriptor
 *     parser) after any change; a malformed descriptor will not
 *     bugcheck but will cause HidParser to reject enumeration and
 *     mouhid/kbdhid child PDOs will not appear.
 */

#include "driver.h"
#include "hid_descriptor.h"
#include "trace.h"
#include "hid_descriptor.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VhidkmHidGetClassDescriptor)
#pragma alloc_text(PAGE, VhidkmHidGetReportDescriptorSize)
#pragma alloc_text(PAGE, VhidkmHidCopyReportDescriptor)
#endif

static const UCHAR g_HidReportDescriptor[] =
{
    /* ---------------------------------------------------------------
     * Keyboard top-level application collection (Report ID 1)
     * --------------------------------------------------------------- */
    0x05, 0x01,                   /* Usage Page (Generic Desktop)       */
    0x09, 0x06,                   /* Usage (Keyboard)                   */
    0xA1, 0x01,                   /* Collection (Application)           */
    0x85, 0x01,                   /*   Report ID (1)                    */

    /* 8 modifier bits */
    0x05, 0x07,                   /*   Usage Page (Keyboard/Keypad)     */
    0x19, 0xE0,                   /*   Usage Min (Keyboard LeftControl) */
    0x29, 0xE7,                   /*   Usage Max (Keyboard Right GUI)   */
    0x15, 0x00,                   /*   Logical Min (0)                  */
    0x25, 0x01,                   /*   Logical Max (1)                  */
    0x75, 0x01,                   /*   Report Size (1)                  */
    0x95, 0x08,                   /*   Report Count (8)                 */
    0x81, 0x02,                   /*   Input (Data,Var,Abs)             */

    /* Reserved byte (boot-layout parity) */
    0x75, 0x08,                   /*   Report Size (8)                  */
    0x95, 0x01,                   /*   Report Count (1)                 */
    0x81, 0x03,                   /*   Input (Const,Var,Abs)            */

    /* 5 LED output bits */
    0x05, 0x08,                   /*   Usage Page (LEDs)                */
    0x19, 0x01,                   /*   Usage Min (Num Lock)             */
    0x29, 0x05,                   /*   Usage Max (Kana)                 */
    0x75, 0x01,                   /*   Report Size (1)                  */
    0x95, 0x05,                   /*   Report Count (5)                 */
    0x91, 0x02,                   /*   Output (Data,Var,Abs)            */

    /* 3 LED padding bits */
    0x75, 0x03,                   /*   Report Size (3)                  */
    0x95, 0x01,                   /*   Report Count (1)                 */
    0x91, 0x03,                   /*   Output (Const,Var,Abs)           */

    /* 6 keycode bytes */
    0x05, 0x07,                   /*   Usage Page (Keyboard/Keypad)     */
    0x19, 0x00,                   /*   Usage Min (0)                    */
    0x29, 0xFF,                   /*   Usage Max (255)                  */
    0x15, 0x00,                   /*   Logical Min (0)                  */
    0x26, 0xFF, 0x00,             /*   Logical Max (255)                */
    0x75, 0x08,                   /*   Report Size (8)                  */
    0x95, 0x06,                   /*   Report Count (6)                 */
    0x81, 0x00,                   /*   Input (Data,Ary,Abs)             */
    0xC0,                         /* End Collection                     */

    /* ---------------------------------------------------------------
     * Mouse top-level application collection (Report IDs 2 and 3)
     * --------------------------------------------------------------- */
    0x05, 0x01,                   /* Usage Page (Generic Desktop)       */
    0x09, 0x02,                   /* Usage (Mouse)                      */
    0xA1, 0x01,                   /* Collection (Application)           */
    0x09, 0x01,                   /*   Usage (Pointer)                  */
    0xA1, 0x00,                   /*   Collection (Physical)            */

    /* ---- Relative mouse (Report ID 2) ---- */
    0x85, 0x02,                   /*     Report ID (2)                  */
    0x05, 0x09,                   /*     Usage Page (Button)            */
    0x19, 0x01,                   /*     Usage Min (Button 1)           */
    0x29, 0x05,                   /*     Usage Max (Button 5)           */
    0x15, 0x00,                   /*     Logical Min (0)                */
    0x25, 0x01,                   /*     Logical Max (1)                */
    0x75, 0x01,                   /*     Report Size (1)                */
    0x95, 0x05,                   /*     Report Count (5)               */
    0x81, 0x02,                   /*     Input (Data,Var,Abs)           */
    0x75, 0x03,                   /*     Report Size (3)                */
    0x95, 0x01,                   /*     Report Count (1)               */
    0x81, 0x03,                   /*     Input (Const,Var,Abs)          */

    0x05, 0x01,                   /*     Usage Page (Generic Desktop)   */
    0x09, 0x30,                   /*     Usage (X)                      */
    0x09, 0x31,                   /*     Usage (Y)                      */
    0x16, 0x01, 0x80,             /*     Logical Min (-32767)           */
    0x26, 0xFF, 0x7F,             /*     Logical Max ( 32767)           */
    0x75, 0x10,                   /*     Report Size (16)               */
    0x95, 0x02,                   /*     Report Count (2)               */
    0x81, 0x06,                   /*     Input (Data,Var,Rel)           */

    0x09, 0x38,                   /*     Usage (Wheel)                  */
    0x15, 0x81,                   /*     Logical Min (-127)             */
    0x25, 0x7F,                   /*     Logical Max ( 127)             */
    0x75, 0x08,                   /*     Report Size (8)                */
    0x95, 0x01,                   /*     Report Count (1)               */
    0x81, 0x06,                   /*     Input (Data,Var,Rel)           */

    0x05, 0x0C,                   /*     Usage Page (Consumer)          */
    0x0A, 0x38, 0x02,             /*     Usage (AC Pan)                 */
    0x15, 0x81,                   /*     Logical Min (-127)             */
    0x25, 0x7F,                   /*     Logical Max ( 127)             */
    0x75, 0x08,                   /*     Report Size (8)                */
    0x95, 0x01,                   /*     Report Count (1)               */
    0x81, 0x06,                   /*     Input (Data,Var,Rel)           */

    /* ---- Absolute mouse (Report ID 3) ---- */
    0x85, 0x03,                   /*     Report ID (3)                  */
    0x05, 0x09,                   /*     Usage Page (Button)            */
    0x19, 0x01,                   /*     Usage Min (Button 1)           */
    0x29, 0x05,                   /*     Usage Max (Button 5)           */
    0x15, 0x00,                   /*     Logical Min (0)                */
    0x25, 0x01,                   /*     Logical Max (1)                */
    0x75, 0x01,                   /*     Report Size (1)                */
    0x95, 0x05,                   /*     Report Count (5)               */
    0x81, 0x02,                   /*     Input (Data,Var,Abs)           */
    0x75, 0x03,                   /*     Report Size (3)                */
    0x95, 0x01,                   /*     Report Count (1)               */
    0x81, 0x03,                   /*     Input (Const,Var,Abs)          */

    0x05, 0x01,                   /*     Usage Page (Generic Desktop)   */
    0x09, 0x30,                   /*     Usage (X)                      */
    0x09, 0x31,                   /*     Usage (Y)                      */
    0x15, 0x00,                   /*     Logical Min (0)                */
    0x26, 0xFF, 0x7F,             /*     Logical Max (32767)            */
    0x75, 0x10,                   /*     Report Size (16)               */
    0x95, 0x02,                   /*     Report Count (2)               */
    0x81, 0x02,                   /*     Input (Data,Var,Abs)           */

    0x09, 0x38,                   /*     Usage (Wheel)                  */
    0x15, 0x81,                   /*     Logical Min (-127)             */
    0x25, 0x7F,                   /*     Logical Max ( 127)             */
    0x75, 0x08,                   /*     Report Size (8)                */
    0x95, 0x01,                   /*     Report Count (1)               */
    0x81, 0x06,                   /*     Input (Data,Var,Rel)           */

    0x05, 0x0C,                   /*     Usage Page (Consumer)          */
    0x0A, 0x38, 0x02,             /*     Usage (AC Pan)                 */
    0x15, 0x81,                   /*     Logical Min (-127)             */
    0x25, 0x7F,                   /*     Logical Max ( 127)             */
    0x75, 0x08,                   /*     Report Size (8)                */
    0x95, 0x01,                   /*     Report Count (1)               */
    0x81, 0x06,                   /*     Input (Data,Var,Rel)           */

    0xC0,                         /*   End Collection (Physical)        */
    0xC0                          /* End Collection (Application)       */
};

/*
 * Compile-time size contract. The bus driver embeds
 * VHID_REPORT_DESCRIPTOR_SIZE in the HID class descriptor's
 * wReportDescriptorLength field; both values must agree.
 */
C_ASSERT(sizeof(g_HidReportDescriptor) == 175);

_Use_decl_annotations_
VOID
VhidkmHidGetClassDescriptor(
    PHID_DESCRIPTOR Descriptor
    )
{
    PAGED_CODE();

    RtlZeroMemory(Descriptor, sizeof(*Descriptor));
    Descriptor->bLength            = sizeof(HID_DESCRIPTOR);
    Descriptor->bDescriptorType    = 0x21; /* HID class descriptor type */
    Descriptor->bcdHID             = 0x0111;
    Descriptor->bCountry           = 0x00;
    Descriptor->bNumDescriptors    = 0x01;
    Descriptor->DescriptorList[0].bReportType = 0x22; /* REPORT */
    Descriptor->DescriptorList[0].wReportLength =
        (USHORT)sizeof(g_HidReportDescriptor);
}

_Use_decl_annotations_
ULONG
VhidkmHidGetReportDescriptorSize(
    VOID
    )
{
    PAGED_CODE();
    return (ULONG)sizeof(g_HidReportDescriptor);
}

_Use_decl_annotations_
VOID
VhidkmHidCopyReportDescriptor(
    PVOID  Buffer,
    ULONG  BufferLength
    )
{
    ULONG toCopy;

    PAGED_CODE();

    toCopy = (BufferLength < sizeof(g_HidReportDescriptor))
             ? BufferLength : (ULONG)sizeof(g_HidReportDescriptor);
    RtlCopyMemory(Buffer, g_HidReportDescriptor, toCopy);
}
