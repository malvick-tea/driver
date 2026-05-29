/*
 * user/vhid.c
 *
 * Implementation of the user-mode VHID SDK. See user/vhid.h for the
 * public contract.
 *
 * Structure:
 *
 *   [discovery]
 *     FindInterfacePath      — resolve a device-interface GUID to its
 *                              \\?\ path string via SetupDiGetClassDevs
 *                              / SetupDiEnumDeviceInterfaces / SetupDi
 *                              GetDeviceInterfaceDetailW.
 *     OpenInterface          — open the resolved path with
 *                              FILE_SHARE_READ|WRITE and no buffering
 *                              flags (IOCTL routing is method-based).
 *
 *   [ioctl helpers]
 *     IoctlSync              — synchronous DeviceIoControl with the
 *                              OVERLAPPED path so cancelling works on
 *                              long-running IOCTLs. Folds STATUS_PENDING
 *                              into WaitForSingleObject.
 *     IoctlOverlapped        — asynchronous DeviceIoControl with a
 *                              caller-supplied event, for
 *                              wait_led_change's timeout-aware wait.
 *
 *   [lifecycle]
 *     vhid_open              — end-to-end startup.
 *     vhid_close             — teardown + resource release.
 *
 *   [public wrappers]
 *     Thin shims that marshal arguments into the protocol structs and
 *     call IoctlSync. Every wrapper validates its ctx pointer and the
 *     owning handle so a use-after-close is an error return, not a
 *     crash.
 *
 * Thread-safety:
 *   The context is not internally locked. Each public function makes
 *   at most one DeviceIoControl call and does not mutate the context,
 *   so multiple threads may issue injection IOCTLs concurrently using
 *   the same handle — the driver dispatches them serially on the
 *   default queue. Do not call vhid_close concurrently with any other
 *   vhid_* call on the same context; closing a handle while another
 *   thread holds it crosses a Win32 rule regardless of this SDK.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "vhid.h"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

/* Context. */

struct vhid_ctx {
    HANDLE      BusHandle;          /* control handle of vusbbus.sys */
    HANDLE      HidHandle;          /* control handle of vhidkm.sys   */
    uint32_t    SlotId;
    GUID        InstanceId;
    uint32_t    PluggedIn;          /* 1 once PLUG_IN succeeded       */
    wchar_t     HidInterfacePath[MAX_PATH];
};

/* Internal: device-interface discovery. */

static DWORD FindInterfacePath(
    const GUID *IfGuid,
    wchar_t    *OutPath,
    size_t      OutPathChars
    )
{
    HDEVINFO                          devInfo;
    SP_DEVICE_INTERFACE_DATA          ifData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = NULL;
    DWORD                              detailSize = 0;
    DWORD                              status = ERROR_NOT_FOUND;

    devInfo = SetupDiGetClassDevsW(IfGuid, NULL, NULL,
                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }

    ZeroMemory(&ifData, sizeof(ifData));
    ifData.cbSize = sizeof(ifData);

    /*
     * v1 publishes exactly one interface per GUID. We only read index
     * 0; a future multi-slot bus would loop until the enum returns
     * NO_MORE_ITEMS.
     */
    if (!SetupDiEnumDeviceInterfaces(devInfo, NULL, IfGuid, 0, &ifData)) {
        status = GetLastError();
        goto done;
    }

    /* Two-call pattern: first call sizes the buffer, second fills it. */
    SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0,
                                      &detailSize, NULL);
    if (detailSize == 0) {
        status = GetLastError();
        goto done;
    }

    detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)LocalAlloc(LPTR, detailSize);
    if (detail == NULL) {
        status = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

    if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail,
                                           detailSize, NULL, NULL)) {
        status = GetLastError();
        goto done;
    }

    if (wcslen(detail->DevicePath) + 1 > OutPathChars) {
        status = ERROR_INSUFFICIENT_BUFFER;
        goto done;
    }

    wcscpy_s(OutPath, OutPathChars, detail->DevicePath);
    status = ERROR_SUCCESS;

