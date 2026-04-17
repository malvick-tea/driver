/*
 * hid/ctl_ioctls.h
 *
 * User-mode IOCTL surface for vhidkm.sys. Wires up the EvtIoDeviceControl
 * callback on the control device's default queue. Every IOCTL is
 * dispatched from a single switch inside ctl_ioctls.c to keep the
 * code path auditable.
 */

#pragma once
#ifndef VHID_HID_CTL_IOCTLS_H_
#define VHID_HID_CTL_IOCTLS_H_

#include "driver.h"

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VhidkmCtlIoctlInitialize(
    _In_ WDFDEVICE ControlDevice
    );

#endif /* VHID_HID_CTL_IOCTLS_H_ */
