/*++
	Copyright (c) DuoWoA authors. All Rights Reserved.

	SPDX-License-Identifier: BSD-3-Clause

Module Name:

	HwnClient.c - Handling HwnClx Requests

Abstract:

Environment:

	Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "spb.h"
#include "controller.h"
#include "hwndefs.h"

#ifdef DEBUG
#include "hwnclient.tmh"
#endif

static VOID
AW869XHapticSleepMs(
	ULONG Milliseconds
)
{
	LARGE_INTEGER interval = {};

	interval.QuadPart = -((LONGLONG)Milliseconds * 10 * 1000);
	KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AW869XHapticInitializeDevice)
#pragma alloc_text (PAGE, AW869XHapticUnInitializeDevice)
#pragma alloc_text (PAGE, AW869XHapticQueryDeviceInformation)
#pragma alloc_text (PAGE, AW869XHapticStartDevice)
#pragma alloc_text (PAGE, AW869XHapticStopDevice)
#pragma alloc_text (PAGE, AW869XHapticSetState)
#pragma alloc_text (PAGE, AW869XHapticGetState)
#endif

PDEVICE_CONTEXT globalContext = NULL;

BOOLEAN
AW869XHapticEvtInterruptIsr(
	WDFINTERRUPT Interrupt,
	ULONG MessageID
)
{
	UNREFERENCED_PARAMETER(Interrupt);
	UNREFERENCED_PARAMETER(MessageID);

	return TRUE;
}

NTSTATUS
AW869XHapticInitializeDevice(
	__in WDFDEVICE Device,
	__in PVOID Context,
	__in WDFCMRESLIST ResourcesRaw,
	__in WDFCMRESLIST ResourcesTranslated
)
{
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
	PCM_PARTIAL_RESOURCE_DESCRIPTOR res, resRaw;
	ULONG resourceCount;
	ULONG i;
	BOOLEAN I2CDetected = FALSE;
	BOOLEAN InterruptDetected = FALSE;
	WDF_INTERRUPT_CONFIG interruptConfig;

	PAGED_CODE();

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
#endif

	PDEVICE_CONTEXT devContext = (PDEVICE_CONTEXT)Context;
	
	globalContext = devContext;
	
	devContext->Device = Device;
	devContext->InterruptPresent = FALSE;

	//
	// Get the resouce hub connection ID for our I2C driver
	//
	resourceCount = WdfCmResourceListGetCount(ResourcesTranslated);

	for (i = 0; i < resourceCount; i++)
	{
		res = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
		resRaw = WdfCmResourceListGetDescriptor(ResourcesRaw, i);

		if (res->Type == CmResourceTypeConnection &&
			res->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
			res->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
		{
			devContext->I2CContext.I2cResHubId.LowPart =
				res->u.Connection.IdLowPart;
			devContext->I2CContext.I2cResHubId.HighPart =
				res->u.Connection.IdHighPart;

			I2CDetected = TRUE;
		}
		else if (res->Type == CmResourceTypeInterrupt)
		{
			WDF_INTERRUPT_CONFIG_INIT(
				&interruptConfig,
				AW869XHapticEvtInterruptIsr,
				NULL
			);

			interruptConfig.InterruptTranslated = res;
			interruptConfig.InterruptRaw = resRaw;
			interruptConfig.PassiveHandling = TRUE;

			status = WdfInterruptCreate(
				Device,
				&interruptConfig,
				WDF_NO_OBJECT_ATTRIBUTES,
				&devContext->InterruptObject
			);

			if (!NT_SUCCESS(status))
			{
#ifdef DEBUG
				Trace(
					TRACE_LEVEL_ERROR,
					TRACE_INIT,
					"WdfInterruptCreate failed %!STATUS!",
					status
				);
#endif

				goto exit;
			}

			InterruptDetected = TRUE;
			devContext->InterruptPresent = TRUE;
		}
	}

	if (I2CDetected)
	{
		status = STATUS_SUCCESS;
	}

	if (!InterruptDetected)
	{
#ifdef DEBUG
		Trace(
			TRACE_LEVEL_WARNING,
			TRACE_INIT,
			"No interrupt resource found, continuing in I2C-only mode");
#endif
	}

	if (!NT_SUCCESS(status))
	{
#ifdef DEBUG
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_INIT,
			"Error finding CmResourceTypeConnection resource - %!STATUS!",
			status);
#endif
		goto exit;
	}

	//
	// Initialize Spb so the driver can issue reads/writes
	//
	status = SpbTargetInitialize(Device, &devContext->I2CContext);

	if (!NT_SUCCESS(status))
	{
#ifdef DEBUG
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_INIT,
			"Error in Spb initialization - %!STATUS!",
			status);
#endif
		goto exit;
	}

	status = AW8624Initialize(devContext);

	if (!NT_SUCCESS(status))
	{
#ifdef DEBUG
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_INIT,
			"Error in AW8624 initialization - %!STATUS!",
			status);
#endif
		goto exit;
	}

	devContext->NumberOfHapticsDevices = 1;

exit:
	return status;
}

NTSTATUS
AW869XHapticUnInitializeDevice(
	__in WDFDEVICE Device,
	__in PVOID Context
)
{
	NTSTATUS status = STATUS_SUCCESS;

	PAGED_CODE();

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
#endif

	PAW8624_HAPTICS_CURRENT_STATE currentState = NULL;
	PAW8624_HAPTICS_CURRENT_STATE stateToFree = NULL;

	PDEVICE_CONTEXT devContext = (PDEVICE_CONTEXT)Context;

	currentState = devContext->CurrentStates;

	while (currentState != NULL)
	{
		stateToFree = currentState;
		currentState = currentState->NextState;
		ExFreePoolWithTag(stateToFree, HAPTICS_POOL_TAG);
	}

	SpbTargetDeinitialize(Device, &devContext->I2CContext);

	return status;
}

NTSTATUS
AW869XHapticQueryDeviceInformation(
	__in PVOID Context,
	__out PCLIENT_DEVICE_INFORMATION Information
)
{
	NTSTATUS status = STATUS_SUCCESS;

	PAGED_CODE();

	PDEVICE_CONTEXT devContext = (PDEVICE_CONTEXT)Context;

#ifdef DEBUG
	Trace(
		TRACE_LEVEL_INFORMATION,
		TRACE_INIT,
		"%!FUNC! Entry");
#endif

	Information->Version = HWN_DEVICE_INFORMATION_VERSION;
	Information->Size = sizeof(CLIENT_DEVICE_INFORMATION);

	Information->TotalHwNs = devContext->NumberOfHapticsDevices;

#ifdef DEBUG
	Trace(
		TRACE_LEVEL_INFORMATION,
		TRACE_INIT,
		"%!FUNC! Exit");
#endif

	return status;
}

NTSTATUS
AW869XHapticStartDevice(
	__in PVOID Context
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_CONTEXT devContext = (PDEVICE_CONTEXT)Context;

	PAGED_CODE();

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
#endif

#ifdef DEBUG
	if (devContext == NULL)
	{
		Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "%!FUNC!: invalid context");
		return STATUS_INVALID_PARAMETER;
	}
#endif

	status = AW8624Start(devContext);

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC!: status=%!STATUS!", status);
#endif

	return status;
}

NTSTATUS
AW869XHapticStopDevice(
	__in PVOID Context
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_CONTEXT devContext = (PDEVICE_CONTEXT)Context;

	PAGED_CODE();

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
#endif

#ifdef DEBUG
	if (devContext == NULL)
	{
		Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "%!FUNC!: invalid context");
		return STATUS_INVALID_PARAMETER;
	}
#endif

	if (devContext->BlinkTimer != NULL)
	{
		WdfTimerStop(devContext->BlinkTimer, FALSE);
	}

	status = AW8624VibrateUntilStopped(devContext, 50);
	if (NT_SUCCESS(status))
	{
		AW869XHapticSleepMs(100);
	}

	status = AW8624Stop(devContext);

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC!: pulseMs=%lu intensity=%lu status=%!STATUS!", 100UL, 50UL, status);
#endif

	return status;
}

#define NUMBER_OF_HWN_DEVICES(x) (x - HWN_HEADER_SIZE) / HWN_SETTINGS_SIZE
#define EXTRA_BYTES_AFTER_HWN_DEVICES(x) ((x - HWN_HEADER_SIZE) % HWN_SETTINGS_SIZE)

NTSTATUS
AW869XHapticSetState(
	__in PVOID Context,
	__in_bcount(BufferLength) PVOID Buffer,
	__in ULONG BufferLength,
	__out PULONG BytesWritten
)
{
	NTSTATUS status = STATUS_SUCCESS;

	PAGED_CODE();

#ifdef DEBUG
	Trace(
		TRACE_LEVEL_INFORMATION,
		TRACE_INIT,
		"%!FUNC! Entry");
#endif

	if (Context == NULL || Buffer == NULL || BytesWritten == NULL)
	{
#ifdef DEBUG
		Trace(
			TRACE_LEVEL_INFORMATION,
			TRACE_INIT,
			"Invalid buffers");
#endif

		status = STATUS_INVALID_PARAMETER;
		goto exit;
	}

	if (BufferLength <= HWN_HEADER_SIZE || EXTRA_BYTES_AFTER_HWN_DEVICES(BufferLength) != 0)
	{
#ifdef DEBUG
		Trace(
			TRACE_LEVEL_INFORMATION,
			TRACE_INIT,
			"Invalid Output buffer sizes");
#endif

		status = STATUS_INVALID_BUFFER_SIZE;
		goto exit;
	}

	PDEVICE_CONTEXT devContext = (PDEVICE_CONTEXT)Context;
	PHWN_HEADER hwnHeader = (PHWN_HEADER)Buffer;
	ULONG NumberOfHwnDevicesInBuffer = 1;
	ULONG i;

	*BytesWritten = 0;

	NumberOfHwnDevicesInBuffer = NUMBER_OF_HWN_DEVICES(BufferLength);

	for (i = 0; i < NumberOfHwnDevicesInBuffer; i++)
	{
#ifdef DEBUG
		Trace(
			TRACE_LEVEL_INFORMATION,
			TRACE_HAPTICS,
			"%!FUNC!: hwnId=%lu state=%lu intensity=%lu period=%lu duty=%lu cycles=%lu",
			hwnHeader->HwNSettingsInfo[i].HwNId,
			(ULONG)hwnHeader->HwNSettingsInfo[i].OffOnBlink,
			hwnHeader->HwNSettingsInfo[i].HwNSettings[HWN_INTENSITY],
			hwnHeader->HwNSettingsInfo[i].HwNSettings[HWN_PERIOD],
			hwnHeader->HwNSettingsInfo[i].HwNSettings[HWN_DUTY_CYCLE],
			hwnHeader->HwNSettingsInfo[i].HwNSettings[HWN_CYCLE_COUNT]);
#endif

		// Set device settings here
		status = AW869XHapticSetDevice(Context, &(hwnHeader->HwNSettingsInfo[i]));
		if (!NT_SUCCESS(status))
		{
			goto exit;
		}

		// Backup device settings somewhere to retrieve them later
		status = AW869XHapticSetCurrentDeviceState(devContext, &(hwnHeader->HwNSettingsInfo[i]), HWN_SETTINGS_SIZE);
		if (!NT_SUCCESS(status))
		{
			goto exit;
		}
	}

	*BytesWritten = BufferLength;

exit:

#ifdef DEBUG
	Trace(
		TRACE_LEVEL_INFORMATION,
		TRACE_INIT,
		"%!FUNC! Exit");
#endif

	return status;
}

VOID
AW869XHapticBlinkTimerFunc(
	WDFTIMER Timer
)
{
	WDFDEVICE device;
	PDEVICE_CONTEXT devContext;
	NTSTATUS status;

	device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
	if (device == NULL)
	{
		return;
	}

	devContext = DeviceGetContext(device);
	if (devContext == NULL)
	{
		return;
	}

	status = AW8624Stop(devContext);
	if (NT_SUCCESS(status) && devContext->CurrentStates != NULL)
	{
		devContext->CurrentStates->CurrentState.OffOnBlink = HWN_OFF;
		devContext->CurrentStates->CurrentState.HwNSettings[HWN_INTENSITY] = 0;
	}
	devContext->PreviousState = HWN_OFF;

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_HAPTICS, "%!FUNC!: status=%!STATUS!", status);
#endif
}

NTSTATUS
AW869XHapticGetState(
	__in PVOID Context,
	__out_bcount(OutputBufferLength) PVOID OutputBuffer,
	__in ULONG OutputBufferLength,
	__in_bcount(InputBufferLength) PVOID InputBuffer,
	__in ULONG InputBufferLength,
	__out PULONG BytesRead
)
{
	NTSTATUS status = STATUS_SUCCESS;

	PAGED_CODE();

#ifdef DEBUG
	Trace(
		TRACE_LEVEL_INFORMATION,
		TRACE_INIT,
		"%!FUNC! Entry");
#endif

	if (Context == NULL || OutputBuffer == NULL || BytesRead == NULL)
	{
#ifdef DEBUG
		Trace(
			TRACE_LEVEL_INFORMATION,
			TRACE_INIT,
			"Invalid buffers");
#endif

		status = STATUS_INVALID_PARAMETER;
		goto exit;
	}

	if (OutputBufferLength <= HWN_HEADER_SIZE || EXTRA_BYTES_AFTER_HWN_DEVICES(OutputBufferLength) != 0)
	{
#ifdef DEBUG
		Trace(
			TRACE_LEVEL_INFORMATION,
			TRACE_INIT,
			"Invalid Output buffer sizes");
#endif

		status = STATUS_INVALID_BUFFER_SIZE;
		goto exit;
	}

	PDEVICE_CONTEXT devContext = (PDEVICE_CONTEXT)Context;
	PHWN_HEADER hwnHeader;
	ULONG NumberOfHwnDevicesInBuffer = 1;
	ULONG i;

	*BytesRead = 0;

	if (InputBuffer == NULL)
	{
		// Send information about all Haptics devices

		hwnHeader = (PHWN_HEADER)OutputBuffer;

		hwnHeader->HwNPayloadSize = HWN_HEADER_SIZE + devContext->NumberOfHapticsDevices * HWN_SETTINGS_SIZE;
		hwnHeader->HwNPayloadVersion = 1;
		hwnHeader->HwNRequests = devContext->NumberOfHapticsDevices;

		NumberOfHwnDevicesInBuffer = NUMBER_OF_HWN_DEVICES(OutputBufferLength);

		for (i = 0; i < NumberOfHwnDevicesInBuffer; i++)
		{
			hwnHeader->HwNSettingsInfo[i].HwNId = i;

			// Set other settings values into the buffer here.
			status = AW869XHapticGetCurrentDeviceState(devContext, &hwnHeader->HwNSettingsInfo[i], HWN_SETTINGS_SIZE);
			if (!NT_SUCCESS(status))
			{
				goto exit;
			}
		}

		*BytesRead = OutputBufferLength;
	}
	else
	{
		// Send information about a specific Haptics device

		if (InputBufferLength <= HWN_HEADER_SIZE || EXTRA_BYTES_AFTER_HWN_DEVICES(InputBufferLength) != 0)
		{
#ifdef DEBUG
			Trace(
				TRACE_LEVEL_INFORMATION,
				TRACE_INIT,
				"Invalid Input buffer sizes");
#endif

			status = STATUS_INVALID_BUFFER_SIZE;
			goto exit;
		}

		hwnHeader = (PHWN_HEADER)InputBuffer;

		NumberOfHwnDevicesInBuffer = NUMBER_OF_HWN_DEVICES(InputBufferLength);

		for (i = 0; i < NumberOfHwnDevicesInBuffer; i++)
		{
			// Set other settings values into the buffer here.
			status = AW869XHapticGetCurrentDeviceState(devContext, &hwnHeader->HwNSettingsInfo[i], HWN_SETTINGS_SIZE);
			if (!NT_SUCCESS(status))
			{
				goto exit;
			}
		}

		*BytesRead = InputBufferLength;
	}

exit:

#ifdef DEBUG
	Trace(
		TRACE_LEVEL_INFORMATION,
		TRACE_INIT,
		"%!FUNC! Exit");
#endif

	return status;
}
