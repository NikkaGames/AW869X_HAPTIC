/*++
	Copyright (c) DuoWoA authors. All Rights Reserved.

	SPDX-License-Identifier: BSD-3-Clause

Module Name:

	device.c - Device handling events for example driver.

Abstract:

   This file contains the device entry points and callbacks.

Environment:

	Kernel-mode Driver Framework

--*/

#include "driver.h"

#ifdef DEBUG
#include "device.tmh"
#endif

NTSTATUS
AW869XHapticCreateDevice(
	_Inout_ WDFDRIVER Driver,
	_Inout_ PWDFDEVICE_INIT DeviceInit
)
/*++

Routine Description:

	Worker routine called to create a device and its software resources.

Arguments:

	DeviceInit - Pointer to an opaque init structure. Memory for this
					structure will be freed by the framework when the WdfDeviceCreate
					succeeds. So don't access the structure after that point.

Return Value:

	NTSTATUS

--*/
{
	WDF_OBJECT_ATTRIBUTES deviceAttributes;
	WDF_OBJECT_ATTRIBUTES timerAttributes;
	WDFDEVICE device;
	WDF_TIMER_CONFIG timerConfig;
	NTSTATUS status;
	PDEVICE_CONTEXT devContext;

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

	status = HwNProcessAddDevicePreDeviceCreate(
		Driver,
		DeviceInit,
		&deviceAttributes
	);

	if (!NT_SUCCESS(status)) {
#ifdef DEBUG
		Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "HwNProcessAddDevicePreDeviceCreate failed %!STATUS!", status);
#endif
		return status;
	}

	status = WdfDeviceCreate(
		&DeviceInit,
		&deviceAttributes,
		&device
	);

	if (!NT_SUCCESS(status)) {
#ifdef DEBUG
		Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfDeviceCreate failed %!STATUS!", status);
#endif
		return status;
	}

	status = HwNProcessAddDevicePostDeviceCreate(
		Driver,
		device,
		(LPGUID)&HWN_DEVINTERFACE_VIBRATOR
	);

	if (!NT_SUCCESS(status)) {
#ifdef DEBUG
		Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "HwNProcessAddDevicePostDeviceCreate failed %!STATUS!", status);
#endif
		return status;
	}

	status = WdfDeviceCreateDeviceInterface(
		device,
		(LPGUID)&HWN_DEVINTERFACE,
		NULL
	);

	if (!NT_SUCCESS(status)) {
#ifdef DEBUG
		Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfDeviceCreateDeviceInterface(HWN_DEVINTERFACE) failed %!STATUS!", status);
#endif
		return status;
	}

	devContext = DeviceGetContext(device);
	if (devContext != NULL)
	{
		devContext->Device = device;

		WDF_TIMER_CONFIG_INIT(&timerConfig, AW869XHapticBlinkTimerFunc);
		timerConfig.AutomaticSerialization = FALSE;

		WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
		timerAttributes.ParentObject = device;
		timerAttributes.ExecutionLevel = WdfExecutionLevelPassive;

		status = WdfTimerCreate(&timerConfig, &timerAttributes, &devContext->BlinkTimer);
		if (!NT_SUCCESS(status))
		{
#ifdef DEBUG
			Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfTimerCreate failed %!STATUS!", status);
#endif
			return status;
		}
	}

	return status;
}
