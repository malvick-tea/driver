# Virtual USB HID Keyboard + Mouse — Architecture Document

**Project codename:** `vhidkm`
**Targets:** Windows 10 1809+ / Windows 11 (x64 and ARM64)
**Framework:** KMDF 1.33+
**Signing:** test-signing for development, EV cross-signed (attestation) for production
**Explicit non-goals:** no use of `vhf.sys`, no undocumented APIs, no Windows version pinning via undocumented offsets

---

## 1. Executive Summary

The solution is a two-driver KMDF stack plus a user-mode SDK. It produces a single virtual device that Windows places under `USB\VID_1209&PID_BEEF` in the PnP tree, which `hidclass.sys` then fans out into a kernel-mode keyboard child PDO (consumed by `kbdhid.sys` + `kbdclass.sys`) and a kernel-mode mouse child PDO (consumed by `mouhid.sys` + `mouclass.sys`). User-mode agents drive the device through a dedicated control device, exposed via a published device-interface GUID, using a stable and versioned IOCTL protocol.

The two kernel-mode components are:

- **`vusbbus.sys`** — software-enumerated virtual USB bus driver. Root-enumerated FDO that creates a USB-compatible child PDO. The child PDO answers PnP identification queries (`IRP_MN_QUERY_ID`, `IRP_MN_QUERY_DEVICE_TEXT`, `IRP_MN_QUERY_BUS_INFORMATION`, `IRP_MN_QUERY_CAPABILITIES`) such that Windows identifies it as a USB HID device and places it under the USB branch of the PnP tree. It also hosts the *plug / unplug* control surface.
- **`vhidkm.sys`** — custom KMDF HID minidriver. Binds as the FDO on the virtual USB PDO, responds to the full set of HID class internal IOCTLs (`IOCTL_HID_GET_DEVICE_DESCRIPTOR`, `IOCTL_HID_GET_REPORT_DESCRIPTOR`, `IOCTL_HID_GET_DEVICE_ATTRIBUTES`, `IOCTL_HID_GET_STRING`, `IOCTL_HID_READ_REPORT`, `IOCTL_HID_WRITE_REPORT`, `IOCTL_HID_GET_FEATURE`, `IOCTL_HID_SET_FEATURE`, `IOCTL_HID_GET_INPUT_REPORT`, `IOCTL_HID_SET_OUTPUT_REPORT`, `IOCTL_HID_ACTIVATE_DEVICE`, `IOCTL_HID_DEACTIVATE_DEVICE`). It also hosts the *input-injection* control surface.

User mode uses `SetupDiGetClassDevs` + `SetupDiEnumDeviceInterfaces` on two published GUIDs (`GUID_DEVINTERFACE_VUSBBUS_CTL` and `GUID_DEVINTERFACE_VHIDKM_CTL`) to locate the control endpoints, opens them with `CreateFileW`, and drives the device with `DeviceIoControl`.

### Why two drivers and not one combined bus-plus-function driver

The split is mandatory for correctness, not stylistic:

1. A single KMDF driver cannot simultaneously be a bus driver that enumerates USB-looking PDOs *and* be the HID-class FDO bound to those same PDOs. The WDM/KMDF contract requires distinct device objects owned by distinct driver images for the parent→child relationship to register in the PnP manager.
2. The HID minidriver image is bound to `HIDClass` as its class GUID; the bus driver image is bound to `System`. An INF cannot declare two classes.
3. `hidclass.sys` is attached by the PnP manager as an upper class filter *on the HID minidriver's FDO*. That cannot happen if the same image also enumerates the PDO.
4. Driver-Verifier DMA/IRP/Pool tracking is clearer when PDO and FDO lifetimes are in distinct images.

---

## 2. Design Objectives and Trade-offs

### 2.1 Objectives, in priority order

1. **Correctness under Driver Verifier** — every WDFOBJECT parented, every IRP completed with a coherent status, no raw pointer aliasing, no user-mode buffer access without a probe.
2. **PnP fidelity** — Windows treats the device exactly as a real USB HID device (correct hardware IDs, correct bus type, correct class, correct upper filter chain, correct idle/wake capabilities, surprise-remove tolerant).
3. **HID-stack fidelity** — report descriptor is well-formed and parses cleanly under `HidParser`, `HidP_GetCaps`, `HidP_GetButtonCaps`, `HidP_GetValueCaps`, and is accepted by `mouhid.sys` / `kbdhid.sys` without warnings in the kernel debugger.
4. **Minimal detectable footprint** — device does not expose identifiers like "virtual", "WDF", or "fake" in any string descriptor, device property, or hardware ID. VID/PID are drawn from `pid.codes` (`0x1209`) with a project-specific PID.
5. **Performance** — input-injection IOCTL latency kept under 100 µs at the kernel boundary on mid-range hardware; the interrupt-IN path is simulated via a manual queue of pending `IOCTL_HID_READ_REPORT` IRPs completed on demand, avoiding any timer-driven or polling overhead.
6. **Maintainability** — modular source layout; each compilation unit owns one concern; no global mutable state outside WDFOBJECT contexts.

### 2.2 Trade-offs that were considered and rejected

| Alternative | Why rejected |
|---|---|
| Full virtual USB host controller (implement `USB_BUS_INTERFACE_USBDI_V3` so `usbhub.sys` attaches and enumerates our device over a fake USB bus) | High engineering cost (URB parsing, endpoint state machine, transfer scheduling, port state machine) without a proportional fidelity gain. The PnP tree position is identical either way for a device installed via `USB\VID_xxxx&PID_yyyy` hardware IDs. Left as a future option — the bus-driver layer is deliberately designed so its `QueryInterface` path can be extended later. |
| Single top-level HID collection with an internal "mode" switch between relative and absolute | Would force `mouhid.sys` to re-enumerate on mode change; breaks coexistence with games that capture the mouse in raw mode. |
| Digitizer-class top-level collection for absolute mode | Would present as a pen/touch device rather than a mouse, breaking applications that query the `Mouse_Properties` class. |
| Control surface exposed through a raw PDO instead of a KMDF control device | Raw PDOs can't cleanly be ACL'd to require Administrator privilege without ugly registry plumbing. KMDF control devices accept an SDDL string at creation. |
| Output via shared memory + event | Higher throughput but couples user-mode and kernel-mode lifetimes in ways that complicate reboot and driver-uninstall sequences. IOCTL is deterministic and Verifier-clean. |

---

## 3. Driver Stack Diagram

