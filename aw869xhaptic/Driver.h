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
EVT_WDF_DRIVER_DEVICE_ADD AW869XHapticEvtDeviceAdd;
EVT_WDF_DRIVER_UNLOAD AW869XHapticEvtDriverUnload;
EVT_WDF_OBJECT_CONTEXT_CLEANUP AW869XHapticEvtDriverContextCleanup;
HWN_CLIENT_INITIALIZE_DEVICE AW869XHapticInitializeDevice;
HWN_CLIENT_UNINITIALIZE_DEVICE AW869XHapticUnInitializeDevice;
HWN_CLIENT_QUERY_DEVICE_INFORMATION AW869XHapticQueryDeviceInformation;
HWN_CLIENT_START_DEVICE AW869XHapticStartDevice;
HWN_CLIENT_STOP_DEVICE AW869XHapticStopDevice;
HWN_CLIENT_SET_STATE AW869XHapticSetState;
HWN_CLIENT_GET_STATE AW869XHapticGetState;

EVT_WDF_INTERRUPT_ISR AW869XHapticEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC AW869XHapticEvtInterruptDpc;
EVT_WDF_TIMER AW869XHapticBlinkTimerFunc;

EXTERN_C_END
