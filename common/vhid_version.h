/*
 * vhid_version.h
 *
 * Single source of truth for the project's version numbers. These values
 * surface in several places that must agree:
 *
 *   - DriverVer in both INFs (vusbbus.inx, vhidkm.inx). The INF DriverVer
 *     is populated by Stampinf from the .vcxproj <DriverVer> property at
 *     build time; the constant here is kept in sync by developer
 *     discipline rather than by preprocessor substitution, because INFs
 *     are not run through the C preprocessor.
 *
 *   - The bcdDevice field of the USB device descriptor, which becomes
 *     the "REV_xxxx" suffix of the device instance id and the
 *     VersionNumber returned from IOCTL_HID_GET_DEVICE_ATTRIBUTES.
 *
 *   - The VHID_VERSION struct returned by IOCTL_VUSBBUS_GET_VERSION and
 *     IOCTL_VHIDKM_GET_VERSION. User-mode callers should read this
 *     before issuing any other IOCTL and refuse to continue on mismatch.
 *
 * Bump VHID_API_LEVEL whenever the IOCTL ABI changes in a way user-mode
 * code must adapt to (added/removed IOCTL, changed request/response
 * layout, changed report struct layout). Never reuse an API level.
 */

#pragma once
#ifndef VHID_VERSION_H_
#define VHID_VERSION_H_

#define VHID_VERSION_MAJOR   1u
#define VHID_VERSION_MINOR   0u
#define VHID_VERSION_BUILD   0u

#define VHID_API_LEVEL       1u

#define VHID_VERSION_STR_A   "1.0.0"
#define VHID_VERSION_STR_W   L"1.0.0"

/*
 * Defaults advertised by the virtual USB device. v1 accepts but ignores
 * caller-supplied VID/PID/REV in IOCTL_VUSBBUS_PLUG_IN; the INF hardware
 * IDs are static, so the PDO must report these values regardless of what
 * the caller requested. The plumbing for per-instance VID/PID is present
 * so that a future driver revision could introduce a dynamic-INF model
 * without breaking the existing ABI.
 */
#define VHID_DEFAULT_VID     0x1209u  /* pid.codes registered free-range VID */
#define VHID_DEFAULT_PID     0xBEEFu  /* project-specific PID                */
#define VHID_DEFAULT_REV     0x0100u  /* bcdDevice 1.00                      */

#define VHID_DEFAULT_MFG_STRING_W      L"Virtual HID Systems"
#define VHID_DEFAULT_PRODUCT_STRING_W  L"Virtual HID Keyboard + Mouse"

#endif /* VHID_VERSION_H_ */