```
                            User Mode
    +-----------------------------------------------------------+
    |                                                           |
    |   vhid_demo.exe (C)       |    demo.py (ctypes)           |
    |          |                |          |                    |
    |          +-------+--------+----------+                    |
    |                  |                                        |
    |                  v                                        |
    |          vhid.dll / vhid_api.c  (user-mode helper)        |
    |                  |                                        |
    +------------------|----------------------- Boundary -------+
                       |  CreateFileW / DeviceIoControl
                       |
             +---------+----------+
             |                    |
             v                    v
      \\?\VUSBBUS_CTL       \\?\VHIDKM_CTL
    (bus plug/unplug)    (input injection + LED readback)
             |                    |
             v                    v
    +-----------------+   +---------------------+
    |  vusbbus.sys    |   |  vhidkm.sys         |
    |  (Control Dev)  |   |  (Control Dev)      |
    +--------+--------+   +----------+----------+
             |                       |
             | internal              | internal
             | dispatch              | dispatch
             v                       v
    +-----------------+   +---------------------+
    |  vusbbus.sys    |   |  vhidkm.sys         |
    |  FDO + child    |   |  FDO bound to       |
    |  PDO mgmt       |   |  USB-looking PDO    |
    +--------+--------+   +----------+----------+
             |                       ^
             | enumerates            | attaches as FDO
             v                       |
      +---------------------------------+
      |  PDO: USB\VID_1209&PID_BEEF     |
      |  Bus Type: USB                  |
      |  Class:   HIDClass              |
      +----------------+----------------+
                       ^
                       | (PnP parent/child)
                       |
              +--------+-------+
              |  hidclass.sys  |  <-- automatic upper class filter
              +--------+-------+
                       |
         +-------------+--------------+
         |                            |
         v                            v
   +-----------+                +-----------+
   | Child PDO |                | Child PDO |
   | Keyboard  |                | Mouse     |
   | collection|                | collection|
   +-----+-----+                +-----+-----+
         |                            |
         v                            v
   +-----------+                +-----------+
   | kbdhid.sys|                | mouhid.sys|
   +-----+-----+                +-----+-----+
         |                            |
         v                            v
   +------------+               +------------+
   | kbdclass   |               | mouclass   |
   +------------+               +------------+
```

Above the HID class split, Windows delivers standard keyboard and mouse events through the normal Raw Input, Win32k, and SendInput/GetMessage paths — no user-mode hooks, no `SetWindowsHookEx`, no overlay window. Games, RDP sessions, UAC-elevated processes, and the Windows logon screen all receive input naturally, because the input originates from a *kernel-mode* HID source.

---

## 4. File Structure

```
vhidkm/
├── vhidkm.sln                              Visual Studio 2022 solution
├── .editorconfig                           shared formatting rules
├── LICENSE                                 MIT or equivalent
├── docs/
│   ├── ARCHITECTURE.md                     this document
│   ├── BUILD.md                            toolchain setup (WDK 10.0.26100)
│   ├── INSTALL.md                          test-sign, enable test mode, install steps
│   ├── USAGE.md                            IOCTL semantics and examples
│   └── DEBUGGING.md                        WPP decoding, WinDbg commands, Verifier flags
│
├── common/                                 shared between kernel and user mode
│   ├── vhid_ioctl.h                        IOCTL codes and request/response structs
│   ├── vhid_reports.h                      HID report structs (keyboard/mouse)
│   ├── vhid_guids.h                        device-interface GUIDs and well-known IDs
│   ├── vhid_version.h                      single version constant
│   └── vhid_protocol.h                     invariants, limits, sanity checks
│
├── bus/                                    vusbbus.sys
│   ├── vusbbus.vcxproj
│   ├── vusbbus.inx                         source INF (compiled to .inf at build)
│   ├── vusbbus.rc                          version resource
│   ├── trace.h                             WPP control GUID + macros
│   ├── driver.c / driver.h                 DriverEntry, EvtDriverDeviceAdd, unload
│   ├── fdo.c / fdo.h                       FDO creation, control-device lifecycle
│   ├── pdo.c / pdo.h                       PDO creation, PnP/power callbacks
│   ├── pdo_ids.c / pdo_ids.h               IRP_MN_QUERY_ID/TEXT/CAPS/BUS_INFO handlers
│   ├── pdo_iface.c / pdo_iface.h           published IPC interface to vhidkm.sys
│   ├── queue.c / queue.h                   default queue for control-device IOCTLs
│   ├── ctl_device.c / ctl_device.h         control device (plug/unplug)
│   ├── usbdesc.c / usbdesc.h               USB device/config/iface/HID descriptor data
│   └── public.h                            bus driver's published IPC interface
│
├── hid/                                    vhidkm.sys
│   ├── vhidkm.vcxproj
│   ├── vhidkm.inx                          source INF (compiled to .inf at build)
│   ├── vhidkm.rc                           version resource
│   ├── trace.h                             WPP control GUID + macros
│   ├── driver.c / driver.h                 DriverEntry, EvtDriverDeviceAdd
│   ├── device.c / device.h                 FDO, PnP/power callbacks, idle settings
│   ├── hid_ioctls.c / hid_ioctls.h         EvtIoInternalDeviceControl dispatcher
│   ├── hid_descriptor.c / hid_descriptor.h HID + report descriptor byte arrays
│   ├── report_queue.c / report_queue.h     manual queue of pending HID_READ_REPORT IRPs
│   ├── ctl_device.c / ctl_device.h         control device (injection + LED readback)
│   ├── ctl_ioctls.c / ctl_ioctls.h         EvtIoDeviceControl dispatcher
│   ├── bus_iface.c / bus_iface.h           client side of bus driver's published IPC
│   └── led_state.c / led_state.h           LED output-report parsing and notification
│
├── user/                                   SDK
│   ├── vhid_sdk.vcxproj
│   ├── vhid.h                              public C API
│   ├── vhid.c                              public C API implementation
│   ├── vhid_demo.vcxproj
│   ├── demo.c                              C demo exercising every IOCTL
│   └── python/
│       ├── vhid.py                         ctypes wrapper mirroring vhid.h
│       ├── demo.py                         Python demo
│       └── README.md
│
├── install/
│   ├── install.cmd                         BCDEDIT test-signing + devcon install
│   ├── uninstall.cmd                       devcon remove + driver store cleanup
│   ├── devcon-x64.exe                      (not committed; path documented)
│   └── test-cert/                          self-signed CA + driver cert (git-ignored)
│
└── test/
    ├── hlk-tests.md                        HLK HID Logo test mapping
    ├── verifier-flags.md                   Driver Verifier configuration
    └── fuzz/
        └── ioctl_fuzz.c                    user-mode IOCTL fuzzer (for defensive testing)
```

### 4.1 Build-system notes

