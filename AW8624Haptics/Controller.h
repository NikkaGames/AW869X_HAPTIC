#pragma once

#include "device.h"

NTSTATUS
AW8624Start(
	IN PDEVICE_CONTEXT pDevice
);

NTSTATUS
AW8624Stop(
	IN PDEVICE_CONTEXT pDevice
);
NTSTATUS
AW8624VibrateUntilStopped(
	IN PDEVICE_CONTEXT pDevice,
	IN ULONG Intensity
);

NTSTATUS
AW8624Initialize(
	IN PDEVICE_CONTEXT pDevice
);