done:
    if (detail != NULL) LocalFree(detail);
    SetupDiDestroyDeviceInfoList(devInfo);
    return status;
}

/*
 * Wait for an interface to appear. PnP processing on PLUG_IN may take
 * a few hundred milliseconds before the HID control device publishes
 * its interface — the bus driver returns from PLUG_IN as soon as the
 * PDO is reported, not after PnP has finished the stack build-up.
 */
static DWORD WaitForInterface(
    const GUID *IfGuid,
    wchar_t    *OutPath,
    size_t      OutPathChars,
    DWORD       TimeoutMs
    )
{
    const DWORD pollIntervalMs = 50;
    DWORD       elapsed        = 0;

    for (;;) {
        DWORD s = FindInterfacePath(IfGuid, OutPath, OutPathChars);
        if (s == ERROR_SUCCESS) {
            return ERROR_SUCCESS;
        }
        if (elapsed >= TimeoutMs) {
            return ERROR_TIMEOUT;
        }
        Sleep(pollIntervalMs);
        elapsed += pollIntervalMs;
    }
}

static HANDLE OpenInterface(const wchar_t *Path)
{
    return CreateFileW(Path,
                       GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                       NULL);
}

/* Internal: IOCTL helpers. */

static DWORD IoctlSync(
    HANDLE  Handle,
    DWORD   Code,
    LPVOID  InBuf,
    DWORD   InLen,
    LPVOID  OutBuf,
    DWORD   OutLen,
    DWORD  *BytesReturned
    )
{
    OVERLAPPED  ov;
    DWORD       br = 0;
    BOOL        ok;

    if (Handle == NULL || Handle == INVALID_HANDLE_VALUE) {
        return ERROR_INVALID_HANDLE;
    }

    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ov.hEvent == NULL) {
        return GetLastError();
    }

    ok = DeviceIoControl(Handle, Code, InBuf, InLen, OutBuf, OutLen, &br, &ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            if (!GetOverlappedResult(Handle, &ov, &br, TRUE)) {
                err = GetLastError();
                CloseHandle(ov.hEvent);
                return err;
            }
            err = ERROR_SUCCESS;
        }
        if (err != ERROR_SUCCESS) {
            CloseHandle(ov.hEvent);
            return err;
        }
    }

    CloseHandle(ov.hEvent);
    if (BytesReturned != NULL) *BytesReturned = br;
    return ERROR_SUCCESS;
}

static DWORD IoctlOverlapped(
    HANDLE      Handle,
    DWORD       Code,
    LPVOID      InBuf,
    DWORD       InLen,
    LPVOID      OutBuf,
    DWORD       OutLen,
    OVERLAPPED *Ov
    )
{
    BOOL    ok;
    DWORD   br = 0;

    ok = DeviceIoControl(Handle, Code, InBuf, InLen, OutBuf, OutLen, &br, Ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            return ERROR_IO_PENDING;
        }
        return err;
    }
    return ERROR_SUCCESS;
}

/* Lifecycle. */