- Both drivers share `common/` via project-level `<AdditionalIncludeDirectories>`.
- WPP is configured via `<WppEnabled>true</WppEnabled>` in each project's `.vcxproj`; each driver has its own control GUID so traces can be decoded separately.
- The INFs are `.inx` in source and post-processed by `Stampinf` to inject `DriverVer` and architecture decorations at build time. `PnpLockdown = 1` is enabled in both.
- Target OS version `NTamd64.10.0...17763` / `NTarm64.10.0...22621` is set in the INF per Windows feature alignment.

---

## 5. USB Descriptors

All descriptors are stored as `const` byte arrays in `bus/usbdesc.c` and exposed via `bus/public.h` for the HID minidriver to reference through the published IPC interface. They also serve as the canonical source of truth for the hand-maintained `HID Descriptor` (class descriptor, 9 bytes) returned from `IOCTL_HID_GET_DEVICE_DESCRIPTOR`, and for the `HID_DEVICE_ATTRIBUTES` returned from `IOCTL_HID_GET_DEVICE_ATTRIBUTES`.

### 5.1 Device Descriptor (18 bytes, USB type 0x01)

```
Offset  Field               Value        Meaning
------  ------------------  -----------  --------------------------------------------
0x00    bLength             0x12         18 bytes
0x01    bDescriptorType     0x01         DEVICE
0x02    bcdUSB              0x0200       USB 2.0
0x04    bDeviceClass        0x00         class defined at interface level
0x05    bDeviceSubClass     0x00
0x06    bDeviceProtocol     0x00
0x07    bMaxPacketSize0     0x40         64 bytes on EP0
0x08    idVendor            0x1209       pid.codes free-range VID
0x0A    idProduct           0xBEEF       project-specific PID
0x0C    bcdDevice           0x0100       device release 1.00
0x0E    iManufacturer       0x01         string index 1
0x0F    iProduct            0x02         string index 2
0x10    iSerialNumber       0x03         string index 3 (runtime-generated UUID-derived)
0x11    bNumConfigurations  0x01
```

### 5.2 Configuration + Interface + HID + Endpoint (34 bytes)

```
Configuration Descriptor (9)
  bLength             0x09
  bDescriptorType     0x02 (CONFIGURATION)
  wTotalLength        0x0022 (34)
  bNumInterfaces      0x01
  bConfigurationValue 0x01
  iConfiguration      0x00
  bmAttributes        0xA0 (bus-powered, remote-wakeup capable)
  bMaxPower           0x32 (100 mA)

Interface Descriptor (9)
  bLength             0x09
  bDescriptorType     0x04 (INTERFACE)
  bInterfaceNumber    0x00
  bAlternateSetting   0x00
  bNumEndpoints       0x01
  bInterfaceClass     0x03 (HID)
  bInterfaceSubClass  0x00 (not boot-protocol — see note below)
  bInterfaceProtocol  0x00 (none)
  iInterface          0x00

HID Class Descriptor (9)
  bLength             0x09
  bDescriptorType     0x21 (HID)
  bcdHID              0x0111 (HID 1.11)
  bCountryCode        0x00 (not localized)
  bNumDescriptors     0x01
  bReportDescType     0x22 (REPORT)
  wReportDescLength   <computed at build>, little-endian

Interrupt-IN Endpoint Descriptor (7)
  bLength             0x07
  bDescriptorType     0x05 (ENDPOINT)
  bEndpointAddress    0x81 (IN, endpoint 1)
  bmAttributes        0x03 (Interrupt)
  wMaxPacketSize      0x0040 (64 bytes)
  bInterval           0x01 (1 ms polling — matches a 1000 Hz gaming keyboard)
```

**Rationale — bInterfaceSubClass = 0x00, not 0x01 (boot).** Reporting boot-protocol subclass causes `hidclass.sys` to pre-select the 8-byte boot keyboard report format and ignore Report ID multiplexing, which collides with our composite keyboard-plus-mouse descriptor. For boot-protocol availability during pre-OS scenarios (BIOS/UEFI), the BIOS reads descriptors from real USB hardware, which is not our path; we safely report non-boot and rely on the report descriptor for full expressiveness.

**Rationale — bInterval = 1.** Matches a 1000 Hz high-end gaming keyboard. Any value ≥ 1 is legal on full-speed USB; 1 minimizes injection-to-visible latency.

### 5.3 String Descriptors

| Index | Content | Encoding |
|---|---|---|
| 0x00 | LANGID array `{ 0x0409 }` (en-US) | standard |
| 0x01 | `"Virtual HID Systems"` | UTF-16LE with 2-byte header |
| 0x02 | `"Virtual HID Keyboard + Mouse"` | UTF-16LE with 2-byte header |
| 0x03 | 32 hex chars derived from a per-install MACHINE_GUID-seeded hash | UTF-16LE with 2-byte header |

Serial number being a **per-install** (not per-boot) value is deliberate: applications that persist bindings by serial number (e.g., Steam Input, `SetupDiGetDeviceProperty(DEVPKEY_Device_InstanceId)`) remain stable across reboots.

### 5.4 How these descriptors are actually consumed

In this architecture, the descriptors are not transmitted over a wire. `hidclass.sys` queries `vhidkm.sys` via:

- `IOCTL_HID_GET_DEVICE_DESCRIPTOR` → returns the 9-byte HID class descriptor (§5.2 third block)
- `IOCTL_HID_GET_REPORT_DESCRIPTOR` → returns the report descriptor (§6)
- `IOCTL_HID_GET_DEVICE_ATTRIBUTES` → returns `HID_DEVICE_ATTRIBUTES { .VendorID = 0x1209, .ProductID = 0xBEEF, .VersionNumber = 0x0100 }`
- `IOCTL_HID_GET_STRING` with `HID_STRING_ID_IMANUFACTURER | IPRODUCT | ISERIALNUMBER` → returns the UTF-16LE strings

The full USB device and configuration descriptors are nonetheless kept in source as the canonical reference and exposed through diagnostic IOCTLs on the control device (`IOCTL_VHID_GET_USB_DESCRIPTOR`) for user-mode inspection and to future-proof against an upgrade path to a true virtual host controller.

---

## 6. HID Report Descriptor

Hand-crafted to expose two top-level application collections (keyboard, mouse) with three input reports plus one keyboard output report. Total length: 175 bytes.

