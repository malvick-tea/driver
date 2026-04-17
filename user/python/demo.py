"""
user/python/demo.py

End-to-end Python demo for vhid.py. Mirrors the functionality of
user/demo.c so feature parity between the C SDK and the Python
wrapper is easy to spot at a glance.

Run as an Administrator:

    python demo.py

What it does:

    1. Opens the VHID stack.
    2. Prints bus and HID versions.
    3. Sets screen metrics from the current session.
    4. Centres the cursor on the primary virtual screen.
    5. Types "HELLO" with the left Shift modifier.
    6. Scrolls the mouse wheel up then down.
    7. Reads the current LED byte and waits up to 3 seconds for a
       change (tap CapsLock on any real keyboard to release the wait).
    8. Resets all inputs and tears down cleanly.
"""

from __future__ import annotations

import ctypes
import sys
import time

from vhid import (
    Vhid,
    VhidError,
    KbdMod,
    KbdLed,
    VHID_DEFAULT_VID,
    VHID_DEFAULT_PID,
    VHID_DEFAULT_REV,
    user32,
    SM_XVIRTUALSCREEN,
    SM_YVIRTUALSCREEN,
    SM_CXVIRTUALSCREEN,
    SM_CYVIRTUALSCREEN,
)


# HID Usage Page 0x07 keycodes.
KEY_H, KEY_E, KEY_L, KEY_O = 0x0B, 0x08, 0x0F, 0x12


def log(msg: str) -> None:
    print(f"[vhid_demo.py] {msg}", flush=True)


def main() -> int:
    log(f"opening VHID stack "
        f"(VID=0x{VHID_DEFAULT_VID:04x} PID=0x{VHID_DEFAULT_PID:04x})")

    try:
        vhid = Vhid.open(VHID_DEFAULT_VID, VHID_DEFAULT_PID,
                         VHID_DEFAULT_REV, serial="vhid-demo-py",
                         timeout_ms=5000)
    except VhidError as e:
        log(f"vhid_open failed: {e}")
        return 1

    with vhid as v:
        bv = v.bus_version()
        hv = v.hid_version()
        log(f"bus v{bv.Major}.{bv.Minor}.{bv.Build} (api {bv.ApiLevel})")
        log(f"hid v{hv.Major}.{hv.Minor}.{hv.Build} (api {hv.ApiLevel}) "
            f"slot={v.slot_id}")

        # -- Screen metrics ------------------------------------------------
        try:
            v.set_screen_metrics_auto()
        except VhidError as e:
            log(f"set_screen_metrics_auto failed: {e}")

        # -- Cursor to virtual-screen centre ------------------------------
        x = (user32.GetSystemMetrics(SM_XVIRTUALSCREEN) +
             user32.GetSystemMetrics(SM_CXVIRTUALSCREEN) // 2)
        y = (user32.GetSystemMetrics(SM_YVIRTUALSCREEN) +
             user32.GetSystemMetrics(SM_CYVIRTUALSCREEN) // 2)
        log(f"moving to ({x}, {y})")
        try:
            v.mouse_abs_px(0, x, y)
        except VhidError as e:
            log(f"mouse_abs_px failed: {e}")

        # -- Type HELLO ----------------------------------------------------
        log("typing HELLO...")
        for code in (KEY_H, KEY_E, KEY_L, KEY_L, KEY_O):
            try:
                v.keystroke(KbdMod.LSHIFT, code, hold_ms=60)
                time.sleep(0.08)
            except VhidError as e:
                log(f"keystroke failed: {e}")

        # -- Wheel ---------------------------------------------------------
        log("wheel up then down...")
        try:
            v.mouse_rel(0, 0, 0, wheel=+3)
            time.sleep(0.2)
            v.mouse_rel(0, 0, 0, wheel=-3)
        except VhidError as e:
            log(f"mouse_rel failed: {e}")

        # -- LED wait ------------------------------------------------------
        try:
            leds = v.get_led_state()
            log(f"initial LEDs = 0x{leds:02x}")
            log("waiting up to 3s for an LED change (tap CapsLock)...")
            try:
                new_leds = v.wait_led_change(leds, timeout_ms=3000)
                log(f"LED change 0x{leds:02x} -> 0x{new_leds:02x}")
            except VhidError as e:
                if e.winerror == 1460:  # ERROR_TIMEOUT
                    log("no LED change observed (timeout)")
                else:
                    log(f"wait_led_change failed: {e}")
        except VhidError as e:
            log(f"get_led_state failed: {e}")

        # -- Reset ---------------------------------------------------------
        try:
            v.reset()
        except VhidError as e:
            log(f"reset failed: {e}")

    log("closed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
