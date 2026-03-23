/*++
	Copyright (c) DuoWoA authors. All Rights Reserved.

	SPDX-License-Identifier: BSD-3-Clause

Module Name:

	HwnDefs.c - Translation functions for HwnClx requests

Abstract:

Environment:

	Kernel-mode Driver Framework

--*/

#pragma once

#include "device.h"

NTSTATUS
AW869XHapticSetDevice(
	PDEVICE_CONTEXT devContext,
	PHWN_SETTINGS hwnSettings
);

NTSTATUS
AW869XHapticInitializeDeviceState(
	PDEVICE_CONTEXT devContext
);

NTSTATUS
AW869XHapticGetCurrentDeviceState(
	PDEVICE_CONTEXT devContext,
	PHWN_SETTINGS hwnSettings,
	ULONG hwnSettingsLength
);

NTSTATUS
AW869XHapticSetCurrentDeviceState(
	PDEVICE_CONTEXT devContext,
	PHWN_SETTINGS hwnSettings,
	ULONG hwnSettingsLength
);