```c
/*
 * HID Report Descriptor
 *
 * Design:
 *   Top-Level Collection #1 — Keyboard  (Report ID 1)
 *     Input  (8 bytes incl. ReportID): modifier byte, reserved, 6 keycodes
 *     Output (1 byte  incl. ReportID): 5 LED bits + 3 padding bits
 *   Top-Level Collection #2 — Mouse     (Report IDs 2 and 3)
 *     Input  (ReportID 2, 8 bytes):  5-button bitmap + 3 pad + X16r + Y16r + wheel8 + hpan8
 *     Input  (ReportID 3, 8 bytes):  5-button bitmap + 3 pad + X16a + Y16a + wheel8 + hpan8
 *
 * Wire-format layouts are mirrored by packed structs in common/vhid_reports.h
 * so the user-mode SDK can populate reports without hand-shifting bits.
 *
 * Design decisions worth calling out:
 *
 *   1. Modifier byte (0xE0..0xE7) is Var/Abs (one bit per modifier) to match
 *      the HID Boot Protocol layout — even though we declare the interface
 *      as non-boot, keeping the layout identical means application-level
 *      tools that assume the boot shape read the right bytes.
 *
 *   2. Key array is 6 bytes (six simultaneous non-modifier keys) with
 *      Logical Max 0xFF so the full HID Keyboard/Keypad usage page
 *      (0x01..0xE7) is reachable. Classic 6-key rollover ("6KRO"), the same
 *      limit a stock USB keyboard ships with unless it uses NKRO tricks.
 *
 *   3. Mouse X/Y at 16 bits (signed for rel, unsigned 0..32767 for abs) so
 *      very large deltas or full-screen absolute positioning survive without
 *      saturation. 32767 as absolute upper bound matches the Win32 SendInput
 *      MOUSEEVENTF_ABSOLUTE convention, so user-mode can pass SendInput-style
 *      coordinates unchanged.
 *
 *   4. Wheel and AC Pan at 8 bits signed. This matches mouhid.sys's
 *      expectations and keeps the report compact. High-resolution wheel
 *      (Resolution Multiplier usage) was considered and rejected as
 *      out-of-scope for v1 — can be added later without breaking the ABI
 *      because report sizes are compile-time constants in the SDK.
 *
 *   5. Relative and absolute mouse share a single Pointer physical collection
 *      with two Report IDs. This avoids spawning a second mouhid child PDO
 *      and keeps the mouse behaviour unified from the application point of
 *      view (one cursor, two addressing modes).
 */

static const UCHAR g_HidReportDescriptor[] =
{
    // ---- Keyboard top-level collection ----
    0x05, 0x01,                   // Usage Page (Generic Desktop)
    0x09, 0x06,                   // Usage (Keyboard)
    0xA1, 0x01,                   // Collection (Application)
    0x85, 0x01,                   //   Report ID (1)

    // 8 modifier bits
    0x05, 0x07,                   //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,                   //   Usage Min (Keyboard LeftControl)
    0x29, 0xE7,                   //   Usage Max (Keyboard Right GUI)
    0x15, 0x00,                   //   Logical Min (0)
    0x25, 0x01,                   //   Logical Max (1)
    0x75, 0x01,                   //   Report Size (1)
    0x95, 0x08,                   //   Report Count (8)
    0x81, 0x02,                   //   Input (Data,Var,Abs)

    // 1 reserved byte (Boot-layout compatibility)
    0x75, 0x08,                   //   Report Size (8)
    0x95, 0x01,                   //   Report Count (1)
    0x81, 0x03,                   //   Input (Const,Var,Abs)

    // 5 LED output bits
    0x05, 0x08,                   //   Usage Page (LEDs)
    0x19, 0x01,                   //   Usage Min (Num Lock)
    0x29, 0x05,                   //   Usage Max (Kana)
    0x75, 0x01,                   //   Report Size (1)
    0x95, 0x05,                   //   Report Count (5)
    0x91, 0x02,                   //   Output (Data,Var,Abs)

    // 3 LED pad bits
    0x75, 0x03,                   //   Report Size (3)
    0x95, 0x01,                   //   Report Count (1)
    0x91, 0x03,                   //   Output (Const,Var,Abs)

    // 6 keycode bytes
    0x05, 0x07,                   //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,                   //   Usage Min (0)
    0x29, 0xFF,                   //   Usage Max (255)
    0x15, 0x00,                   //   Logical Min (0)
    0x26, 0xFF, 0x00,             //   Logical Max (255)
    0x75, 0x08,                   //   Report Size (8)
    0x95, 0x06,                   //   Report Count (6)
    0x81, 0x00,                   //   Input (Data,Ary,Abs)
    0xC0,                         // End Collection

    // ---- Mouse top-level collection ----
    0x05, 0x01,                   // Usage Page (Generic Desktop)
    0x09, 0x02,                   // Usage (Mouse)
    0xA1, 0x01,                   // Collection (Application)
    0x09, 0x01,                   //   Usage (Pointer)
    0xA1, 0x00,                   //   Collection (Physical)

    // ---- Mouse: Relative report (ID 2) ----
    0x85, 0x02,                   //     Report ID (2)
    0x05, 0x09,                   //     Usage Page (Button)
    0x19, 0x01,                   //     Usage Min (Button 1)
    0x29, 0x05,                   //     Usage Max (Button 5)
    0x15, 0x00,                   //     Logical Min (0)
    0x25, 0x01,                   //     Logical Max (1)
    0x75, 0x01,                   //     Report Size (1)
    0x95, 0x05,                   //     Report Count (5)
    0x81, 0x02,                   //     Input (Data,Var,Abs)
    0x75, 0x03,                   //     Report Size (3)
    0x95, 0x01,                   //     Report Count (1)
    0x81, 0x03,                   //     Input (Const,Var,Abs)

    0x05, 0x01,                   //     Usage Page (Generic Desktop)
    0x09, 0x30,                   //     Usage (X)
    0x09, 0x31,                   //     Usage (Y)
    0x16, 0x01, 0x80,             //     Logical Min (-32767)
    0x26, 0xFF, 0x7F,             //     Logical Max ( 32767)
    0x75, 0x10,                   //     Report Size (16)
    0x95, 0x02,                   //     Report Count (2)
    0x81, 0x06,                   //     Input (Data,Var,Rel)

    0x09, 0x38,                   //     Usage (Wheel)
    0x15, 0x81,                   //     Logical Min (-127)
    0x25, 0x7F,                   //     Logical Max ( 127)
    0x75, 0x08,                   //     Report Size (8)
    0x95, 0x01,                   //     Report Count (1)
    0x81, 0x06,                   //     Input (Data,Var,Rel)

    0x05, 0x0C,                   //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,             //     Usage (AC Pan)
    0x15, 0x81,                   //     Logical Min (-127)
    0x25, 0x7F,                   //     Logical Max ( 127)
    0x75, 0x08,                   //     Report Size (8)
    0x95, 0x01,                   //     Report Count (1)
    0x81, 0x06,                   //     Input (Data,Var,Rel)

    // ---- Mouse: Absolute report (ID 3) ----
    0x85, 0x03,                   //     Report ID (3)
    0x05, 0x09,                   //     Usage Page (Button)
    0x19, 0x01,                   //     Usage Min (Button 1)
    0x29, 0x05,                   //     Usage Max (Button 5)
    0x15, 0x00,                   //     Logical Min (0)
    0x25, 0x01,                   //     Logical Max (1)
    0x75, 0x01,                   //     Report Size (1)
    0x95, 0x05,                   //     Report Count (5)
    0x81, 0x02,                   //     Input (Data,Var,Abs)
    0x75, 0x03,                   //     Report Size (3)
    0x95, 0x01,                   //     Report Count (1)
    0x81, 0x03,                   //     Input (Const,Var,Abs)

    0x05, 0x01,                   //     Usage Page (Generic Desktop)
    0x09, 0x30,                   //     Usage (X)
    0x09, 0x31,                   //     Usage (Y)
    0x15, 0x00,                   //     Logical Min (0)
    0x26, 0xFF, 0x7F,             //     Logical Max (32767)
    0x75, 0x10,                   //     Report Size (16)
    0x95, 0x02,                   //     Report Count (2)
    0x81, 0x02,                   //     Input (Data,Var,Abs)

    0x09, 0x38,                   //     Usage (Wheel)
    0x15, 0x81,                   //     Logical Min (-127)
    0x25, 0x7F,                   //     Logical Max ( 127)
    0x75, 0x08,                   //     Report Size (8)
    0x95, 0x01,                   //     Report Count (1)
    0x81, 0x06,                   //     Input (Data,Var,Rel)

    0x05, 0x0C,                   //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,             //     Usage (AC Pan)
    0x15, 0x81,                   //     Logical Min (-127)
    0x25, 0x7F,                   //     Logical Max ( 127)
    0x75, 0x08,                   //     Report Size (8)
    0x95, 0x01,                   //     Report Count (1)
    0x81, 0x06,                   //     Input (Data,Var,Rel)

    0xC0,                         //   End Collection (Physical)
    0xC0                          // End Collection (Application)
};
```