DWORD vhid_open(
    uint16_t          vid,
    uint16_t          pid,
    uint16_t          version,
    const wchar_t    *serial_optional,
    uint32_t          plugin_timeout_ms,
    vhid_ctx        **out_ctx
    )
{
    vhid_ctx           *ctx      = NULL;
    wchar_t             busPath[MAX_PATH];
    VHID_VERSION        ver;
    VHID_PLUGIN_REQ     req;
    VHID_PLUGIN_RESP    resp;
    DWORD               br       = 0;
    DWORD               status;

    if (out_ctx == NULL) return ERROR_INVALID_PARAMETER;
    *out_ctx = NULL;

    ctx = (vhid_ctx*)LocalAlloc(LPTR, sizeof(*ctx));
    if (ctx == NULL) return ERROR_NOT_ENOUGH_MEMORY;

    ctx->BusHandle = INVALID_HANDLE_VALUE;
    ctx->HidHandle = INVALID_HANDLE_VALUE;

    /* Resolve + open the bus control device. */
    status = FindInterfacePath(&GUID_DEVINTERFACE_VUSBBUS_CTL,
                                busPath, _countof(busPath));
    if (status != ERROR_SUCCESS) {
        goto fail;
    }

    ctx->BusHandle = OpenInterface(busPath);
    if (ctx->BusHandle == INVALID_HANDLE_VALUE) {
        status = GetLastError();
        goto fail;
    }

    /* Bus API-level check. Refuse to continue on mismatch. */
    status = IoctlSync(ctx->BusHandle, IOCTL_VUSBBUS_GET_VERSION,
                       NULL, 0, &ver, sizeof(ver), &br);
    if (status != ERROR_SUCCESS) goto fail;
    if (br < sizeof(ver) || ver.ApiLevel != VHID_API_LEVEL) {
        status = ERROR_REVISION_MISMATCH;
        goto fail;
    }

    /* Plug in. Defaults are applied server-side when fields are zero. */
    ZeroMemory(&req, sizeof(req));
    req.Size    = (UINT32)sizeof(req);
    req.Vid     = vid;
    req.Pid     = pid;
    req.Version = version;
    if (serial_optional != NULL) {
        size_t n = wcsnlen_s(serial_optional, _countof(req.Serial));
        memcpy(req.Serial, serial_optional, n * sizeof(wchar_t));
    }

    status = IoctlSync(ctx->BusHandle, IOCTL_VUSBBUS_PLUG_IN,
                       &req, sizeof(req), &resp, sizeof(resp), &br);
    if (status != ERROR_SUCCESS) goto fail;
    if (br < sizeof(resp)) {
        status = ERROR_INVALID_DATA;
        goto fail;
    }
    ctx->SlotId     = resp.SlotId;
    ctx->InstanceId = resp.InstanceId;
    ctx->PluggedIn  = 1;

    /* Wait for the HID control device to finish its PnP dance. */
    status = WaitForInterface(&GUID_DEVINTERFACE_VHIDKM_CTL,
                               ctx->HidInterfacePath,
                               _countof(ctx->HidInterfacePath),
                               plugin_timeout_ms ? plugin_timeout_ms : 5000);
    if (status != ERROR_SUCCESS) goto fail;

    ctx->HidHandle = OpenInterface(ctx->HidInterfacePath);
    if (ctx->HidHandle == INVALID_HANDLE_VALUE) {
        status = GetLastError();
        goto fail;
    }

    /* HID API-level check. */
    status = IoctlSync(ctx->HidHandle, IOCTL_VHIDKM_GET_VERSION,
                       NULL, 0, &ver, sizeof(ver), &br);
    if (status != ERROR_SUCCESS) goto fail;
    if (br < sizeof(ver) || ver.ApiLevel != VHID_API_LEVEL) {
        status = ERROR_REVISION_MISMATCH;
        goto fail;
    }

    *out_ctx = ctx;
    return ERROR_SUCCESS;

fail:
    vhid_close(ctx);
    return status;
}

void vhid_close(vhid_ctx *ctx)
{
    if (ctx == NULL) return;

    if (ctx->HidHandle != INVALID_HANDLE_VALUE && ctx->HidHandle != NULL) {
        CloseHandle(ctx->HidHandle);
        ctx->HidHandle = INVALID_HANDLE_VALUE;
    }

    if (ctx->PluggedIn && ctx->BusHandle != INVALID_HANDLE_VALUE &&
        ctx->BusHandle != NULL) {
        VHID_UNPLUG_REQ ur;
        ZeroMemory(&ur, sizeof(ur));
        ur.Size   = (UINT32)sizeof(ur);
        ur.SlotId = ctx->SlotId;
        /*
         * Best-effort: if the driver has already torn down (service
         * stopped, surprise remove) the IOCTL fails and we do not
         * surface the error. The caller asked to close, not to
         * guarantee a clean unplug.
         */
        (void)IoctlSync(ctx->BusHandle, IOCTL_VUSBBUS_UNPLUG,
                        &ur, sizeof(ur), NULL, 0, NULL);
        ctx->PluggedIn = 0;
    }

    if (ctx->BusHandle != INVALID_HANDLE_VALUE && ctx->BusHandle != NULL) {
        CloseHandle(ctx->BusHandle);
        ctx->BusHandle = INVALID_HANDLE_VALUE;
    }

    LocalFree(ctx);
}

