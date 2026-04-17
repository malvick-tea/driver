/*
 * bus/pdo_ids.h
 *
 * Helpers for populating the PDO's identity strings. The bus driver
 * keeps the string-formatting logic separate from the PDO lifecycle
 * so the format of hardware / instance ids is auditable in isolation
 * (they are the surface area that PnP matching reads from).
 */

#pragma once
#ifndef VHID_BUS_PDO_IDS_H_
#define VHID_BUS_PDO_IDS_H_

#include "driver.h"
#include "fdo.h"

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
VusbBusPdoAssignIds(
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ PCVUSBBUS_SLOT  Slot
    );

#endif /* VHID_BUS_PDO_IDS_H_ */