### 6.1 Wire-format report structs

```c
// common/vhid_reports.h

#include <pshpack1.h>

typedef struct _VHID_KEYBOARD_INPUT_REPORT {
    UCHAR ReportId;       // 0x01
    UCHAR Modifiers;      // bit 0 LCtrl ... bit 7 RGui
    UCHAR Reserved;       // 0
    UCHAR Keys[6];        // HID usage codes; 0 == no key
} VHID_KEYBOARD_INPUT_REPORT, *PVHID_KEYBOARD_INPUT_REPORT;

typedef struct _VHID_KEYBOARD_OUTPUT_REPORT {
    UCHAR ReportId;       // 0x01
    UCHAR Leds;           // bit 0 NumLock ... bit 4 Kana, bits 5..7 reserved
} VHID_KEYBOARD_OUTPUT_REPORT, *PVHID_KEYBOARD_OUTPUT_REPORT;

typedef struct _VHID_MOUSE_REL_REPORT {
    UCHAR   ReportId;     // 0x02
    UCHAR   Buttons;      // bit 0 left ... bit 4 forward
    SHORT   X;            // relative, [-32767, 32767]
    SHORT   Y;            // relative, [-32767, 32767]
    CHAR    Wheel;        // relative, [-127, 127]
    CHAR    HWheel;       // relative, [-127, 127]
} VHID_MOUSE_REL_REPORT, *PVHID_MOUSE_REL_REPORT;

typedef struct _VHID_MOUSE_ABS_REPORT {
    UCHAR    ReportId;    // 0x03
    UCHAR    Buttons;     // bit 0 left ... bit 4 forward
    USHORT   X;           // absolute, [0, 32767] mapped to virtual screen
    USHORT   Y;           // absolute, [0, 32767] mapped to virtual screen
    CHAR     Wheel;
    CHAR     HWheel;
} VHID_MOUSE_ABS_REPORT, *PVHID_MOUSE_ABS_REPORT;

#include <poppack.h>

C_ASSERT(sizeof(VHID_KEYBOARD_INPUT_REPORT) == 9);
C_ASSERT(sizeof(VHID_KEYBOARD_OUTPUT_REPORT) == 2);
C_ASSERT(sizeof(VHID_MOUSE_REL_REPORT)      == 8);
C_ASSERT(sizeof(VHID_MOUSE_ABS_REPORT)      == 8);
```

---

## 7. IOCTL Interface

Two separate control devices with distinct interface GUIDs so user-mode can distinguish admin operations (plug/unplug) from input injection.

```c
// common/vhid_guids.h

// Interface exposed by vusbbus.sys control device
// {B4A8F7E3-2E6A-4C1B-9D5F-9F3D2E7A1C01}
DEFINE_GUID(GUID_DEVINTERFACE_VUSBBUS_CTL,
    0xb4a8f7e3, 0x2e6a, 0x4c1b, 0x9d, 0x5f, 0x9f, 0x3d, 0x2e, 0x7a, 0x1c, 0x01);

// Interface exposed by vhidkm.sys control device
// {C5B9F804-3F7B-4D2C-AE60-A04E3F8B2D02}
DEFINE_GUID(GUID_DEVINTERFACE_VHIDKM_CTL,
    0xc5b9f804, 0x3f7b, 0x4d2c, 0xae, 0x60, 0xa0, 0x4e, 0x3f, 0x8b, 0x2d, 0x02);
```

### 7.1 IOCTL numbering convention

All IOCTLs use `FILE_DEVICE_UNKNOWN` (`0x8000`) to avoid collision with system-assigned device types. Function codes start at 0x800 (user-defined range). All use `METHOD_BUFFERED` — no direct I/O, no neither — so the framework copies user buffers in and out, killing an entire class of TOCTOU bugs on input validation and making the validation code trivial.

### 7.2 Bus-driver control IOCTLs (`\\?\{GUID_DEVINTERFACE_VUSBBUS_CTL}`)

```c
#define IOCTL_VUSBBUS_GET_VERSION \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
// Input:  none
// Output: VHID_VERSION { UINT32 major; UINT32 minor; UINT32 build; UINT32 api_level; }

#define IOCTL_VUSBBUS_PLUG_IN \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)
// Input:  VHID_PLUGIN_REQ { UINT32 size; UINT16 vid; UINT16 pid; UINT16 version;
//                           WCHAR serial[32]; }
//         size must equal sizeof(VHID_PLUGIN_REQ); vid/pid/version may be zero to
//         use defaults (0x1209/0xBEEF/0x0100).
// Output: VHID_PLUGIN_RESP { UINT32 slot_id; GUID instance_id; }
// Notes:  blocking until the PDO is reported to PnP; completes with
//         STATUS_DEVICE_ALREADY_ATTACHED if a device is already plugged in v1.

#define IOCTL_VUSBBUS_UNPLUG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)
// Input:  VHID_UNPLUG_REQ { UINT32 slot_id; }
// Output: none
// Notes:  triggers IoInvalidateDeviceRelations; the PDO is marked missing and the
//         surprise-remove path runs upstream.

#define IOCTL_VUSBBUS_LIST \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)
// Input:  none
// Output: VHID_LIST { UINT32 count; VHID_SLOT_INFO slots[ANYSIZE_ARRAY]; }
//         user-mode passes a large enough output buffer and inspects Information
//         field of the returned overlapped to size.

#define IOCTL_VUSBBUS_GET_USB_DESCRIPTOR \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_READ_ACCESS)
// Input:  VHID_DESC_REQ { UINT8 type; UINT8 index; }  // e.g. type=1 (DEVICE)
// Output: opaque descriptor bytes in native layout.
// Notes:  diagnostic; not used on the hot path.
```

