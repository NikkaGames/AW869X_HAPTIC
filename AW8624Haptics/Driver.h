/*++
	Copyright (c) DuoWoA authors. All Rights Reserved.

	SPDX-License-Identifier: BSD-3-Clause

Module Name:

	driver.h

Abstract:

	This file contains the driver definitions.

Environment:

	Kernel-mode Driver Framework

--*/

#pragma once

#include <wdm.h>
#include <wdf.h>
#include <initguid.h>
#include <hwnclx.h>
#include <hwn.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include "device.h"
#include "spb.h"

#ifdef DEBUG
#include "trace.h"
#endif

EXTERN_C_START

//
// WDFDRIVER Events
//

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AW8624HapticsEvtDeviceAdd;
EVT_WDF_DRIVER_UNLOAD AW8624HapticsEvtDriverUnload;
EVT_WDF_OBJECT_CONTEXT_CLEANUP AW8624HapticsEvtDriverContextCleanup;
HWN_CLIENT_INITIALIZE_DEVICE AW8624HapticsInitializeDevice;
HWN_CLIENT_UNINITIALIZE_DEVICE AW8624HapticsUnInitializeDevice;
HWN_CLIENT_QUERY_DEVICE_INFORMATION AW8624HapticsQueryDeviceInformation;
HWN_CLIENT_START_DEVICE AW8624HapticsStartDevice;
HWN_CLIENT_STOP_DEVICE AW8624HapticsStopDevice;
HWN_CLIENT_SET_STATE AW8624HapticsSetState;
HWN_CLIENT_GET_STATE AW8624HapticsGetState;

EVT_WDF_INTERRUPT_ISR AW8624HapticsEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC AW8624HapticsEvtInterruptDpc;
EVT_WDF_TIMER AW8624HapticsBlinkTimerFunc;

EXTERN_C_END
