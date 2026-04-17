# Using vhidkm

This document describes the user-mode contract: the IOCTL protocol
on the two control devices, the C SDK, and the Python wrapper.
Every example compiles and runs against the driver exactly as shown.

---

## 1. Overview

```
          SDK (user/vhid.h)
                 |
                 |  opens two handles
                 v
    +---------------------+        +---------------------+
    | \\?\VUSBBUS_CTL     |        | \\?\VHIDKM_CTL      |
    | plug / unplug / list|        | keyboard / mouse    |
    |                     |        | LED state, screen   |
    |                     |        | metrics, reset      |
    +---------------------+        +---------------------+
           |                                 |
           |  IOCTL_VUSBBUS_*                |  IOCTL_VHIDKM_*
           v                                 v
```

The two control devices live behind two device-interface GUIDs:

- `GUID_DEVINTERFACE_VUSBBUS_CTL` — exposed by `vusbbus.sys`.
- `GUID_DEVINTERFACE_VHIDKM_CTL` — exposed by `vhidkm.sys`.

Both GUIDs and every IOCTL code are defined in `common/vhid_ioctl.h`.
The SDL on each control device is
`D:P(A;;GA;;;SY)(A;;GA;;;BA)`, so only `SYSTEM` and
`BUILTIN\Administrators` can open handles.

---

## 2. Startup sequence

A process that wants to inject input must follow this order:

1. **Discover and open the bus control device.**
   Enumerate device interfaces with
   `SetupDiGetClassDevs(GUID_DEVINTERFACE_VUSBBUS_CTL, ...)`, take the
   first one, and open it with `CreateFileW`.

2. **Version handshake.**
   Issue `IOCTL_VUSBBUS_GET_VERSION`. The returned `VHID_VERSION.ApiLevel`
   must equal `VHID_API_LEVEL` (currently `1`). If it does not, fail
   loudly — the driver on this machine speaks a different ABI.

3. **Plug in the virtual device.**
   Issue `IOCTL_VUSBBUS_PLUG_IN` with a `VHID_PLUGIN_REQ`. Zero
   `Vid`/`Pid`/`Version` fields get the defaults
   (`VHID_DEFAULT_VID=0x1209`, `VHID_DEFAULT_PID=0xBEEF`,
   `VHID_DEFAULT_REV=0x0100`). `Serial` is copied verbatim (no
   null terminator required, up to 32 `WCHAR`s).

4. **Wait for the HID control interface.**
   PnP takes a short time to materialise the HID child stack after
   `PLUG_IN` completes. Use `CM_Register_Notification` with
   `CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE` filtered on
   `GUID_DEVINTERFACE_VHIDKM_CTL`, or poll
   `SetupDiGetClassDevs` every 50 ms until an interface appears.
   The SDK caps the wait at the caller-supplied timeout (default
   5000 ms).

5. **Open the HID control device and version-handshake.**
   Same pattern as step 1–2 but on `GUID_DEVINTERFACE_VHIDKM_CTL`
   and `IOCTL_VHIDKM_GET_VERSION`.

6. **(Optional) Set screen metrics** with
   `IOCTL_VHIDKM_SET_SCREEN_METRICS` if you intend to use the
   pixel-coordinate mouse path (`IOCTL_VHIDKM_MOUSE_ABS_PX`).

After that point, inject reports at will.

### Shutdown

1. Close the function-device handle. The driver's cleanup callback
   enqueues an all-up keyboard + all-up mouse reset so any held keys
   are released even if the caller crashed.
2. Issue `IOCTL_VUSBBUS_UNPLUG` with the `SlotId` returned by the
   plug-in call.
3. Close the bus-device handle.

---

## 3. IOCTL reference

All IOCTLs are `METHOD_BUFFERED`, `FILE_DEVICE_UNKNOWN`. The access
column is one of `R` (`FILE_READ_ACCESS`) or `W` (`FILE_WRITE_ACCESS`).
Sizes are in bytes; the `Size` field at the head of every request
struct must equal `sizeof(<struct>)`, a check enforced at the kernel
boundary.

### 3.1 `vusbbus.sys` control (function codes `0x800`..`0x8FF`)

| IOCTL | Acc | Input | Output | Purpose |
|---|---|---|---|---|
| `IOCTL_VUSBBUS_GET_VERSION` | R | - | `VHID_VERSION` (16) | Version handshake. Called before any other IOCTL. |
| `IOCTL_VUSBBUS_PLUG_IN` | W | `VHID_PLUGIN_REQ` | `VHID_PLUGIN_RESP` | Creates the virtual USB child PDO. Returns `SlotId` + `InstanceId`. |
| `IOCTL_VUSBBUS_UNPLUG` | W | `VHID_UNPLUG_REQ` | - | Marks the slot's PDO missing and tears the stack down. |
| `IOCTL_VUSBBUS_LIST` | R | - | `VHID_SLOT_LIST` (variable) | Enumerates live slots; `STATUS_BUFFER_TOO_SMALL` with required size on short buffer. |
| `IOCTL_VUSBBUS_GET_USB_DESCRIPTOR` | R | `VHID_DESC_REQ` | descriptor bytes | Diagnostic read of the canonical USB descriptors. |