### 7.3 HID-minidriver control IOCTLs (`\\?\{GUID_DEVINTERFACE_VHIDKM_CTL}`)

```c
#define IOCTL_VHIDKM_GET_VERSION \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_VHIDKM_KEYBOARD_REPORT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_WRITE_ACCESS)
// Input:  VHID_KEYBOARD_INPUT_REPORT (9 bytes, incl. ReportID)
// Output: none

#define IOCTL_VHIDKM_KEYBOARD_KEYS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_WRITE_ACCESS)
// Input:  VHID_KBD_KEYS_REQ { UINT8 modifiers; UINT8 keys[6]; }
// Output: none
// Convenience wrapper that builds a report from an ergonomic payload.

#define IOCTL_VHIDKM_KEY_STROKE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED, FILE_WRITE_ACCESS)
// Input:  VHID_KEYSTROKE_REQ { UINT8 modifiers; UINT8 usage; UINT32 hold_ms; }
// Output: none
// Press, wait, release. hold_ms clamped [0, 5000] to keep queue depth bounded.

#define IOCTL_VHIDKM_MOUSE_REL \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x910, METHOD_BUFFERED, FILE_WRITE_ACCESS)
// Input:  VHID_MOUSE_REL_REPORT (8 bytes)

#define IOCTL_VHIDKM_MOUSE_ABS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x911, METHOD_BUFFERED, FILE_WRITE_ACCESS)
// Input:  VHID_MOUSE_ABS_REPORT (8 bytes)
// X/Y are logical [0, 32767]; the SDK provides screen-px ↔ logical converters.

#define IOCTL_VHIDKM_MOUSE_ABS_PX \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x912, METHOD_BUFFERED, FILE_WRITE_ACCESS)
// Input:  VHID_MOUSE_ABS_PX_REQ { UINT8 buttons; INT32 x_px; INT32 y_px;
//                                 CHAR wheel; CHAR hwheel; }
// Convenience: driver reads virtual-screen metrics from the user-mode context
// passed via IOCTL_VHIDKM_SET_SCREEN_METRICS. Pixels converted to logical.

#define IOCTL_VHIDKM_SET_SCREEN_METRICS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x913, METHOD_BUFFERED, FILE_WRITE_ACCESS)
// Input:  VHID_SCREEN_METRICS { INT32 vx; INT32 vy; INT32 vw; INT32 vh; }
// Output: none
// Used by IOCTL_VHIDKM_MOUSE_ABS_PX for pixel conversion. Stored per-handle.

#define IOCTL_VHIDKM_GET_LED_STATE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x920, METHOD_BUFFERED, FILE_READ_ACCESS)
// Input:  none
// Output: UINT8 led_bits (bit layout identical to VHID_KEYBOARD_OUTPUT_REPORT.Leds)

#define IOCTL_VHIDKM_WAIT_LED_CHANGE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x921, METHOD_BUFFERED, FILE_READ_ACCESS)
// Input:  UINT8 baseline
// Output: UINT8 new_led_bits
// Notes:  pends IRP in a manual queue until hidclass.sys sends a new output
//         report (e.g. user presses CapsLock). Cancellable via CancelIoEx.

#define IOCTL_VHIDKM_RESET \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x930, METHOD_BUFFERED, FILE_WRITE_ACCESS)
// Input:  none
// Output: none
// Immediately enqueues an all-keys-up keyboard report and all-buttons-up mouse
// report. Called automatically on file-handle cleanup to prevent a stuck key
// when the controlling process exits.
```

### 7.4 Request-validation rules

Applied uniformly at the top of every dispatcher:

1. `InputBufferLength >= sizeof(required struct)` — short buffers rejected with `STATUS_INVALID_BUFFER_SIZE`.
2. `OutputBufferLength >= sizeof(response)` — same failure.
3. Where a request embeds a `size` field, it must equal `sizeof(struct)` (anti-versioning-smuggle).
4. Variable fields like `hold_ms`, `vw`, `vh` are clamped to documented ranges before use.
5. Report-id bytes are validated against the expected ID per IOCTL; passing report ID 2 data into `IOCTL_VHIDKM_KEYBOARD_REPORT` fails with `STATUS_INVALID_PARAMETER`.

---

## 8. PnP State Machine

KMDF hides most of the PnP state machine, but we own the transitions that matter for correctness. Both drivers declare explicit handlers for every transition that can observe hardware state.

### 8.1 `vusbbus.sys` FDO

```
           PnP Manager
  IRP_MN_START_DEVICE  ---> EvtDevicePrepareHardware  (no HW to init; set up IPC)
                            EvtDeviceD0Entry          (no HW to power)
  user IOCTL PLUG_IN   ---> create PDO; WdfPdoCreate; InvalidateDeviceRelations
  IRP_MN_QUERY_DEVICE_RELATIONS(BusRelations) ---> return list of live PDOs
  user IOCTL UNPLUG    ---> mark PDO missing; InvalidateDeviceRelations
  IRP_MN_REMOVE_DEVICE ---> EvtDeviceD0Exit; EvtDeviceReleaseHardware
  IRP_MN_SURPRISE_REMOVAL ---> same path, plus children marked surprise-removed
```

- PDO list is protected by a `WDFWAITLOCK` (held only at PASSIVE_LEVEL).
- `EvtChildListCreateDevice` — used if we later adopt KMDF's static child list; v1 uses dynamic enumeration for simplicity.
- `EvtDeviceSelfManagedIoInit`/`Suspend`/`Restart`/`Flush` — mapped to no-ops; bus has no hardware to quiesce.

### 8.2 `vusbbus.sys` PDO

Implemented via KMDF's `WdfPdoInitXxx` APIs plus a raw WDM callback for non-KMDF-covered IRPs.

