/*++
    Copyright (c) Microsoft Corporation. All Rights Reserved.
    Copyright (c) DuoWoA authors. All Rights Reserved.

    SPDX-License-Identifier: BSD-3-Clause

    Module Name:

        aw8624.h

    Abstract:

        Generic Awinic high-voltage haptics definitions used by the Windows
        HWNCLX port. The file name is kept for project compatibility.

    Environment:

        Kernel Mode

--*/

#pragma once

#include "haptic_hv_reg.h"

#define AW_REG_CHIPID        0x00
#define AW_REG_CHIPIDH       0x57
#define AW_REG_CHIPIDL       0x58

#define AW8695_CHIPID        0x95
#define AW8697_CHIPID        0x97
#define AW86905_CHIPID       0x05
#define AW86907_CHIPID       0x04
#define AW86915_CHIPID       0x07
#define AW86917_CHIPID       0x06
#define AW86715_CHIPID       0x7150
#define AW86716_CHIPID       0x7170
#define AW86717_CHIPID       0x7171
#define AW86718_CHIPID       0x7180
#define AW86925_CHIPID       0x9250
#define AW86926_CHIPID       0x9260
#define AW86927_CHIPID       0x9270
#define AW86928_CHIPID       0x9280

#define AW_I2C_READ_RETRIES  5
#define AW_STANDBY_RETRIES   100
#define AW_GAIN_MAX          0x80

typedef enum _AW_HAPTIC_FAMILY
{
    AwHapticFamilyUnknown = 0,
    AwHapticFamily869x,
    AwHapticFamily869xx,
    AwHapticFamily8671x,
    AwHapticFamily8692x,
} AW_HAPTIC_FAMILY;

typedef struct _AW_HAPTIC_SETTINGS
{
    UCHAR Mode;
    ULONG F0Pre;
    UCHAR F0CaliPercent;
    UCHAR MaxBstVol;
    UCHAR BstVolDefault;
    UCHAR BstVolRam;
    UCHAR D2sGain;
    UCHAR ContDrv1Lvl;
    UCHAR ContDrv2Lvl;
    UCHAR ContDrv1Time;
    UCHAR ContDrv2Time;
    UCHAR ContDrvWidth;
    UCHAR ContWaitNum;
    UCHAR ContBrkTime;
    UCHAR ContTrackMargin;
    UCHAR BrkBstMd;
    UCHAR ContTset;
    UCHAR ContBemfSet;
    UCHAR ContBstBrkGain;
    UCHAR ContBrkGain;
    UCHAR Tset;
    USHORT ContZcThr;
    USHORT ContTd;
    UCHAR ContNumBrk;
    UCHAR ContDrvLvl;
    UCHAR ContDrvLvlOv;
    UCHAR RSpare;
    UCHAR BstDbg[3];
    UCHAR BemfConfig[4];
    UCHAR DurationTime[3];
    BOOLEAN TrackEnable;
} AW_HAPTIC_SETTINGS, *PAW_HAPTIC_SETTINGS;

typedef struct _DEVICE_CONTEXT DEVICE_CONTEXT, *PDEVICE_CONTEXT;

NTSTATUS
AW8624Start(
    _In_ PDEVICE_CONTEXT pDevice
);

NTSTATUS
AW8624Stop(
    _In_ PDEVICE_CONTEXT pDevice
);

NTSTATUS
AW8624VibrateUntilStopped(
    _In_ PDEVICE_CONTEXT pDevice,
    _In_ ULONG Intensity
);

NTSTATUS
AW8624PlayPulse(
    _In_ PDEVICE_CONTEXT pDevice,
    _In_ ULONG Intensity,
    _In_ ULONG DurationMs
);

NTSTATUS
AW8624Initialize(
    _In_ PDEVICE_CONTEXT pDevice
);
