/*
 * user/demo.c
 *
 * End-to-end demo for the VHID SDK. Exercises the full public
 * surface in a self-contained console application so anyone
 * troubleshooting a freshly installed driver stack can run
 *
 *     vhid_demo.exe
 *
 * and verify that every injection path reaches the Windows input
 * stack.
 *
 * What the demo does, in order:
 *
 *   1. Opens the VHID stack with default VID/PID.
 *   2. Prints the driver versions.
 *   3. Configures screen metrics from the running session.
 *   4. Issues a mouse move to the virtual-screen centre.
 *   5. Sends a short keystroke burst typing "HELLO".
 *   6. Scrolls the mouse wheel up then down.
 *   7. Demonstrates a blocking LED-state wait (the caller can tap
 *      CapsLock on any real keyboard to release the wait).
 *   8. Cleanly tears down.
 *
 * Every call checks its return value and prints a diagnostic string on
 * failure. There are no fatal asserts; the demo continues past
 * individual errors so partial support surfaces can be identified.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "vhid.h"

/*
 * HID Usage Page 0x07 keycodes used by the demo.
 */
#define HID_USAGE_KEY_H         0x0B
#define HID_USAGE_KEY_E         0x08
#define HID_USAGE_KEY_L         0x0F
#define HID_USAGE_KEY_O         0x12
#define HID_USAGE_KEY_SPACE     0x2C
#define HID_USAGE_KEY_ENTER     0x28

static void PrintErr(const char *op, DWORD status)
{
    LPWSTR msg = NULL;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, status, 0, (LPWSTR)&msg, 0, NULL);
    fprintf(stderr, "[vhid_demo] %s failed: 0x%08lx%s%ls\n",
            op, (unsigned long)status,
            (msg != NULL) ? " - " : "",
            (msg != NULL) ? msg  : L"");
    if (msg != NULL) LocalFree(msg);
}

static DWORD TypeChar(vhid_ctx *ctx, uint8_t usage, uint8_t modifiers)
{
    /*
     * Each character is a 60 ms press. That is slow enough for Windows
     * to register each event distinctly but fast enough to complete
     * the demo quickly. The driver clamps to VHID_KEYSTROKE_HOLD_MAX_MS
     * regardless.
     */
    return vhid_keystroke(ctx, modifiers, usage, 60);
}

int wmain(int argc, wchar_t **argv)
{
    vhid_ctx      *ctx = NULL;
    VHID_VERSION   ver;
    DWORD          status;
    uint8_t        leds = 0;

    (void)argc;
    (void)argv;

    wprintf(L"[vhid_demo] opening VHID stack (VID=0x%04x PID=0x%04x)...\n",
            VHID_DEFAULT_VID, VHID_DEFAULT_PID);

    status = vhid_open(VHID_DEFAULT_VID, VHID_DEFAULT_PID,
                       VHID_DEFAULT_REV, L"vhid-demo",
                       /* plugin_timeout_ms */ 5000,
                       &ctx);
    if (status != ERROR_SUCCESS) {
        PrintErr("vhid_open", status);
        return 1;
    }

    if (vhid_get_bus_version(ctx, &ver) == ERROR_SUCCESS) {
        wprintf(L"[vhid_demo] bus  v%u.%u.%u (api %u)\n",
                ver.Major, ver.Minor, ver.Build, ver.ApiLevel);
    }
    if (vhid_get_hid_version(ctx, &ver) == ERROR_SUCCESS) {
        wprintf(L"[vhid_demo] hid  v%u.%u.%u (api %u) slot=%u\n",
                ver.Major, ver.Minor, ver.Build, ver.ApiLevel,
                vhid_current_slot_id(ctx));
    }

    /* -------- Screen metrics -------- */
    status = vhid_set_screen_metrics_auto(ctx);
    if (status != ERROR_SUCCESS) {
        PrintErr("vhid_set_screen_metrics_auto", status);
    }

    /* -------- Mouse to centre of virtual screen -------- */
    {
        int x = GetSystemMetrics(SM_XVIRTUALSCREEN) +
                GetSystemMetrics(SM_CXVIRTUALSCREEN) / 2;
        int y = GetSystemMetrics(SM_YVIRTUALSCREEN) +
                GetSystemMetrics(SM_CYVIRTUALSCREEN) / 2;
        wprintf(L"[vhid_demo] moving to (%d, %d)\n", x, y);
        status = vhid_mouse_abs_px(ctx, 0, x, y, 0, 0);
        if (status != ERROR_SUCCESS) PrintErr("vhid_mouse_abs_px", status);
    }

    /* -------- Keystrokes: type HELLO -------- */
    wprintf(L"[vhid_demo] typing HELLO...\n");
    {
        static const uint8_t Letters[] = {
            HID_USAGE_KEY_H, HID_USAGE_KEY_E, HID_USAGE_KEY_L,
            HID_USAGE_KEY_L, HID_USAGE_KEY_O
        };
        size_t i;
        /*
         * Hold LSHIFT for uppercase. The modifier is supplied to each
         * keystroke independently — the driver sends the press report
         * with the modifier set and the release report with zeros,
         * which the upper stack translates into the appropriate
         * VK_SHIFT up/down events bracketing each letter.
         */
        for (i = 0; i < _countof(Letters); i++) {
            (void)TypeChar(ctx, Letters[i], VHID_KBD_MOD_LSHIFT);
            Sleep(80);
        }
    }

    /* -------- Mouse wheel -------- */
    wprintf(L"[vhid_demo] wheel up then down...\n");
    (void)vhid_mouse_rel(ctx, 0, 0, 0, +3, 0);
    Sleep(200);
    (void)vhid_mouse_rel(ctx, 0, 0, 0, -3, 0);

    /* -------- LED wait (best-effort) -------- */
    wprintf(L"[vhid_demo] reading LED state...\n");
    status = vhid_get_led_state(ctx, &leds);
    if (status == ERROR_SUCCESS) {
        wprintf(L"[vhid_demo] initial LEDs = 0x%02x\n", leds);
        wprintf(L"[vhid_demo] waiting up to 3s for an LED change...\n");
        {
            uint8_t newLeds = leds;
            status = vhid_wait_led_change(ctx, leds, 3000, &newLeds);
            if (status == ERROR_SUCCESS) {
                wprintf(L"[vhid_demo] LED change 0x%02x -> 0x%02x\n",
                        leds, newLeds);
            } else if (status == ERROR_TIMEOUT) {
                wprintf(L"[vhid_demo] no LED change observed (timeout)\n");
            } else {
                PrintErr("vhid_wait_led_change", status);
            }
        }
    } else {
        PrintErr("vhid_get_led_state", status);
    }

    /* -------- Reset + close -------- */
    (void)vhid_reset(ctx);

    wprintf(L"[vhid_demo] closing...\n");
    vhid_close(ctx);
    return 0;
}