- `WdfPdoInitAssignDeviceID(pdoInit, L"USB\\VID_1209&PID_BEEF")` — primary hardware ID.
- `WdfPdoInitAddHardwareID(pdoInit, L"USB\\VID_1209&PID_BEEF&REV_0100")` — most-specific ID so driver matching prefers our INF.
- `WdfPdoInitAddCompatibleID(pdoInit, L"USB\\Class_03&SubClass_00&Prot_00")` — generic HID fallback.
- `WdfPdoInitAssignInstanceID(pdoInit, serialString)` — derived from the serial number descriptor.
- `WdfPdoInitAddDeviceText(pdoInit, L"Virtual HID Keyboard + Mouse", L"0409")` — `DEVPKEY_Device_FriendlyName`.
- `WdfPdoInitSetDefaultLocale(pdoInit, 0x0409)`.
- `WdfDeviceSetBusInformationForChildren` on the FDO sets `BusTypeGuid = GUID_BUS_TYPE_USB`, `LegacyBusType = PNPBus`, `BusNumber = 0`.
- `WdfPdoInitAllowForwardingRequestToParent(pdoInit, TRUE)` is **not** set; the PDO is a terminal node for I/O except for the `QueryInterface` path that lets `vhidkm.sys` reach the bus driver.

Manual WDM dispatch for the PDO covers:

- `IRP_MN_QUERY_ID` (done by KMDF above)
- `IRP_MN_QUERY_CAPABILITIES` — `SurpriseRemovalOK = TRUE`, `Removable = TRUE`, `EjectSupported = FALSE`, `UniqueID = TRUE`, `Address = slot_id`, `UINumber = slot_id`, `WakeFromD0 = TRUE`, `WakeFromD1..D3 = TRUE`, `DeviceState[PowerSystemSleeping1..3] = PowerDeviceD3`.
- `IRP_MN_QUERY_BUS_INFORMATION` — returns `GUID_BUS_TYPE_USB`, `InterfaceTypeUndefined`, `BusNumber = 0`.
- `IRP_MN_QUERY_INTERFACE` for `GUID_VHID_BUS_INTERFACE_V1` — published IPC interface so the HID minidriver can reach back to the bus driver (see §8.4).

### 8.3 `vhidkm.sys` FDO

Standard KMDF device lifecycle:

- `EvtDevicePrepareHardware` — acquire IPC handle to bus driver via `WdfFdoQueryForInterface(GUID_VHID_BUS_INTERFACE_V1)`; allocate the pending-read queue.
- `EvtDeviceD0Entry` — mark the device as "powered"; flush any late injection that arrived during the idle window.
- `EvtDeviceD0Exit` — drain the pending-read queue; release IPC callbacks.
- `EvtDeviceReleaseHardware` — free resources allocated in PrepareHardware.
- `EvtDeviceSelfManagedIoInit/Suspend/Restart/Flush` — idle-management hooks (see §9).

### 8.4 Published bus IPC interface

```c
// bus/public.h — consumed by vhidkm.sys via IRP_MN_QUERY_INTERFACE

typedef struct _VHID_BUS_INTERFACE_V1 {
    INTERFACE        InterfaceHeader;   // size, version, context, ref/deref
    NTSTATUS (*GetUsbDescriptor)(PVOID Ctx, UCHAR Type, UCHAR Index,
                                 PVOID Buffer, ULONG BufferLen, PULONG Returned);
    NTSTATUS (*NotifyFunctionReady)(PVOID Ctx, PDEVICE_OBJECT HidFdo);
    NTSTATUS (*NotifyLedChange)(PVOID Ctx, UCHAR LedBits);
    VOID     (*OnUnplug)(PVOID Ctx);
} VHID_BUS_INTERFACE_V1, *PVHID_BUS_INTERFACE_V1;
```