/* Versioning. */

DWORD vhid_get_bus_version(vhid_ctx *ctx, VHID_VERSION *out_ver)
{
    DWORD br = 0;
    if (ctx == NULL || out_ver == NULL) return ERROR_INVALID_PARAMETER;
    return IoctlSync(ctx->BusHandle, IOCTL_VUSBBUS_GET_VERSION,
                     NULL, 0, out_ver, sizeof(*out_ver), &br);
}

DWORD vhid_get_hid_version(vhid_ctx *ctx, VHID_VERSION *out_ver)
{
    DWORD br = 0;
    if (ctx == NULL || out_ver == NULL) return ERROR_INVALID_PARAMETER;
    return IoctlSync(ctx->HidHandle, IOCTL_VHIDKM_GET_VERSION,
                     NULL, 0, out_ver, sizeof(*out_ver), &br);
}

/* Screen metrics. */

DWORD vhid_set_screen_metrics(
    vhid_ctx *ctx,
    int32_t   x,
    int32_t   y,
    int32_t   width,
    int32_t   height
    )
{
    VHID_SCREEN_METRICS req;
    DWORD br = 0;

    if (ctx == NULL) return ERROR_INVALID_PARAMETER;

    ZeroMemory(&req, sizeof(req));
    req.Size          = (UINT32)sizeof(req);
    req.VirtualX      = x;
    req.VirtualY      = y;
    req.VirtualWidth  = width;
    req.VirtualHeight = height;

    return IoctlSync(ctx->HidHandle, IOCTL_VHIDKM_SET_SCREEN_METRICS,
                     &req, sizeof(req), NULL, 0, &br);
}

DWORD vhid_set_screen_metrics_auto(vhid_ctx *ctx)
{
    int x  = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y  = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int cx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (cx <= 0 || cy <= 0) return ERROR_INVALID_STATE;
    return vhid_set_screen_metrics(ctx, x, y, cx, cy);
}

/* Keyboard. */

DWORD vhid_keyboard(
    vhid_ctx      *ctx,
    uint8_t        modifiers,
    const uint8_t  keys[VHID_KBD_MAX_KEYS]
    )
{
    VHID_KBD_KEYS_REQ req;
    DWORD br = 0;

    if (ctx == NULL) return ERROR_INVALID_PARAMETER;

    ZeroMemory(&req, sizeof(req));
    req.Size      = (UINT32)sizeof(req);
    req.Modifiers = modifiers;
    if (keys != NULL) {
        memcpy(req.Keys, keys, sizeof(req.Keys));
    }

    return IoctlSync(ctx->HidHandle, IOCTL_VHIDKM_KEYBOARD_KEYS,
                     &req, sizeof(req), NULL, 0, &br);
}

DWORD vhid_keystroke(
    vhid_ctx *ctx,
    uint8_t   modifiers,
    uint8_t   usage,
    uint32_t  hold_ms
    )
{
    VHID_KEYSTROKE_REQ req;
    DWORD br = 0;

    if (ctx == NULL) return ERROR_INVALID_PARAMETER;

    ZeroMemory(&req, sizeof(req));
    req.Size      = (UINT32)sizeof(req);
    req.Modifiers = modifiers;
    req.Usage     = usage;
    req.HoldMs    = hold_ms;

    return IoctlSync(ctx->HidHandle, IOCTL_VHIDKM_KEY_STROKE,
                     &req, sizeof(req), NULL, 0, &br);
}

/* Mouse. */

