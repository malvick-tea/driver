"""
user/python/vhid.py

Pure-ctypes Python wrapper around the VHID user-mode control surface.
No external dependencies (no pywin32, no cffi) — imports only stdlib
modules so that a freshly built driver can be driven from any Python
3.8+ installation on Windows.

The module mirrors the C SDK in user/vhid.h. Every DeviceIoControl
invocation goes through a single private helper (_ioctl_sync) which
handles the OVERLAPPED dance and surfaces GetLastError as an
OSError with the Win32 errno set correctly. Public entry points live
on the Vhid class; a context-manager protocol (__enter__/__exit__)
mirrors the vhid_open / vhid_close pair from the C SDK.

Numeric constants are imported by hand rather than generated from the
C headers so the module stands alone. The constants are checked
against the C headers in the unit-test suite; any drift there is a
build failure.

Usage:

    from vhid import Vhid, KbdMod, MouseBtn

    with Vhid.open() as v:
        v.set_screen_metrics_auto()
        v.mouse_abs_px(0, 960, 540)
        v.keystroke(KbdMod.LSHIFT, 0x0B, 60)     # Shift + H
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import enum
import uuid
from contextlib import contextmanager
from typing import Iterable, Optional, Tuple


# ---------------------------------------------------------------------------
# Win32 bindings
# ---------------------------------------------------------------------------

kernel32   = ctypes.WinDLL("kernel32",  use_last_error=True)
setupapi   = ctypes.WinDLL("setupapi",  use_last_error=True)
cfgmgr32   = ctypes.WinDLL("cfgmgr32",  use_last_error=True)
user32     = ctypes.WinDLL("user32",    use_last_error=True)

INVALID_HANDLE_VALUE = wt.HANDLE(-1).value
GENERIC_READ         = 0x80000000
GENERIC_WRITE        = 0x40000000
FILE_SHARE_READ      = 0x00000001
FILE_SHARE_WRITE     = 0x00000002
OPEN_EXISTING        = 3
FILE_FLAG_OVERLAPPED = 0x40000000
FILE_ATTRIBUTE_NORMAL = 0x80
INFINITE             = 0xFFFFFFFF
WAIT_OBJECT_0        = 0
WAIT_TIMEOUT         = 0x00000102
ERROR_IO_PENDING     = 997
ERROR_TIMEOUT        = 1460
ERROR_INSUFFICIENT_BUFFER = 122

DIGCF_PRESENT          = 0x00000002
DIGCF_DEVICEINTERFACE  = 0x00000010


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wt.DWORD),
        ("Data2", wt.WORD),
        ("Data3", wt.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]

    @classmethod
    def from_uuid(cls, s: str) -> "GUID":
        u = uuid.UUID(s)
        g = cls()
        g.Data1 = u.fields[0]
        g.Data2 = u.fields[1]
        g.Data3 = u.fields[2]
        rest = u.bytes[-8:]
        g.Data4 = (ctypes.c_ubyte * 8)(*rest)
        return g

    def to_uuid(self) -> uuid.UUID:
        return uuid.UUID(fields=(
            self.Data1, self.Data2, self.Data3,
            self.Data4[0], self.Data4[1],
            int.from_bytes(bytes(self.Data4[2:8]), "big"),
        ))


class OVERLAPPED(ctypes.Structure):
    _fields_ = [
        ("Internal",      ctypes.c_void_p),
        ("InternalHigh",  ctypes.c_void_p),
        ("Offset",        wt.DWORD),
        ("OffsetHigh",    wt.DWORD),
        ("hEvent",        wt.HANDLE),
    ]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [
        ("cbSize",             wt.DWORD),
        ("InterfaceClassGuid", GUID),
        ("Flags",              wt.DWORD),
        ("Reserved",           ctypes.c_void_p),
    ]


class SP_DEVICE_INTERFACE_DETAIL_DATA_W(ctypes.Structure):
    _fields_ = [
        ("cbSize",     wt.DWORD),
        ("DevicePath", wt.WCHAR * 1),
    ]


kernel32.CreateFileW.restype  = wt.HANDLE
kernel32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD,
                                  ctypes.c_void_p, wt.DWORD, wt.DWORD,
                                  wt.HANDLE]

kernel32.CloseHandle.restype  = wt.BOOL
kernel32.CloseHandle.argtypes = [wt.HANDLE]

kernel32.CreateEventW.restype  = wt.HANDLE
kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wt.BOOL, wt.BOOL,
                                   wt.LPCWSTR]

kernel32.WaitForSingleObject.restype  = wt.DWORD
kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]

kernel32.CancelIoEx.restype  = wt.BOOL
kernel32.CancelIoEx.argtypes = [wt.HANDLE, ctypes.POINTER(OVERLAPPED)]

kernel32.DeviceIoControl.restype  = wt.BOOL
kernel32.DeviceIoControl.argtypes = [wt.HANDLE, wt.DWORD,
                                      ctypes.c_void_p, wt.DWORD,
                                      ctypes.c_void_p, wt.DWORD,
                                      ctypes.POINTER(wt.DWORD),
                                      ctypes.POINTER(OVERLAPPED)]

kernel32.GetOverlappedResult.restype  = wt.BOOL
kernel32.GetOverlappedResult.argtypes = [wt.HANDLE,
                                          ctypes.POINTER(OVERLAPPED),
                                          ctypes.POINTER(wt.DWORD),
                                          wt.BOOL]

kernel32.Sleep.argtypes = [wt.DWORD]

setupapi.SetupDiGetClassDevsW.restype  = wt.HANDLE
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wt.LPCWSTR,
                                           wt.HWND, wt.DWORD]

setupapi.SetupDiEnumDeviceInterfaces.restype  = wt.BOOL
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
    wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(GUID), wt.DWORD,
    ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]

setupapi.SetupDiGetDeviceInterfaceDetailW.restype  = wt.BOOL
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
    wt.HANDLE, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
    ctypes.POINTER(SP_DEVICE_INTERFACE_DETAIL_DATA_W), wt.DWORD,
    ctypes.POINTER(wt.DWORD), ctypes.c_void_p]

setupapi.SetupDiDestroyDeviceInfoList.restype  = wt.BOOL
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wt.HANDLE]

user32.GetSystemMetrics.restype  = ctypes.c_int
user32.GetSystemMetrics.argtypes = [ctypes.c_int]

SM_XVIRTUALSCREEN   = 76
SM_YVIRTUALSCREEN   = 77
SM_CXVIRTUALSCREEN  = 78
SM_CYVIRTUALSCREEN  = 79


# ---------------------------------------------------------------------------
# Protocol constants (mirror of common/vhid_*.h)
# ---------------------------------------------------------------------------

VHID_API_LEVEL        = 1
VHID_DEFAULT_VID      = 0x1209
VHID_DEFAULT_PID      = 0xBEEF
VHID_DEFAULT_REV      = 0x0100

VHID_KBD_MAX_KEYS     = 6
VHID_MOUSE_ABS_MAX    = 32767
VHID_MAX_REPORT_SIZE  = 9

VHID_REPORTID_KEYBOARD_INPUT = 0x01
VHID_REPORTID_MOUSE_REL      = 0x02
VHID_REPORTID_MOUSE_ABS      = 0x03


class KbdMod(enum.IntFlag):
    LCTRL  = 0x01
    LSHIFT = 0x02
    LALT   = 0x04
    LGUI   = 0x08
    RCTRL  = 0x10
    RSHIFT = 0x20
    RALT   = 0x40
    RGUI   = 0x80


class MouseBtn(enum.IntFlag):
    LEFT    = 0x01
    RIGHT   = 0x02
    MIDDLE  = 0x04
    BACK    = 0x08
    FORWARD = 0x10


class KbdLed(enum.IntFlag):
    NUM     = 0x01
    CAPS    = 0x02
    SCROLL  = 0x04
    COMPOSE = 0x08
    KANA    = 0x10


GUID_DEVINTERFACE_VUSBBUS_CTL = GUID.from_uuid(
    "{B4A8F7E3-2E6A-4C1B-9D5F-9F3D2E7A1C01}")
GUID_DEVINTERFACE_VHIDKM_CTL  = GUID.from_uuid(
    "{C5B9F804-3F7B-4D2C-AE60-A04E3F8B2D02}")


def CTL_CODE(device_type: int, function: int,
             method: int = 0, access: int = 0x0001) -> int:
    """METHOD_BUFFERED = 0. FILE_READ_ACCESS = 0x0001.
    FILE_WRITE_ACCESS = 0x0002."""
    return (device_type << 16) | (access << 14) | (function << 2) | method


FILE_DEVICE_UNKNOWN = 0x00000022
METHOD_BUFFERED     = 0
FILE_READ_ACCESS    = 0x0001
FILE_WRITE_ACCESS   = 0x0002

IOCTL_VUSBBUS_GET_VERSION = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800,
                                      METHOD_BUFFERED, FILE_READ_ACCESS)
IOCTL_VUSBBUS_PLUG_IN     = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801,
                                      METHOD_BUFFERED, FILE_WRITE_ACCESS)
IOCTL_VUSBBUS_UNPLUG      = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802,
                                      METHOD_BUFFERED, FILE_WRITE_ACCESS)
IOCTL_VUSBBUS_LIST        = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803,
                                      METHOD_BUFFERED, FILE_READ_ACCESS)

IOCTL_VHIDKM_GET_VERSION        = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900,
                                            METHOD_BUFFERED, FILE_READ_ACCESS)
IOCTL_VHIDKM_KEYBOARD_REPORT    = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901,
                                            METHOD_BUFFERED, FILE_WRITE_ACCESS)
IOCTL_VHIDKM_KEYBOARD_KEYS      = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902,
                                            METHOD_BUFFERED, FILE_WRITE_ACCESS)
IOCTL_VHIDKM_KEY_STROKE         = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903,
                                            METHOD_BUFFERED, FILE_WRITE_ACCESS)
IOCTL_VHIDKM_MOUSE_REL          = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x910,
                                            METHOD_BUFFERED, FILE_WRITE_ACCESS)
IOCTL_VHIDKM_MOUSE_ABS          = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x911,
                                            METHOD_BUFFERED, FILE_WRITE_ACCESS)
IOCTL_VHIDKM_MOUSE_ABS_PX       = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x912,
                                            METHOD_BUFFERED, FILE_WRITE_ACCESS)
IOCTL_VHIDKM_SET_SCREEN_METRICS = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x913,
                                            METHOD_BUFFERED, FILE_WRITE_ACCESS)
IOCTL_VHIDKM_GET_LED_STATE      = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x920,
                                            METHOD_BUFFERED, FILE_READ_ACCESS)
IOCTL_VHIDKM_WAIT_LED_CHANGE    = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x921,
                                            METHOD_BUFFERED, FILE_READ_ACCESS)
IOCTL_VHIDKM_RESET              = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x930,
                                            METHOD_BUFFERED, FILE_WRITE_ACCESS)


# ---------------------------------------------------------------------------
# Wire-format structs (1-byte packed — HID reports have no padding)
# ---------------------------------------------------------------------------

class _Packed(ctypes.Structure):
    _pack_ = 1


class VHID_VERSION(ctypes.Structure):
    _fields_ = [
        ("Major",    ctypes.c_uint32),
        ("Minor",    ctypes.c_uint32),
        ("Build",    ctypes.c_uint32),
        ("ApiLevel", ctypes.c_uint32),
    ]


class VHID_PLUGIN_REQ(_Packed):
    _fields_ = [
        ("Size",     ctypes.c_uint32),
        ("Vid",      ctypes.c_uint16),
        ("Pid",      ctypes.c_uint16),
        ("Version",  ctypes.c_uint16),
        ("Reserved", ctypes.c_uint16),
        ("Serial",   ctypes.c_wchar * 32),
    ]


class VHID_PLUGIN_RESP(_Packed):
    _fields_ = [
        ("SlotId",     ctypes.c_uint32),
        ("InstanceId", GUID),
    ]


class VHID_UNPLUG_REQ(_Packed):
    _fields_ = [
        ("Size",   ctypes.c_uint32),
        ("SlotId", ctypes.c_uint32),
    ]


class VHID_SLOT_INFO(_Packed):
    _fields_ = [
        ("SlotId",     ctypes.c_uint32),
        ("Vid",        ctypes.c_uint16),
        ("Pid",        ctypes.c_uint16),
        ("Version",    ctypes.c_uint16),
        ("Reserved",   ctypes.c_uint16),
        ("InstanceId", GUID),
        ("Serial",     ctypes.c_wchar * 32),
    ]


class VHID_KBD_KEYS_REQ(_Packed):
    _fields_ = [
        ("Size",      ctypes.c_uint32),
        ("Modifiers", ctypes.c_uint8),
        ("Reserved",  ctypes.c_uint8),
        ("Keys",      ctypes.c_uint8 * VHID_KBD_MAX_KEYS),
    ]


class VHID_KEYSTROKE_REQ(_Packed):
    _fields_ = [
        ("Size",      ctypes.c_uint32),
        ("Modifiers", ctypes.c_uint8),
        ("Usage",     ctypes.c_uint8),
        ("Reserved",  ctypes.c_uint16),
        ("HoldMs",    ctypes.c_uint32),
    ]


class VHID_MOUSE_REL_REPORT(_Packed):
    _fields_ = [
        ("ReportId", ctypes.c_uint8),
        ("Buttons",  ctypes.c_uint8),
        ("X",        ctypes.c_int16),
        ("Y",        ctypes.c_int16),
        ("Wheel",    ctypes.c_int8),
        ("HWheel",   ctypes.c_int8),
    ]


class VHID_MOUSE_ABS_REPORT(_Packed):
    _fields_ = [
        ("ReportId", ctypes.c_uint8),
        ("Buttons",  ctypes.c_uint8),
        ("X",        ctypes.c_uint16),
        ("Y",        ctypes.c_uint16),
        ("Wheel",    ctypes.c_int8),
        ("HWheel",   ctypes.c_int8),
    ]


class VHID_MOUSE_ABS_PX_REQ(_Packed):
    _fields_ = [
        ("Size",     ctypes.c_uint32),
        ("Buttons",  ctypes.c_uint8),
        ("Wheel",    ctypes.c_int8),
        ("HWheel",   ctypes.c_int8),
        ("Reserved", ctypes.c_uint8),
        ("XPx",      ctypes.c_int32),
        ("YPx",      ctypes.c_int32),
    ]


class VHID_SCREEN_METRICS(_Packed):
    _fields_ = [
        ("Size",          ctypes.c_uint32),
        ("VirtualX",      ctypes.c_int32),
        ("VirtualY",      ctypes.c_int32),
        ("VirtualWidth",  ctypes.c_int32),
        ("VirtualHeight", ctypes.c_int32),
    ]


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class VhidError(OSError):
    """Raised when a driver IOCTL or a Win32 discovery call fails."""


def _raise_last(op: str) -> None:
    err = ctypes.get_last_error()
    raise VhidError(err, f"{op} failed (Win32 error {err})")


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _find_interface_path(iface: GUID) -> Optional[str]:
    handle = setupapi.SetupDiGetClassDevsW(
        ctypes.byref(iface), None, None,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if handle == INVALID_HANDLE_VALUE:
        return None

    try:
        iface_data = SP_DEVICE_INTERFACE_DATA()
        iface_data.cbSize = ctypes.sizeof(iface_data)
        if not setupapi.SetupDiEnumDeviceInterfaces(
                handle, None, ctypes.byref(iface), 0,
                ctypes.byref(iface_data)):
            return None

        required = wt.DWORD(0)
        setupapi.SetupDiGetDeviceInterfaceDetailW(
            handle, ctypes.byref(iface_data), None, 0,
            ctypes.byref(required), None)
        if required.value == 0:
            return None

        buf = ctypes.create_string_buffer(required.value)
        detail = ctypes.cast(
            buf, ctypes.POINTER(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
        # The cbSize field refers to the header only, NOT the allocation.
        detail.contents.cbSize = ctypes.sizeof(
            SP_DEVICE_INTERFACE_DETAIL_DATA_W)

        if not setupapi.SetupDiGetDeviceInterfaceDetailW(
                handle, ctypes.byref(iface_data), detail,
                required.value, None, None):
            return None

        # DevicePath follows the 4-byte cbSize field in the buffer.
        path_offset = ctypes.sizeof(wt.DWORD)
        raw = ctypes.wstring_at(
            ctypes.cast(detail, ctypes.c_void_p).value + path_offset)
        return raw
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(handle)


def _wait_for_interface(iface: GUID, timeout_ms: int) -> Optional[str]:
    elapsed = 0
    step = 50
    while True:
        p = _find_interface_path(iface)
        if p:
            return p
        if elapsed >= timeout_ms:
            return None
        kernel32.Sleep(step)
        elapsed += step


def _open_path(path: str) -> int:
    h = kernel32.CreateFileW(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        None, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        None)
    if h == 0 or h == INVALID_HANDLE_VALUE:
        _raise_last(f"CreateFileW({path!r})")
    return h


def _ioctl_sync(handle: int, code: int,
                in_buf: Optional[bytes] = None,
                out_size: int = 0) -> bytes:
    ev = kernel32.CreateEventW(None, True, False, None)
    if not ev:
        _raise_last("CreateEventW")

    try:
        ov = OVERLAPPED()
        ov.hEvent = ev

        in_ptr  = (ctypes.c_char * len(in_buf)).from_buffer_copy(in_buf) \
                  if in_buf else None
        in_len  = len(in_buf) if in_buf else 0
        out_buf = ctypes.create_string_buffer(out_size) if out_size else None
        br      = wt.DWORD(0)

        ok = kernel32.DeviceIoControl(
            handle, code,
            ctypes.cast(in_ptr, ctypes.c_void_p) if in_ptr else None,
            in_len,
            ctypes.cast(out_buf, ctypes.c_void_p) if out_buf else None,
            out_size,
            ctypes.byref(br),
            ctypes.byref(ov))

        if not ok:
            err = ctypes.get_last_error()
            if err == ERROR_IO_PENDING:
                if not kernel32.GetOverlappedResult(
                        handle, ctypes.byref(ov), ctypes.byref(br), True):
                    _raise_last("GetOverlappedResult")
            else:
                raise VhidError(err,
                                f"DeviceIoControl(0x{code:08x}) failed "
                                f"(Win32 error {err})")

        return bytes(out_buf.raw[:br.value]) if out_buf else b""
    finally:
        kernel32.CloseHandle(ev)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

class Vhid:
    """High-level handle into the VHID stack. Use Vhid.open() as a
    context manager for automatic teardown."""

    def __init__(self) -> None:
        self._bus      : Optional[int] = None
        self._hid      : Optional[int] = None
        self._slot_id  : Optional[int] = None
        self._instance : Optional[uuid.UUID] = None

    # -- Lifecycle ---------------------------------------------------------

    @classmethod
    def open(cls,
             vid:     int = VHID_DEFAULT_VID,
             pid:     int = VHID_DEFAULT_PID,
             version: int = VHID_DEFAULT_REV,
             serial:  Optional[str] = None,
             timeout_ms: int = 5000) -> "Vhid":
        """Open the bus, plug in a device, and open the HID control
        device. Returns a Vhid instance; use it as a context manager
        (`with Vhid.open() as v:`) to guarantee unplug on exit."""

        self = cls()

        bus_path = _find_interface_path(GUID_DEVINTERFACE_VUSBBUS_CTL)
        if bus_path is None:
            raise VhidError(
                0,
                "vusbbus.sys control device not found - is the driver "
                "installed and started?"
            )
        self._bus = _open_path(bus_path)

        # Version + API-level check
        raw = _ioctl_sync(self._bus, IOCTL_VUSBBUS_GET_VERSION,
                          out_size=ctypes.sizeof(VHID_VERSION))
        ver = VHID_VERSION.from_buffer_copy(raw)
        if ver.ApiLevel != VHID_API_LEVEL:
            self._close_bus()
            raise VhidError(
                0,
                f"bus API level mismatch: driver={ver.ApiLevel} "
                f"wrapper={VHID_API_LEVEL}")

        # Plug in
        req = VHID_PLUGIN_REQ()
        req.Size    = ctypes.sizeof(VHID_PLUGIN_REQ)
        req.Vid     = vid
        req.Pid     = pid
        req.Version = version
        if serial:
            req.Serial = serial[:32]
        raw = _ioctl_sync(self._bus, IOCTL_VUSBBUS_PLUG_IN,
                          bytes(req),
                          out_size=ctypes.sizeof(VHID_PLUGIN_RESP))
        resp = VHID_PLUGIN_RESP.from_buffer_copy(raw)
        self._slot_id  = resp.SlotId
        self._instance = resp.InstanceId.to_uuid()

        # Wait for HID control interface
        hid_path = _wait_for_interface(
            GUID_DEVINTERFACE_VHIDKM_CTL, timeout_ms)
        if hid_path is None:
            self.close()
            raise VhidError(
                ERROR_TIMEOUT,
                "timed out waiting for vhidkm.sys control device")
        self._hid = _open_path(hid_path)

        raw = _ioctl_sync(self._hid, IOCTL_VHIDKM_GET_VERSION,
                          out_size=ctypes.sizeof(VHID_VERSION))
        ver = VHID_VERSION.from_buffer_copy(raw)
        if ver.ApiLevel != VHID_API_LEVEL:
            self.close()
            raise VhidError(
                0,
                f"hid API level mismatch: driver={ver.ApiLevel} "
                f"wrapper={VHID_API_LEVEL}")

        return self

    def __enter__(self) -> "Vhid":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def close(self) -> None:
        if self._hid is not None:
            kernel32.CloseHandle(self._hid)
            self._hid = None
        if self._bus is not None and self._slot_id is not None:
            try:
                ur = VHID_UNPLUG_REQ()
                ur.Size   = ctypes.sizeof(VHID_UNPLUG_REQ)
                ur.SlotId = self._slot_id
                _ioctl_sync(self._bus, IOCTL_VUSBBUS_UNPLUG, bytes(ur))
            except VhidError:
                # Best-effort: closing is the operative verb.
                pass
            self._slot_id = None
        self._close_bus()

    def _close_bus(self) -> None:
        if self._bus is not None:
            kernel32.CloseHandle(self._bus)
            self._bus = None

    # -- Introspection -----------------------------------------------------

    @property
    def slot_id(self) -> Optional[int]:
        return self._slot_id

    @property
    def instance_id(self) -> Optional[uuid.UUID]:
        return self._instance

    def bus_version(self) -> VHID_VERSION:
        raw = _ioctl_sync(self._bus, IOCTL_VUSBBUS_GET_VERSION,
                          out_size=ctypes.sizeof(VHID_VERSION))
        return VHID_VERSION.from_buffer_copy(raw)

    def hid_version(self) -> VHID_VERSION:
        raw = _ioctl_sync(self._hid, IOCTL_VHIDKM_GET_VERSION,
                          out_size=ctypes.sizeof(VHID_VERSION))
        return VHID_VERSION.from_buffer_copy(raw)

    # -- Screen metrics ----------------------------------------------------

    def set_screen_metrics(self, x: int, y: int, w: int, h: int) -> None:
        sm = VHID_SCREEN_METRICS()
        sm.Size          = ctypes.sizeof(VHID_SCREEN_METRICS)
        sm.VirtualX      = x
        sm.VirtualY      = y
        sm.VirtualWidth  = w
        sm.VirtualHeight = h
        _ioctl_sync(self._hid, IOCTL_VHIDKM_SET_SCREEN_METRICS, bytes(sm))

    def set_screen_metrics_auto(self) -> None:
        self.set_screen_metrics(
            user32.GetSystemMetrics(SM_XVIRTUALSCREEN),
            user32.GetSystemMetrics(SM_YVIRTUALSCREEN),
            user32.GetSystemMetrics(SM_CXVIRTUALSCREEN),
            user32.GetSystemMetrics(SM_CYVIRTUALSCREEN),
        )

    # -- Keyboard ----------------------------------------------------------

    def keyboard(self, modifiers: int,
                 keys: Iterable[int] = ()) -> None:
        """Submit a keyboard input report built from an iterable of
        up to VHID_KBD_MAX_KEYS HID usage codes. Extra keys are
        silently dropped to preserve the classic USB 6-key rollover."""
        k = list(keys)[:VHID_KBD_MAX_KEYS]
        k += [0] * (VHID_KBD_MAX_KEYS - len(k))
        req = VHID_KBD_KEYS_REQ()
        req.Size      = ctypes.sizeof(VHID_KBD_KEYS_REQ)
        req.Modifiers = int(modifiers) & 0xFF
        for i, code in enumerate(k):
            req.Keys[i] = int(code) & 0xFF
        _ioctl_sync(self._hid, IOCTL_VHIDKM_KEYBOARD_KEYS, bytes(req))

    def keystroke(self, modifiers: int, usage: int,
                  hold_ms: int = 50) -> None:
        req = VHID_KEYSTROKE_REQ()
        req.Size      = ctypes.sizeof(VHID_KEYSTROKE_REQ)
        req.Modifiers = int(modifiers) & 0xFF
        req.Usage     = int(usage)     & 0xFF
        req.HoldMs    = int(hold_ms)   & 0xFFFFFFFF
        _ioctl_sync(self._hid, IOCTL_VHIDKM_KEY_STROKE, bytes(req))

    # -- Mouse -------------------------------------------------------------

    def mouse_rel(self, buttons: int, dx: int, dy: int,
                  wheel: int = 0, hwheel: int = 0) -> None:
        rep = VHID_MOUSE_REL_REPORT()
        rep.ReportId = VHID_REPORTID_MOUSE_REL
        rep.Buttons  = int(buttons) & 0x1F
        rep.X        = int(dx)
        rep.Y        = int(dy)
        rep.Wheel    = int(wheel)
        rep.HWheel   = int(hwheel)
        _ioctl_sync(self._hid, IOCTL_VHIDKM_MOUSE_REL, bytes(rep))

    def mouse_abs(self, buttons: int, x: int, y: int,
                  wheel: int = 0, hwheel: int = 0) -> None:
        x = max(0, min(int(x), VHID_MOUSE_ABS_MAX))
        y = max(0, min(int(y), VHID_MOUSE_ABS_MAX))
        rep = VHID_MOUSE_ABS_REPORT()
        rep.ReportId = VHID_REPORTID_MOUSE_ABS
        rep.Buttons  = int(buttons) & 0x1F
        rep.X        = x
        rep.Y        = y
        rep.Wheel    = int(wheel)
        rep.HWheel   = int(hwheel)
        _ioctl_sync(self._hid, IOCTL_VHIDKM_MOUSE_ABS, bytes(rep))

    def mouse_abs_px(self, buttons: int, x: int, y: int,
                     wheel: int = 0, hwheel: int = 0) -> None:
        req = VHID_MOUSE_ABS_PX_REQ()
        req.Size    = ctypes.sizeof(VHID_MOUSE_ABS_PX_REQ)
        req.Buttons = int(buttons) & 0x1F
        req.Wheel   = int(wheel)
        req.HWheel  = int(hwheel)
        req.XPx     = int(x)
        req.YPx     = int(y)
        _ioctl_sync(self._hid, IOCTL_VHIDKM_MOUSE_ABS_PX, bytes(req))

    # -- LED ---------------------------------------------------------------

    def get_led_state(self) -> int:
        raw = _ioctl_sync(self._hid, IOCTL_VHIDKM_GET_LED_STATE,
                          out_size=1)
        return raw[0] if raw else 0

    def wait_led_change(self, baseline: int,
                        timeout_ms: int = INFINITE) -> int:
        """Block until the LED byte differs from baseline. Returns
        the new byte. Raises VhidError(ERROR_TIMEOUT) on timeout."""
        ev = kernel32.CreateEventW(None, True, False, None)
        if not ev:
            _raise_last("CreateEventW")

        try:
            ov = OVERLAPPED()
            ov.hEvent = ev
            in_buf  = bytes([int(baseline) & 0xFF])
            out_buf = ctypes.create_string_buffer(1)
            br      = wt.DWORD(0)

            in_raw = (ctypes.c_char * 1).from_buffer_copy(in_buf)
            ok = kernel32.DeviceIoControl(
                self._hid, IOCTL_VHIDKM_WAIT_LED_CHANGE,
                ctypes.cast(in_raw, ctypes.c_void_p), 1,
                ctypes.cast(out_buf, ctypes.c_void_p), 1,
                ctypes.byref(br), ctypes.byref(ov))

            if ok:
                return out_buf.raw[0] if isinstance(out_buf.raw[0], int) \
                       else ord(out_buf.raw[0])

            err = ctypes.get_last_error()
            if err != ERROR_IO_PENDING:
                raise VhidError(err, "WAIT_LED_CHANGE failed")

            wr = kernel32.WaitForSingleObject(ev, timeout_ms)
            if wr == WAIT_TIMEOUT:
                kernel32.CancelIoEx(self._hid, ctypes.byref(ov))
                kernel32.GetOverlappedResult(
                    self._hid, ctypes.byref(ov), ctypes.byref(br), True)
                raise VhidError(ERROR_TIMEOUT, "wait_led_change timed out")
            if wr != WAIT_OBJECT_0:
                _raise_last("WaitForSingleObject")

            if not kernel32.GetOverlappedResult(
                    self._hid, ctypes.byref(ov), ctypes.byref(br), False):
                _raise_last("GetOverlappedResult")

            b = out_buf.raw[0]
            return b if isinstance(b, int) else ord(b)
        finally:
            kernel32.CloseHandle(ev)

    # -- Reset -------------------------------------------------------------

    def reset(self) -> None:
        _ioctl_sync(self._hid, IOCTL_VHIDKM_RESET)