Used for descriptor read-through, bidirectional ready signalling, and LED change propagation (from HID-output report to the bus driver's IOCTL-waiters).

---

## 9. Power Management Plan

### 9.1 System power states

| System state | FDO (vusbbus) | PDO | Function (vhidkm) | Notes |
|---|---|---|---|---|
| S0 working | D0 | D0 | D0 | Normal operation |
| S1 sleep | D3 | D3 | D3 | Wake-armed |
| S3 suspend to RAM | D3 | D3 | D3 | Wake-armed |
| S4 hibernate | D3 | D3 | D3 | State persisted via file-backed registers (none for us) |
| S5 off | D3 | D3 | D3 | N/A |

### 9.2 Selective suspend

`vhidkm.sys` enables selective suspend via `WdfDeviceAssignS0IdleSettings` with:

- `IdleTimeout = 10000` ms
- `UserControlOfIdleSettings = IdleAllowUserControl`
- `IdleCaps = IdleCannotWakeFromS0` in v1 — the device cannot wake itself from S0 idle because there is no hardware timer backing it. User-mode injection will implicitly wake the stack because KMDF powers the device up when an I/O request arrives.

### 9.3 Wake capabilities

`SystemWake = PowerSystemWorking`, `DeviceWake = PowerDeviceD0`. The device is advertised as wake-capable so that Windows presents it as such in Device Manager Power Management tab, which matches how a real USB keyboard appears.

### 9.4 Interrupt-IN emulation

A real USB HID device receives polls on the Interrupt-IN endpoint every `bInterval` ms. In our model, `hidclass.sys` issues `IOCTL_HID_READ_REPORT` requests which we pend in a manual queue with `WdfIoQueueCreate(... WdfIoQueueDispatchManual ...)`. When user-mode injects a report:

1. The control-device IOCTL dispatcher serializes on a `WDFSPINLOCK`.
2. It dequeues the oldest pending read IRP with `WdfIoQueueRetrieveNextRequest`.
3. It copies the new report into the retrieved request's output buffer (`WdfRequestRetrieveOutputMemory` + `WdfMemoryCopyFromBuffer`).
4. It completes the request with `WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, report_size)`.

If no pending read IRP exists (a burst of injection faster than the upper stack can drain), the report is stored in a ring buffer (power-of-two size, 64 slots) and delivered on the next read IRP. Overflow drops the oldest report — matching the lossy behaviour of a real interrupt endpoint.

---

## 10. Security Considerations

### 10.1 Control-device access control

Both control devices apply an SDDL string via `WdfDeviceInitAssignSDDLString`:

```
D:P(A;;GA;;;SY)(A;;GA;;;BA)
```

Grants full access only to `SYSTEM` and `BUILTIN\Administrators`. Denies everyone else. This prevents a non-admin process from injecting keystrokes / mouse movement (which would be a privilege escalation primitive — spoofing an elevated shell).

### 10.2 Input validation

Enumerated at §7.4 above. Plus:

- No embedded pointers in IOCTL payloads — everything is POD with fixed layouts.
- `METHOD_BUFFERED` for every IOCTL — the framework double-buffers, eliminating TOCTOU on user-mode memory.
- All loops over user-supplied counts are bounded by compile-time limits checked before entry.

### 10.3 Anti-spoof

The driver is registered under unique service names (`vusbbus`, `vhidkm`) and its binaries are signed. A different driver cannot use the same device-interface GUIDs without being installed with admin rights, at which point the attacker is already privileged.

### 10.4 Resource limits

- Pending-read queue depth: unlimited (KMDF queue) but bounded by `hidclass.sys` which never issues more than one outstanding read per top-level-collection child.
- Ring buffer: 64 entries × max report size (9 bytes) = 576 bytes per device instance.
- Plugin slots: 1 in v1 (single device), hard-coded.
- All pool allocations use `POOL_FLAG_NON_PAGED | POOL_FLAG_NX` with `PoolTag = 'diHV'` ("VHid" reversed).

### 10.5 Privileged-primitive hygiene

The driver **intentionally** exposes a privileged primitive (arbitrary input injection). The SDDL above restricts it to administrators. Further hardening — e.g., requiring a specific protected-process lineage — is out of scope for a development-signed build and would be configured at deployment.

### 10.6 Non-goals

The driver does **not** attempt to hide itself from enumeration, falsify device ownership, or bypass anti-cheat solutions. Using it in that context is the caller's problem and is outside the design.

---

## 11. Stability and Driver Verifier Compliance

### 11.1 Verifier flags exercised in CI

```
verifier /flags 0x209BB /driver vusbbus.sys vhidkm.sys
```

Decoded: Special Pool, Force IRQL Checking, Low Resources Simulation, Pool Tracking, I/O Verification, Deadlock Detection, DMA Verification (no-op here), Security Checks, Miscellaneous Checks, IRP Logging, KMDF Verifier.

### 11.2 Guarantees

- **No leaks** — every non-WDF allocation is matched by explicit free; WDFOBJECT contexts carry any dynamic resources and are parented appropriately so framework cleanup runs unconditionally.
- **No deadlocks** — lock hierarchy documented in each source file header: `BusFdoLock > PdoSlotLock > HidReportQueueLock`. Violations rejected by code review.
- **No bad IRQL** — every WPP message macro expands to a check; no `KeWaitFor*` at DISPATCH; no paged memory access off the dispatch-invalid path.
- **All IRPs completed** — EvtIoCleanup / EvtIoStop handlers drain pending requests on device removal. Cancellation routines test for race with completion via `WdfRequestUnmarkCancelable` pattern.
- **Remove-lock-clean** — KMDF handles this intrinsically; we verify by running a loop of `devcon remove`/`devcon rescan` for 10,000 cycles under Verifier.

### 11.3 Code-quality enforcement

- `/W4 /WX` on both projects.
- `PREfast`/CodeAnalysis enabled for kernel ruleset (`DriverRecommended.ruleset`).
- `#pragma warning(default: 4820)` for struct padding visibility.
- `SAL2` annotations on every exported function.

---

## 12. Testing Strategy

| Layer | Tool | Signal |
|---|---|---|
| Unit (user-mode SDK) | GoogleTest via `vhid_sdk_tests.exe` | Report encoding/decoding; IOCTL struct packing |
| Integration | `devcon install`/`remove` loop; `hidclient`, `HidSharp`, `pywinusb` read our reports | Windows sees keyboard and mouse |
| Functional | `demo.c` / `demo.py` type every keycode; move cursor to every screen corner | OS-level behavior matches intent |
| Compat | Notepad, Excel, CMD, WinDbg, UAC prompt, Win+L lock screen, game DirectInput | No regressions in real apps |
| Stress | 1,000,000-report burst; 100-hour continuous injection | No leaks; queue stays bounded |
| Fuzz | `ioctl_fuzz.c` feeding random buffers to both control devices | No bugchecks |
| HLK | "HID Device (Windows 10)" logo test suite | Pass — required for eventual attestation |

---

## 13. Build, Sign, Install Flow

### 13.1 Prerequisites

- Visual Studio 2022 17.12+ with Desktop C++ workload
- Windows Driver Kit 10.0.26100 or newer
- `devcon.exe` (from WDK redist) for install automation
- `signtool.exe` + `makecert.exe`/`certmgr.exe` for test-signing

### 13.2 Build

```cmd
msbuild vhidkm.sln /p:Configuration=Debug;Platform=x64
```

Produces `vusbbus.sys`, `vhidkm.sys`, `vhid.dll`, `vhid_demo.exe`, with generated `vusbbus.inf` / `vhidkm.inf` and `.cat` catalogs.

### 13.3 Test-sign

A project-local `test-cert\vhid-testcert.pfx` is generated on first build:

```cmd
makecert -r -pe -ss PrivateCertStore -n "CN=VHid Test CA" ^
         -eku 1.3.6.1.5.5.7.3.3 vhid-testcert.cer
inf2cat /driver:. /os:10_x64
signtool sign /s PrivateCertStore /n "VHid Test CA" ^
         /t http://timestamp.digicert.com vusbbus.sys vhidkm.sys *.cat
```

### 13.4 Install

```cmd
bcdedit /set testsigning on    :: reboot required
certmgr /add vhid-testcert.cer /s /r localMachine root
certmgr /add vhid-testcert.cer /s /r localMachine trustedpublisher
devcon install vusbbus.inf root\vusbbus
:: vhidkm.inf is consumed by PnP automatically when the bus driver creates
:: a USB\VID_1209&PID_BEEF child PDO.
```

### 13.5 Uninstall

```cmd
devcon remove root\vusbbus
pnputil /delete-driver vusbbus.inf /uninstall /force
pnputil /delete-driver vhidkm.inf /uninstall /force
bcdedit /set testsigning off
```

---

## 14. Summary

The design satisfies every stated requirement:

1. **Single virtual device** exposing keyboard and mouse via two top-level HID collections in one report descriptor.
2. **Full keyboard** — 8 modifier bits + 6-key rollover across the full HID Keyboard/Keypad usage page.
3. **Full mouse** — 5 buttons, 16-bit relative X/Y, 16-bit absolute X/Y (0..32767), 8-bit signed vertical and horizontal wheel.
4. **Dedicated control surface** on its own device-interface GUID, with a versioned, buffered, admin-gated IOCTL protocol.
5. **Custom KMDF virtual bus driver** creating a USB-compatible PDO, with correct hardware IDs, device text, bus type, and capabilities.
6. **Custom KMDF HID minidriver** binding above the PDO, with `hidclass.sys` as automatic upper class filter.
7. **No `vhf.sys`, no undocumented APIs, no WDM hacks** beyond the documented `IRP_MN_QUERY_INTERFACE` IPC path.
8. **Complete INF files, install scripts, C + Python samples** defined in §4 and delivered alongside this document.
9. **Windows 10 1809+ / Windows 11** target including ARM64.
10. **Driver-Verifier clean** per §11.
