/*
 * bus/queue.h
 *
 * Control-device IOCTL dispatcher bindings.
 *
 * VusbBusQueueInitialize installs the default IO queue on the
 * control device and wires up the IOCTL handler. The handler is a
 * single EvtIoDeviceControl callback that switches on the IoCtl
 * code and delegates to the FDO API in fdo.c.
 *
 * The FDO context pointer is captured into a WDFDEVICE context
 * allocated on the control device so the handler can reach the
 * slot table without a global.
 */

#pragma once
#ifndef VHID_BUS_QUEUE_H_
#define VHID_BUS_QUEUE_H_

#include "driver.h"
#include "fdo.h"

typedef struct _VUSBBUS_CTL_DEV_CONTEXT {
    /*
     * Backpointer to the bus FDO context. The control device is a
     * driver-lifetime singleton that can outlive any single FDO (the
     * root-enumerated bus can be disabled and re-enabled), so this
     * pointer is guarded by Lock and cleared on FDO teardown. Readers
     * take a reference on FdoCtx->Fdo for the duration of their work.
     */
    PVUSBBUS_FDO_CONTEXT FdoCtx;
    WDFSPINLOCK          Lock;
} VUSBBUS_CTL_DEV_CONTEXT, *PVUSBBUS_CTL_DEV_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VUSBBUS_CTL_DEV_CONTEXT, VusbBusCtlDevGetContext);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
VusbBusQueueInitialize(
    _In_ WDFDEVICE ControlDevice
    );

#endif /* VHID_BUS_QUEUE_H_ */