DWORD vhid_mouse_rel(
    vhid_ctx *ctx,
    uint8_t   buttons,
    int16_t   dx,
    int16_t   dy,
    int8_t    wheel,
    int8_t    hwheel
    )
{
    VHID_MOUSE_REL_REPORT rep;
    DWORD br = 0;

    if (ctx == NULL) return ERROR_INVALID_PARAMETER;

    ZeroMemory(&rep, sizeof(rep));
    rep.ReportId = VHID_REPORTID_MOUSE_REL;
    rep.Buttons  = buttons & VHID_MOUSE_BTN_MASK;
    rep.X        = dx;
    rep.Y        = dy;
    rep.Wheel    = wheel;
    rep.HWheel   = hwheel;

    return IoctlSync(ctx->HidHandle, IOCTL_VHIDKM_MOUSE_REL,
                     &rep, sizeof(rep), NULL, 0, &br);
}

DWORD vhid_mouse_abs(
    vhid_ctx *ctx,
    uint8_t   buttons,
    uint16_t  x,
    uint16_t  y,
    int8_t    wheel,
    int8_t    hwheel
    )
{
    VHID_MOUSE_ABS_REPORT rep;
    DWORD br = 0;

    if (ctx == NULL) return ERROR_INVALID_PARAMETER;

    if (x > VHID_MOUSE_ABS_MAX) x = VHID_MOUSE_ABS_MAX;
    if (y > VHID_MOUSE_ABS_MAX) y = VHID_MOUSE_ABS_MAX;

    ZeroMemory(&rep, sizeof(rep));
    rep.ReportId = VHID_REPORTID_MOUSE_ABS;
    rep.Buttons  = buttons & VHID_MOUSE_BTN_MASK;
    rep.X        = x;
    rep.Y        = y;
    rep.Wheel    = wheel;
    rep.HWheel   = hwheel;

    return IoctlSync(ctx->HidHandle, IOCTL_VHIDKM_MOUSE_ABS,
                     &rep, sizeof(rep), NULL, 0, &br);
}

DWORD vhid_mouse_abs_px(
    vhid_ctx *ctx,
    uint8_t   buttons,
    int32_t   x_px,
    int32_t   y_px,
    int8_t    wheel,
    int8_t    hwheel
    )
{
    VHID_MOUSE_ABS_PX_REQ req;
    DWORD br = 0;

    if (ctx == NULL) return ERROR_INVALID_PARAMETER;

    ZeroMemory(&req, sizeof(req));
    req.Size    = (UINT32)sizeof(req);
    req.Buttons = buttons & VHID_MOUSE_BTN_MASK;
    req.Wheel   = wheel;
    req.HWheel  = hwheel;
    req.XPx     = x_px;
    req.YPx     = y_px;

    return IoctlSync(ctx->HidHandle, IOCTL_VHIDKM_MOUSE_ABS_PX,
                     &req, sizeof(req), NULL, 0, &br);
}

/* LED. */

DWORD vhid_get_led_state(vhid_ctx *ctx, uint8_t *out_leds)
{
    UCHAR leds = 0;
    DWORD br = 0;
    DWORD status;

    if (ctx == NULL || out_leds == NULL) return ERROR_INVALID_PARAMETER;

    status = IoctlSync(ctx->HidHandle, IOCTL_VHIDKM_GET_LED_STATE,
                       NULL, 0, &leds, sizeof(leds), &br);
    if (status == ERROR_SUCCESS) {
        *out_leds = (uint8_t)leds;
    }
    return status;
}

