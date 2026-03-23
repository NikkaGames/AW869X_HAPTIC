/*++
	Copyright (c) DuoWoA authors. All Rights Reserved.

	SPDX-License-Identifier: BSD-3-Clause

Module Name:

	HwnDefs.c - Translation functions for HwnClx requests

Abstract:

Environment:

	Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "spb.h"
#include "controller.h"

#ifdef DEBUG
#include "HwnDefs.tmh"
#endif

#define AW869X_DEFAULT_ON_PULSE_MS 150

static VOID
AW869XHapticSleepMs(
	ULONG Milliseconds
)
{
	LARGE_INTEGER interval = {};

	interval.QuadPart = -((LONGLONG)Milliseconds * 10 * 1000);
	KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

NTSTATUS
AW869XHapticToggleVibrationMotor(
	PDEVICE_CONTEXT devContext,
	PHWN_SETTINGS hwnSettings
)
{
	NTSTATUS status = STATUS_SUCCESS;
	ULONG intensity = 0;
	ULONG periodMs = 0;
	ULONG dutyCycle = 0;
	ULONG cycleCount = 0;
	ULONG pulseMs = 0;
	HWN_STATE hwnState;

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
#endif

	if (devContext == NULL || hwnSettings == NULL)
	{
		return STATUS_INVALID_PARAMETER;
	}

	if (devContext->BlinkTimer != NULL)
	{
		WdfTimerStop(devContext->BlinkTimer, FALSE);
	}

	hwnState = hwnSettings->OffOnBlink;
	intensity = hwnSettings->HwNSettings[HWN_INTENSITY];

	if (hwnState != HWN_OFF && intensity == 0)
	{
		intensity = 30;
	}

	switch (hwnState) {
	case HWN_OFF:
	{
		status = AW8624Stop(devContext);
		break;
	}
	case HWN_ON:
	{
		status = AW8624VibrateUntilStopped(devContext, intensity);
		if (NT_SUCCESS(status))
		{
			pulseMs = AW869X_DEFAULT_ON_PULSE_MS;
			AW869XHapticSleepMs(pulseMs);
			status = AW8624Stop(devContext);
		}
		break;
	}
	case HWN_BLINK:
	{
		periodMs = hwnSettings->HwNSettings[HWN_PERIOD];
		dutyCycle = hwnSettings->HwNSettings[HWN_DUTY_CYCLE];
		cycleCount = hwnSettings->HwNSettings[HWN_CYCLE_COUNT];

		if (periodMs == 0)
		{
			periodMs = 120;
		}
		if (dutyCycle == 0 || dutyCycle > 100)
		{
			dutyCycle = 50;
		}
		if (cycleCount == 0)
		{
			cycleCount = 1;
		}

		pulseMs = (periodMs * dutyCycle * cycleCount) / 100;
		if (pulseMs < 20)
		{
			pulseMs = 20;
		}
		if (pulseMs > 5000)
		{
			pulseMs = 5000;
		}

		status = AW8624PlayPulse(devContext, intensity, pulseMs);
		if (NT_SUCCESS(status))
		{
			AW869XHapticSleepMs(pulseMs);
			status = AW8624Stop(devContext);
		}
		break;
	}
	default:
	{
		status = STATUS_NOT_IMPLEMENTED;
		break;
	}
	}

#ifdef DEBUG
	Trace(
		TRACE_LEVEL_INFORMATION,
		TRACE_HAPTICS,
		"%!FUNC!: state=%lu intensity=%lu period=%lu duty=%lu cycles=%lu pulseMs=%lu status=%!STATUS!",
		(ULONG)hwnState,
		intensity,
		periodMs,
		dutyCycle,
		cycleCount,
		pulseMs,
		status);
#endif

	devContext->PreviousState = hwnState;
	return status;
}

NTSTATUS
AW869XHapticSetDevice(
	PDEVICE_CONTEXT devContext,
	PHWN_SETTINGS hwnSettings
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	UINT8 i = 0;

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
#endif

	if (devContext == NULL || hwnSettings == NULL)
	{
		return STATUS_INVALID_PARAMETER;
	}

	if (hwnSettings->HwNId >= (ULONG)devContext->NumberOfHapticsDevices)
	{
		return STATUS_INVALID_PARAMETER;
	}

	for (i = 0; i < devContext->NumberOfHapticsDevices; i++)
	{
		Status = AW869XHapticToggleVibrationMotor(
			devContext,
			hwnSettings
		);
	}

	return Status;
}

NTSTATUS
AW869XHapticInitializeDeviceState(
	PDEVICE_CONTEXT devContext
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	UINT8 i = 0;

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
#endif

	if (devContext == NULL)
	{
		return STATUS_INVALID_PARAMETER;
	}

	devContext->CurrentStates = (PAW8624_HAPTICS_CURRENT_STATE)ExAllocatePool2(
		PagedPool,
		sizeof(AW8624_HAPTICS_CURRENT_STATE),
		HAPTICS_POOL_TAG
	);

	if (devContext->CurrentStates)
	{
		PHWN_SETTINGS HwNSettingsInfo = &devContext->CurrentStates->CurrentState;

		HwNSettingsInfo->HwNId = 0;
		HwNSettingsInfo->HwNType = HWN_VIBRATOR;
		HwNSettingsInfo->OffOnBlink = HWN_OFF;

		for (i = 0; i < HWN_TOTAL_SETTINGS; i++)
		{
			HwNSettingsInfo->HwNSettings[i] = 0;
		}

		HwNSettingsInfo->HwNSettings[HWN_CURRENT_MTE_RESERVED] = HWN_CURRENT_MTE_NOT_SUPPORTED;

		devContext->CurrentStates->NextState = NULL;
	}
	else
	{
		Status = STATUS_UNSUCCESSFUL;
	}

	return Status;
}

NTSTATUS
AW869XHapticGetCurrentDeviceState(
	PDEVICE_CONTEXT devContext,
	PHWN_SETTINGS hwnSettings,
	ULONG hwnSettingsLength
)
{
	NTSTATUS Status = STATUS_SUCCESS;

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
#endif

	if (devContext == NULL || hwnSettings == NULL)
	{
		return STATUS_INVALID_PARAMETER;
	}

	PAW8624_HAPTICS_CURRENT_STATE currentState = devContext->CurrentStates;

	if (!currentState)
	{
		Status = AW869XHapticInitializeDeviceState(devContext);
		if (!NT_SUCCESS(Status))
		{
			return Status;
		}
		currentState = devContext->CurrentStates;
	}

	while (currentState && (hwnSettings->HwNId != currentState->CurrentState.HwNId))
	{
		currentState = currentState->NextState;
	}

	if (!currentState)
	{
		Status = STATUS_UNSUCCESSFUL;
	}
	else
	{
		Status = memcpy_s(
			(PVOID)hwnSettings,
			HWN_SETTINGS_SIZE,
			&(currentState->CurrentState),
			hwnSettingsLength
		);

		if (!NT_SUCCESS(Status))
		{
			Status = STATUS_UNSUCCESSFUL;
		}
	}

	return Status;
}

NTSTATUS
AW869XHapticSetCurrentDeviceState(
	PDEVICE_CONTEXT devContext,
	PHWN_SETTINGS hwnSettings,
	ULONG hwnSettingsLength
)
{
	NTSTATUS Status = STATUS_SUCCESS;

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
#endif

	if (devContext == NULL || hwnSettings == NULL)
	{
		return STATUS_INVALID_PARAMETER;
	}

	PAW8624_HAPTICS_CURRENT_STATE previousState = NULL;
	PAW8624_HAPTICS_CURRENT_STATE currentState = devContext->CurrentStates;

	hwnSettings->HwNSettings[HWN_CYCLE_GRANULARITY] = 0;
	hwnSettings->HwNSettings[HWN_CURRENT_MTE_RESERVED] = HWN_CURRENT_MTE_NOT_SUPPORTED;

	if (NULL == currentState)
	{
		devContext->CurrentStates = (PAW8624_HAPTICS_CURRENT_STATE)ExAllocatePool2(
			PagedPool,
			sizeof(AW8624_HAPTICS_CURRENT_STATE),
			HAPTICS_POOL_TAG);

		if (NULL != devContext->CurrentStates)
		{
			Status = memcpy_s(
				&(devContext->CurrentStates->CurrentState),
				HWN_SETTINGS_SIZE,
				(PVOID)hwnSettings, hwnSettingsLength
			);

			if (!NT_SUCCESS(Status))
			{
				Status = STATUS_UNSUCCESSFUL;
			}
			else
			{
				devContext->CurrentStates->NextState = NULL;
			}
		}
		else
		{
			Status = STATUS_UNSUCCESSFUL;
		}
	}
	else
	{
		while (currentState && (hwnSettings->HwNId != currentState->CurrentState.HwNId))
		{
			previousState = currentState;
			currentState = currentState->NextState;
		}

		if (currentState == NULL)
		{
			currentState = (PAW8624_HAPTICS_CURRENT_STATE)ExAllocatePool2(
				PagedPool,
				sizeof(AW8624_HAPTICS_CURRENT_STATE),
				HAPTICS_POOL_TAG
			);

			if (currentState)
			{
				Status = memcpy_s(
					&(currentState->CurrentState),
					HWN_SETTINGS_SIZE,
					(PVOID)hwnSettings,
					hwnSettingsLength
				);

				if (!NT_SUCCESS(Status))
				{
					Status = STATUS_UNSUCCESSFUL;
				}
				else
				{
					previousState->NextState = currentState;
					currentState->NextState = NULL;
				}
			}
			else
			{
				Status = STATUS_UNSUCCESSFUL;
			}
		}
		else
		{
			Status = memcpy_s(
				&(currentState->CurrentState),
				HWN_SETTINGS_SIZE,
				(PVOID)hwnSettings,
				hwnSettingsLength
			);

			if (!NT_SUCCESS(Status))
			{
				Status = STATUS_UNSUCCESSFUL;
			}
		}
	}

	return Status;
}