### 3.2 `vhidkm.sys` control (function codes `0x900`..`0x9FF`)

| IOCTL | Acc | Input | Output | Purpose |
|---|---|---|---|---|
| `IOCTL_VHIDKM_GET_VERSION` | R | - | `VHID_VERSION` | Version handshake for the function device. |
| `IOCTL_VHIDKM_KEYBOARD_REPORT` | W | `VHID_KEYBOARD_INPUT_REPORT` (9) | - | Raw-report injection. `ReportId` must equal `VHID_REPORTID_KEYBOARD_INPUT`. |
| `IOCTL_VHIDKM_KEYBOARD_KEYS` | W | `VHID_KBD_KEYS_REQ` (12) | - | Modifier bitmap + 6 usages. Driver builds the report. |
| `IOCTL_VHIDKM_KEY_STROKE` | W | `VHID_KEYSTROKE_REQ` (12) | - | Press, hold `HoldMs` ms, release. `HoldMs` clamped `[0, 5000]`. |
| `IOCTL_VHIDKM_MOUSE_REL` | W | `VHID_MOUSE_REL_REPORT` (8) | - | Raw relative mouse injection. |
| `IOCTL_VHIDKM_MOUSE_ABS` | W | `VHID_MOUSE_ABS_REPORT` (8) | - | Raw absolute mouse injection (`X`, `Y` in `0..32767`). |
| `IOCTL_VHIDKM_MOUSE_ABS_PX` | W | `VHID_MOUSE_ABS_PX_REQ` (16) | - | Pixel-coordinate absolute mouse. Requires screen metrics set on this handle. |
| `IOCTL_VHIDKM_SET_SCREEN_METRICS` | W | `VHID_SCREEN_METRICS` (20) | - | Stores the virtual-screen rectangle in the per-file context. |
| `IOCTL_VHIDKM_GET_LED_STATE` | R | - | `UINT8` | Last LED byte observed from `hidclass.sys`. |
| `IOCTL_VHIDKM_WAIT_LED_CHANGE` | R | `UINT8` baseline | `UINT8` new state | Blocks (pending IRP, cancellable) until LED state changes. |
| `IOCTL_VHIDKM_RESET` | W | - | - | Queues all-up keyboard + all-up mouse report. |

### 3.3 Status codes

| NTSTATUS | When |
|---|---|
| `STATUS_SUCCESS` | Normal completion. |
| `STATUS_INVALID_PARAMETER` | `ReportId` mismatched, mouse buttons with reserved bits set, `HoldMs` out of range before clamp (soft), `Serial` too long. |
| `STATUS_INVALID_BUFFER_SIZE` | `InputBufferLength` / `OutputBufferLength` does not match the IOCTL's fixed-size contract, or request struct `Size` field does not equal `sizeof(<struct>)`. |
| `STATUS_BUFFER_TOO_SMALL` | `IOCTL_VUSBBUS_LIST` output buffer smaller than required. `Information` field reports the required size. |
| `STATUS_DEVICE_ALREADY_ATTACHED` | `IOCTL_VUSBBUS_PLUG_IN` issued while a slot is already live (v1 is single-slot). |
| `STATUS_NOT_FOUND` | `IOCTL_VUSBBUS_UNPLUG` with unknown `SlotId`. |
| `STATUS_INVALID_DEVICE_STATE` | `IOCTL_VHIDKM_MOUSE_ABS_PX` before `SET_SCREEN_METRICS`. |
| `STATUS_CANCELLED` | LED-wait cancelled by `CancelIoEx` / handle close. |

---

## 4. C SDK quick-start (`user/vhid.h`)

Minimum viable program:

```c
#include <stdio.h>
#include "vhid.h"

int main(void)
{
    vhid_ctx *ctx = NULL;
    DWORD rc = vhid_open(/*vid*/0, /*pid*/0, /*ver*/0,
                         /*serial*/NULL,
                         /*plugin_timeout_ms*/5000,
                         &ctx);
    if (rc) { fprintf(stderr, "vhid_open failed 0x%08lx\n", rc); return 1; }

    /* Press Shift+H to type a capital H, then release. */
    const uint8_t H_usage = 0x0B;  /* HID Usage Page 0x07, Keyboard h */
    uint8_t keys[VHID_KBD_MAX_KEYS] = { H_usage };
    (void)vhid_keyboard(ctx, VHID_KBD_MOD_LSHIFT, keys);
    Sleep(25);
    memset(keys, 0, sizeof(keys));
    (void)vhid_keyboard(ctx, 0, keys);

    /* Nudge the mouse 20 px to the right. */
    (void)vhid_mouse_rel(ctx, /*buttons*/0, /*dx*/20, /*dy*/0, /*wheel*/0, /*hwheel*/0);

    vhid_close(ctx);
    return 0;
}
```