DWORD vhid_wait_led_change(
    vhid_ctx  *ctx,
    uint8_t    baseline,
    uint32_t   timeout_ms,
    uint8_t   *out_leds
    )
{
    OVERLAPPED  ov;
    UCHAR       inb, outb = 0;
    DWORD       status;
    DWORD       waitRes;
    DWORD       br = 0;

    if (ctx == NULL || out_leds == NULL) return ERROR_INVALID_PARAMETER;

    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ov.hEvent == NULL) return GetLastError();

    inb = (UCHAR)baseline;

    status = IoctlOverlapped(ctx->HidHandle, IOCTL_VHIDKM_WAIT_LED_CHANGE,
                             &inb, sizeof(inb),
                             &outb, sizeof(outb),
                             &ov);
    if (status == ERROR_SUCCESS) {
        /* Inline completion: the driver already differed at call time. */
        *out_leds = (uint8_t)outb;
        CloseHandle(ov.hEvent);
        return ERROR_SUCCESS;
    }
    if (status != ERROR_IO_PENDING) {
        CloseHandle(ov.hEvent);
        return status;
    }

    waitRes = WaitForSingleObject(ov.hEvent, timeout_ms);
    if (waitRes == WAIT_TIMEOUT) {
        /*
         * Cancel the pending IOCTL so the driver does not deliver a
         * completion after we've already given up. CancelIoEx is
         * targeted at this specific OVERLAPPED; other pending I/Os on
         * the same handle are unaffected.
         */
        CancelIoEx(ctx->HidHandle, &ov);
        /* Drain the cancellation so the event is signalled. */
        (void)GetOverlappedResult(ctx->HidHandle, &ov, &br, TRUE);
        CloseHandle(ov.hEvent);
        return ERROR_TIMEOUT;
    }
    if (waitRes != WAIT_OBJECT_0) {
        DWORD e = GetLastError();
        CloseHandle(ov.hEvent);
        return e ? e : ERROR_WAIT_NO_CHILDREN;
    }

    if (!GetOverlappedResult(ctx->HidHandle, &ov, &br, FALSE)) {
        DWORD e = GetLastError();
        CloseHandle(ov.hEvent);
        return e;
    }

    *out_leds = (uint8_t)outb;
    CloseHandle(ov.hEvent);
    return ERROR_SUCCESS;
}

/* Reset. */

DWORD vhid_reset(vhid_ctx *ctx)
{
    DWORD br = 0;
    if (ctx == NULL) return ERROR_INVALID_PARAMETER;
    return IoctlSync(ctx->HidHandle, IOCTL_VHIDKM_RESET,
                     NULL, 0, NULL, 0, &br);
}

/* Bus admin. */

DWORD vhid_list_slots(
    vhid_ctx       *ctx,
    VHID_SLOT_INFO *out_slots,
    uint32_t        max_slots,
    uint32_t       *out_count
    )
{
    VHID_SLOT_LIST *list;
    DWORD           listLen;
    DWORD           br = 0;
    DWORD           status;

    if (ctx == NULL || out_count == NULL) return ERROR_INVALID_PARAMETER;
    if (max_slots > 0 && out_slots == NULL) return ERROR_INVALID_PARAMETER;

    /* Always defined, even on the early IOCTL-failure return below. */
    *out_count = 0;

    /*
     * The driver returns a variable-length VHID_SLOT_LIST. We size the
     * receive buffer to fit max_slots entries; a short buffer triggers
     * STATUS_BUFFER_TOO_SMALL / ERROR_INSUFFICIENT_BUFFER and the
     * caller is expected to retry with a larger buffer.
     */
    listLen = (DWORD)(sizeof(VHID_SLOT_LIST) +
                      (max_slots == 0 ? 0 :
                       (max_slots - 1) * sizeof(VHID_SLOT_INFO)));
    if (listLen < sizeof(VHID_SLOT_LIST)) listLen = sizeof(VHID_SLOT_LIST);

    list = (VHID_SLOT_LIST*)LocalAlloc(LPTR, listLen);
    if (list == NULL) return ERROR_NOT_ENOUGH_MEMORY;

    status = IoctlSync(ctx->BusHandle, IOCTL_VUSBBUS_LIST,
                       NULL, 0, list, listLen, &br);
    if (status != ERROR_SUCCESS) {
        LocalFree(list);
        return status;
    }

    *out_count = list->Count;
    if (list->Count > max_slots) {
        LocalFree(list);
        return ERROR_INSUFFICIENT_BUFFER;
    }

    if (list->Count > 0 && out_slots != NULL) {
        memcpy(out_slots, list->Slots,
               list->Count * sizeof(VHID_SLOT_INFO));
    }

    LocalFree(list);
    return ERROR_SUCCESS;
}

uint32_t vhid_current_slot_id(vhid_ctx *ctx)
{
    return (ctx != NULL) ? ctx->SlotId : 0xFFFFFFFFu;
}