Compile against `vhid.lib` (produced by `user/vhid_sdk.vcxproj`).
Link dependencies: `setupapi.lib`, `cfgmgr32.lib`, `kernel32.lib`,
`advapi32.lib`.

### 4.1 Higher-level helpers

- `vhid_keystroke(ctx, mods, usage, hold_ms)` — queues a press, the
  driver's timer releases after `hold_ms`. Returns immediately.
- `vhid_mouse_abs(ctx, buttons, x, y, wheel, hwheel)` — logical
  coordinates `0..32767`.
- `vhid_mouse_abs_px(ctx, buttons, x_px, y_px, wheel, hwheel)` —
  pixel coordinates. First call must be preceded by
  `vhid_set_screen_metrics_auto(ctx)`.
- `vhid_get_led_state(ctx, &byte)` — non-blocking.
- `vhid_wait_led_change(ctx, baseline, timeout_ms, &byte)` —
  blocks on overlapped I/O; returns `ERROR_TIMEOUT` on timeout.

### 4.2 Error handling

Every SDK entry point returns a `DWORD`:

- `0` means success.
- Non-zero is either a `ERROR_*` from `GetLastError()` (for failures
  in underlying Win32 calls such as `CreateFile`, `DeviceIoControl`,
  `SetupDi*`) or a raw `NTSTATUS` from the driver. A call to
  `FAILED(<status>) || (<status> >= 0xC0000000)` distinguishes the
  two cases. The SDK does not clobber `GetLastError()` on failure,
  so callers that prefer to consult it may.

---

## 5. Python SDK quick-start (`user/python/vhid.py`)

`vhid.py` is a `ctypes` mirror of the C SDK with the same surface.
Import it from anywhere; it auto-loads `vhid.dll` (or falls back to
direct IOCTLs against `vhid.sys`'s control device if no DLL is
present).

```python
import vhid

with vhid.Session(plugin_timeout_ms=5000) as s:
    s.keystroke(modifiers=vhid.KBD_MOD_LSHIFT, usage=0x0B, hold_ms=25)  # Shift+H
    s.mouse_rel(dx=20, dy=0)
    s.mouse_abs_px(x_px=960, y_px=540)           # requires screen metrics
    print("LEDs:", bin(s.get_led_state()))
```

`Session` is a context manager that maps to `vhid_open` /
`vhid_close`. Python 3.9+ is required (typing annotations use PEP
585 built-in generics).

---

## 6. Advanced topics

### 6.1 Running under a non-admin process

The control-device SDDL refuses opens from anyone outside `SYSTEM`
or `Administrators`. If your agent must run in a user session, the
common pattern is a privileged helper service that owns the handles
and exposes a named pipe with a tighter ACL to the user-mode
process. That arrangement is orthogonal to this driver and is not
shipped here.

### 6.2 Multiple concurrent processes

Two user-mode processes can each open the HID control device
independently. Each handle has its own file-object context, so
per-handle state (screen metrics, LED-wait baselines) is isolated.
Report injection is serialised by the KMDF default queue under the
hood, so interleaved writes from two processes are well-defined
(each request is atomic from `hidclass.sys`'s point of view).

However, the bus driver is single-slot in v1 — only one virtual
device exists regardless of how many processes have handles open.
The slot lifetime is bound to whichever handle issued the
`PLUG_IN`. Closing that handle triggers an implicit `UNPLUG` on
the slot.

### 6.3 LED feedback loop

A common use case is driving a status LED on the host based on
`NumLock` state from the OS. Example:

```c
uint8_t baseline = 0;
(void)vhid_get_led_state(ctx, &baseline);
for (;;) {
    uint8_t new_state = 0;
    DWORD rc = vhid_wait_led_change(ctx, baseline, INFINITE, &new_state);
    if (rc) break;
    react_to_led_change(baseline, new_state);
    baseline = new_state;
}
```

Cancellation: close the HID handle or call `CancelIoEx`; the
pending IOCTL completes with `ERROR_OPERATION_ABORTED`.

### 6.4 Report size limits

`VHID_MAX_REPORT_SIZE` is 9 bytes — the keyboard input report. The
IOCTL dispatcher rejects inputs larger than this at the kernel
boundary for the raw-report paths (`KEYBOARD_REPORT`, `MOUSE_REL`,
`MOUSE_ABS`). The structured paths (`KEYBOARD_KEYS`, `KEY_STROKE`,
`MOUSE_ABS_PX`) validate their own request struct sizes.

### 6.5 Reboot and power transitions

The drivers are **selective-suspend capable** (configurable idle
timeout; default 10 s). A system suspend-resume cycle will cause
`hidclass.sys` to tear down and re-open the child stack; pending
`WAIT_LED_CHANGE` IRPs are cancelled with `STATUS_POWER_STATE_INVALID`
and the client must re-issue them. User-mode handles survive the
transition as long as the device-interface remains present.

Full kernel-mode crashdump capture across suspend-resume is handled
by the Verifier flags called out in `docs/DEBUGGING.md`.
