/*++
    Copyright (c) Microsoft Corporation. All Rights Reserved.
    Copyright (c) DuoWoA authors. All Rights Reserved.

    SPDX-License-Identifier: BSD-3-Clause

    Module Name:

        aw8624.c

    Abstract:

        Generic Awinic high-voltage haptics core used by the Windows HWNCLX
        port. The source file name is kept for project compatibility.

    Environment:

        Kernel Mode

--*/

#include "Driver.h"
#include "Controller.h"
#include "aw8624.h"

#ifdef DEBUG
#include "aw8624.tmh"
#endif

#define AW_UNUSED(x) (void)(x)

static NTSTATUS AwReadBytes(PDEVICE_CONTEXT DevContext, UCHAR Address, PUCHAR Data, ULONG Length)
{
    NTSTATUS Status = SpbReadDataSynchronously(&DevContext->I2CContext, Address, Data, Length);
#ifdef DEBUG
    if (!NT_SUCCESS(Status)) {
        Trace(TRACE_LEVEL_ERROR, TRACE_SPB, "%!FUNC!: read addr=0x%02X len=%lu status=%!STATUS!", Address, Length, Status);
    }
#endif
    return Status;
}

static NTSTATUS AwReadByte(PDEVICE_CONTEXT DevContext, UCHAR Address, PUCHAR Data)
{
    return AwReadBytes(DevContext, Address, Data, 1);
}

static NTSTATUS AwWriteBytes(PDEVICE_CONTEXT DevContext, UCHAR Address, const UCHAR* Data, ULONG Length)
{
    NTSTATUS Status = SpbWriteDataSynchronously(&DevContext->I2CContext, Address, (PVOID)Data, Length);
#ifdef DEBUG
    if (!NT_SUCCESS(Status)) {
        Trace(TRACE_LEVEL_ERROR, TRACE_SPB, "%!FUNC!: write addr=0x%02X len=%lu status=%!STATUS!", Address, Length, Status);
    }
#endif
    return Status;
}

static NTSTATUS AwWriteByte(PDEVICE_CONTEXT DevContext, UCHAR Address, UCHAR Data)
{
    return AwWriteBytes(DevContext, Address, &Data, 1);
}

static NTSTATUS AwWriteBits(PDEVICE_CONTEXT DevContext, UCHAR Address, UCHAR Mask, UCHAR Value)
{
    NTSTATUS Status;
    UCHAR RegValue = 0;

    Status = AwReadByte(DevContext, Address, &RegValue);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    RegValue &= Mask;
    RegValue |= Value;
    return AwWriteByte(DevContext, Address, RegValue);
}

static VOID AwSleepUs(ULONG Microseconds)
{
    KeStallExecutionProcessor(Microseconds);
}

static VOID AwSleepMs(ULONG Milliseconds)
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -((LONGLONG)Milliseconds * 10 * 1000);
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

static NTSTATUS AwSoftReset(PDEVICE_CONTEXT DevContext)
{
    NTSTATUS Status;
    UCHAR Reset = 0xAA;

    Status = AwWriteByte(DevContext, AW_REG_CHIPID, Reset);
    if (NT_SUCCESS(Status)) {
        AwSleepUs(3500);
    }

#ifdef DEBUG
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC!: status=%!STATUS!", Status);
#endif

    return Status;
}

static UCHAR AwScaleU8(UCHAR BaseValue, ULONG Percent)
{
    ULONG Scaled;

    if (Percent == 0) {
        return 0;
    }
    if (Percent > 100) {
        Percent = 100;
    }

    Scaled = ((ULONG)BaseValue * Percent) / 100;
    if (Scaled == 0) {
        Scaled = 1;
    }
    if (Scaled > 0xFF) {
        Scaled = 0xFF;
    }
    return (UCHAR)Scaled;
}

static UCHAR AwScaleNonZeroU8(UCHAR BaseValue, ULONG Percent)
{
    UCHAR Scaled = AwScaleU8(BaseValue, Percent);

    if (Scaled == 0) {
        Scaled = 1;
    }

    return Scaled;
}

static VOID AwLoadNx729jLeftSettings(PDEVICE_CONTEXT DevContext)
{
    RtlZeroMemory(&DevContext->Settings, sizeof(DevContext->Settings));

    DevContext->Settings.Mode = 0x05;
    DevContext->Settings.F0Pre = 0x6A4;
    DevContext->Settings.F0CaliPercent = 0x07;
    DevContext->Settings.TrackEnable = TRUE;

    switch (DevContext->Family) {
    case AwHapticFamily8692x:
        DevContext->Settings.BstVolDefault = 0x50;
        DevContext->Settings.D2sGain = 0x04;
        DevContext->Settings.ContBrkGain = 0x08;
        DevContext->Settings.ContBstBrkGain = 0x05;
        DevContext->Settings.ContBemfSet = 0x02;
        DevContext->Settings.ContTset = 0x06;
        DevContext->Settings.BrkBstMd = 0x00;
        DevContext->Settings.ContTrackMargin = 0x0C;
        DevContext->Settings.ContBrkTime = 0x00;
        DevContext->Settings.ContWaitNum = 0x06;
        DevContext->Settings.ContDrvWidth = 0x6A;
        DevContext->Settings.ContDrv2Time = 0x06;
        DevContext->Settings.ContDrv1Time = 0x04;
        DevContext->Settings.ContDrv2Lvl = 0x50;
        DevContext->Settings.ContDrv1Lvl = 0x7F;
        DevContext->Settings.MaxBstVol = 0x7F;
        break;

    case AwHapticFamily8671x:
        DevContext->Settings.BstVolDefault = 0x04;
        DevContext->Settings.D2sGain = 0x04;
        DevContext->Settings.ContTrackMargin = 0x12;
        DevContext->Settings.ContDrv2Time = 0x14;
        DevContext->Settings.ContDrv1Time = 0x04;
        DevContext->Settings.ContBrkGain = 0x08;
        DevContext->Settings.ContBstBrkGain = 0x05;
        DevContext->Settings.ContWaitNum = 0x06;
        DevContext->Settings.ContDrvWidth = 0x71;
        DevContext->Settings.ContBemfSet = 0x02;
        DevContext->Settings.ContTset = 0x06;
        DevContext->Settings.ContBrkTime = 0x08;
        DevContext->Settings.ContDrv2Lvl = 0x28;
        DevContext->Settings.ContDrv1Lvl = 0x7F;
        DevContext->Settings.BrkBstMd = 0x00;
        DevContext->Settings.MaxBstVol = 0x0F;
        break;

    case AwHapticFamily869xx:
        DevContext->Settings.BstVolDefault = 0x20;
        DevContext->Settings.D2sGain = 0x04;
        DevContext->Settings.ContTrackMargin = 0x12;
        DevContext->Settings.ContDrv2Time = 0x14;
        DevContext->Settings.ContDrv1Time = 0x04;
        DevContext->Settings.ContBrkGain = 0x08;
        DevContext->Settings.ContBstBrkGain = 0x05;
        DevContext->Settings.ContWaitNum = 0x06;
        DevContext->Settings.ContDrvWidth = 0x6A;
        DevContext->Settings.ContBemfSet = 0x02;
        DevContext->Settings.ContTset = 0x06;
        DevContext->Settings.ContBrkTime = 0x08;
        DevContext->Settings.ContDrv2Lvl = 0x36;
        DevContext->Settings.ContDrv1Lvl = 0x7F;
        DevContext->Settings.BrkBstMd = 0x00;
        DevContext->Settings.MaxBstVol = 0x3F;
        break;

    case AwHapticFamily869x:
        DevContext->Settings.Tset = 0x12;
        DevContext->Settings.ContTd = 0x009A;
        DevContext->Settings.ContZcThr = 0x0FF1;
        DevContext->Settings.ContNumBrk = 0x03;
        DevContext->Settings.ContDrvLvl = 0x35;
        DevContext->Settings.ContDrvLvlOv = 0x7D;
        DevContext->Settings.MaxBstVol = 0x1F;
        DevContext->Settings.RSpare = 0x68;
        DevContext->Settings.BstDbg[0] = 0x30;
        DevContext->Settings.BstDbg[1] = 0xEB;
        DevContext->Settings.BstDbg[2] = 0xD4;
        DevContext->Settings.BemfConfig[0] = 0x10;
        DevContext->Settings.BemfConfig[1] = 0x08;
        DevContext->Settings.BemfConfig[2] = 0x03;
        DevContext->Settings.BemfConfig[3] = 0xF8;
        break;

    default:
        break;
    }
}

static NTSTATUS AwReadChipId(PDEVICE_CONTEXT DevContext)
{
    NTSTATUS Status;
    UCHAR Value8 = 0;
    UCHAR Value16[2] = { 0 };
    USHORT ChipId16;

    DevContext->Family = AwHapticFamilyUnknown;
    DevContext->ChipId = 0;

    Status = AwReadByte(DevContext, AW_REG_CHIPID, &Value8);
    if (NT_SUCCESS(Status)) {
        switch (Value8) {
        case AW8695_CHIPID:
        case AW8697_CHIPID:
            DevContext->ChipId = Value8;
            DevContext->Family = AwHapticFamily869x;
            break;
        case AW86905_CHIPID:
        case AW86907_CHIPID:
        case AW86915_CHIPID:
        case AW86917_CHIPID:
            DevContext->ChipId = Value8;
            DevContext->Family = AwHapticFamily869xx;
            break;
        default:
            break;
        }
    }

    if (DevContext->Family != AwHapticFamilyUnknown) {
        return STATUS_SUCCESS;
    }

    Status = AwReadBytes(DevContext, AW_REG_CHIPIDH, Value16, sizeof(Value16));
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    ChipId16 = ((USHORT)Value16[0] << 8) | Value16[1];
    switch (ChipId16) {
    case AW86715_CHIPID:
    case AW86716_CHIPID:
    case AW86717_CHIPID:
    case AW86718_CHIPID:
        DevContext->ChipId = ChipId16;
        DevContext->Family = AwHapticFamily8671x;
        return STATUS_SUCCESS;

    case AW86925_CHIPID:
    case AW86926_CHIPID:
    case AW86927_CHIPID:
    case AW86928_CHIPID:
        DevContext->ChipId = ChipId16;
        DevContext->Family = AwHapticFamily8692x;
        return STATUS_SUCCESS;

    default:
#ifdef DEBUG
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "%!FUNC!: unsupported chip id 0x%04X", ChipId16);
#endif
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS AwWaitStandbyCommon(PDEVICE_CONTEXT DevContext, UCHAR RegGlbState)
{
    NTSTATUS Status;
    UCHAR RegValue = 0;
    ULONG Count;

    for (Count = 0; Count < AW_STANDBY_RETRIES; ++Count) {
        Status = AwReadByte(DevContext, RegGlbState, &RegValue);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        if ((RegValue & 0x0F) == 0x00) {
            return STATUS_SUCCESS;
        }
        AwSleepUs(2500);
    }

    return STATUS_IO_TIMEOUT;
}

static NTSTATUS Aw8692xSetGain(PDEVICE_CONTEXT DevContext, UCHAR Gain)
{
    return AwWriteByte(DevContext, AW8692X_REG_PLAYCFG2, Gain);
}

static NTSTATUS Aw8671xSetGain(PDEVICE_CONTEXT DevContext, UCHAR Gain)
{
    return AwWriteByte(DevContext, AW8671X_REG_PLAYCFG2, Gain);
}

static NTSTATUS Aw869xxSetGain(PDEVICE_CONTEXT DevContext, UCHAR Gain)
{
    return AwWriteByte(DevContext, AW869XX_REG_PLAYCFG2, Gain);
}

static NTSTATUS Aw869xSetGain(PDEVICE_CONTEXT DevContext, UCHAR Gain)
{
    return AwWriteByte(DevContext, AW869X_REG_DATDBG, Gain);
}

static NTSTATUS Aw8692xSetBstVol(PDEVICE_CONTEXT DevContext, UCHAR BstVol)
{
    if (BstVol & AW8692X_BIT_PLAYCFG1_BIT8) {
        BstVol = AW8692X_BIT_PLAYCFG1_BST_VOUT_MAX;
    }
    if (BstVol < AW8692X_BIT_PLAYCFG1_BST_VOUT_6V) {
        BstVol = AW8692X_BIT_PLAYCFG1_BST_VOUT_6V;
    }
    return AwWriteBits(DevContext, AW8692X_REG_PLAYCFG1,
        (UCHAR)AW8692X_BIT_PLAYCFG1_BST_VOUT_VREFSET_MASK, BstVol);
}

static NTSTATUS Aw8671xSetBstVol(PDEVICE_CONTEXT DevContext, UCHAR BstVol)
{
    if (BstVol & 0xF0) {
        BstVol = 0x0F;
    }
    return AwWriteBits(DevContext, AW8671X_REG_PLAYCFG1,
        (UCHAR)AW8671X_BIT_PLAYCFG1_CP_CODE_MASK, BstVol);
}

static NTSTATUS Aw869xxSetBstVol(PDEVICE_CONTEXT DevContext, UCHAR BstVol)
{
    if (BstVol & 0xC0) {
        BstVol = 0x3F;
    }
    return AwWriteBits(DevContext, AW869XX_REG_PLAYCFG1,
        (UCHAR)AW869XX_BIT_PLAYCFG1_BST_VOUT_RDA_MASK, BstVol);
}

static NTSTATUS Aw869xSetBstVol(PDEVICE_CONTEXT DevContext, UCHAR BstVol)
{
    if (BstVol & 0xE0) {
        BstVol = 0x1F;
    }
    return AwWriteBits(DevContext, AW869X_REG_BSTDBG4,
        (UCHAR)AW869X_BIT_BSTDBG4_BSTVOL_MASK, (UCHAR)(BstVol << 1));
}

static NTSTATUS Aw8692xMiscInit(PDEVICE_CONTEXT DevContext)
{
    NTSTATUS Status;
    const AW_HAPTIC_SETTINGS* S = &DevContext->Settings;

    Status = Aw8692xSetBstVol(DevContext, S->BstVolDefault);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8692X_REG_CONTCFG1,
        (UCHAR)AW8692X_BIT_CONTCFG1_BRK_BST_MD_MASK, (UCHAR)(S->BrkBstMd << 6));
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8692X_REG_CONTCFG5,
        0x00,
        (UCHAR)((S->ContBstBrkGain << 4) | S->ContBrkGain));
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8692X_REG_CONTCFG13,
        0x00,
        (UCHAR)((S->ContTset << 4) | S->ContBemfSet));
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW8692X_REG_CONTCFG10, S->ContBrkTime);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8692X_REG_DETCFG2,
        (UCHAR)AW8692X_BIT_DETCFG2_D2S_GAIN_MASK, S->D2sGain);
    return Status;
}

static NTSTATUS Aw8671xMiscInit(PDEVICE_CONTEXT DevContext)
{
    NTSTATUS Status;
    const AW_HAPTIC_SETTINGS* S = &DevContext->Settings;

    Status = Aw8671xSetBstVol(DevContext, S->BstVolDefault);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8671X_REG_DETCFG3,
        (UCHAR)AW8671X_BIT_DETCFG3_D2S_GAIN_MASK, S->D2sGain);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW8671X_REG_CONTCFG10, S->ContBrkTime);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8671X_REG_CONTCFG5,
        (UCHAR)AW8671X_BIT_CONTCFG5_BRK_GAIN_MASK, S->ContBrkGain);
    return Status;
}

static NTSTATUS Aw869xxMiscInit(PDEVICE_CONTEXT DevContext)
{
    NTSTATUS Status;

    Status = Aw869xxSetBstVol(DevContext, DevContext->Settings.BstVolDefault);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW869XX_REG_CONTCFG10, DevContext->Settings.ContBrkTime);
    return Status;
}

static NTSTATUS Aw869xSetPref0(PDEVICE_CONTEXT DevContext)
{
    UCHAR Buffer[2];
    Buffer[0] = (UCHAR)((DevContext->Settings.F0Pre >> 8) & 0xFF);
    Buffer[1] = (UCHAR)(DevContext->Settings.F0Pre & 0xFF);
    return AwWriteBytes(DevContext, AW869X_REG_F_PRE_H, Buffer, sizeof(Buffer));
}

static NTSTATUS Aw869xMiscInit(PDEVICE_CONTEXT DevContext)
{
    NTSTATUS Status;
    const AW_HAPTIC_SETTINGS* S = &DevContext->Settings;

    Status = AwWriteBytes(DevContext, AW869X_REG_BSTDBG1, S->BstDbg, sizeof(S->BstDbg));
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW869X_REG_TSET, S->Tset);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW869X_REG_R_SPARE, S->RSpare);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBytes(DevContext, AW869X_REG_BEMF_VTHH_H, S->BemfConfig, sizeof(S->BemfConfig));
    if (!NT_SUCCESS(Status)) return Status;
    Status = Aw869xSetPref0(DevContext);
    if (!NT_SUCCESS(Status)) return Status;
    Status = Aw869xSetBstVol(DevContext, S->MaxBstVol);
    return Status;
}

static NTSTATUS AwDoInitializeFamily(PDEVICE_CONTEXT DevContext)
{
    switch (DevContext->Family) {
    case AwHapticFamily8692x:
        return Aw8692xMiscInit(DevContext);
    case AwHapticFamily8671x:
        return Aw8671xMiscInit(DevContext);
    case AwHapticFamily869xx:
        return Aw869xxMiscInit(DevContext);
    case AwHapticFamily869x:
        return Aw869xMiscInit(DevContext);
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS Aw8692xStop(PDEVICE_CONTEXT DevContext)
{
    NTSTATUS Status;
    UCHAR Stop = AW8692X_BIT_PLAYCFG4_STOP_ON;

    Status = AwWriteByte(DevContext, AW8692X_REG_PLAYCFG4, Stop);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWaitStandbyCommon(DevContext, AW8692X_REG_GLBRD5);
    if (!NT_SUCCESS(Status)) {
        Status = AwWriteBits(DevContext, AW8692X_REG_SYSCTRL3,
            (UCHAR)AW8692X_BIT_SYSCTRL3_STANDBY_MASK, AW8692X_BIT_SYSCTRL3_STANDBY_ON);
        if (!NT_SUCCESS(Status)) return Status;
        return AwWriteBits(DevContext, AW8692X_REG_SYSCTRL3,
            (UCHAR)AW8692X_BIT_SYSCTRL3_STANDBY_MASK, AW8692X_BIT_SYSCTRL3_STANDBY_OFF);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS Aw8671xStop(PDEVICE_CONTEXT DevContext)
{
    NTSTATUS Status;
    UCHAR Stop = AW8671X_BIT_PLAYCFG4_STOP_ON;

    Status = AwWriteByte(DevContext, AW8671X_REG_PLAYCFG4, Stop);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWaitStandbyCommon(DevContext, AW8671X_REG_GLBRD5);
    if (!NT_SUCCESS(Status)) {
        Status = AwWriteBits(DevContext, AW8671X_REG_SYSCTRL2,
            (UCHAR)AW8671X_BIT_SYSCTRL2_STANDBY_MASK, AW8671X_BIT_SYSCTRL2_STANDBY_ON);
        if (!NT_SUCCESS(Status)) return Status;
        return AwWriteBits(DevContext, AW8671X_REG_SYSCTRL2,
            (UCHAR)AW8671X_BIT_SYSCTRL2_STANDBY_MASK, AW8671X_BIT_SYSCTRL2_STANDBY_OFF);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS Aw869xxStop(PDEVICE_CONTEXT DevContext)
{
    NTSTATUS Status;
    UCHAR Stop = AW869XX_BIT_PLAYCFG4_STOP_ON;

    Status = AwWriteByte(DevContext, AW869XX_REG_PLAYCFG4, Stop);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWaitStandbyCommon(DevContext, AW869XX_REG_GLBRD5);
    if (!NT_SUCCESS(Status)) {
        Status = AwWriteBits(DevContext, AW869XX_REG_SYSCTRL2,
            (UCHAR)AW869XX_BIT_SYSCTRL2_STANDBY_MASK, AW869XX_BIT_SYSCTRL2_STANDBY_ON);
        if (!NT_SUCCESS(Status)) return Status;
        return AwWriteBits(DevContext, AW869XX_REG_SYSCTRL2,
            (UCHAR)AW869XX_BIT_SYSCTRL2_STANDBY_MASK, AW869XX_BIT_SYSCTRL2_STANDBY_OFF);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS Aw869xStopInternal(PDEVICE_CONTEXT DevContext)
{
    NTSTATUS Status;
    UCHAR Value = AW869X_BIT_GO_DISABLE;
    UCHAR RegValue = 0;
    ULONG Count;

    Status = AwWriteByte(DevContext, AW869X_REG_GO, Value);
    if (!NT_SUCCESS(Status)) return Status;

    for (Count = 0; Count < AW_STANDBY_RETRIES; ++Count) {
        Status = AwReadByte(DevContext, AW869X_REG_GLB_STATE, &RegValue);
        if (!NT_SUCCESS(Status)) return Status;
        if ((RegValue & 0x0F) == AW869X_BIT_GLB_STATE_STANDBY) {
            break;
        }
        AwSleepUs(2500);
    }

    Status = AwWriteBits(DevContext, AW869X_REG_SYSINTM,
        (UCHAR)AW869X_BIT_SYSINTM_UVLO_MASK, AW869X_BIT_SYSINTM_UVLO_OFF);
    if (!NT_SUCCESS(Status)) return Status;
    return AwWriteBits(DevContext, AW869X_REG_SYSCTRL,
        (UCHAR)AW869X_BIT_SYSCTRL_WORK_MODE_MASK, AW869X_BIT_SYSCTRL_STANDBY);
}

NTSTATUS
AW8624Stop(
    PDEVICE_CONTEXT DevContext
)
{
    if (DevContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    switch (DevContext->Family) {
    case AwHapticFamily8692x:
        return Aw8692xStop(DevContext);
    case AwHapticFamily8671x:
        return Aw8671xStop(DevContext);
    case AwHapticFamily869xx:
        return Aw869xxStop(DevContext);
    case AwHapticFamily869x:
        return Aw869xStopInternal(DevContext);
    default:
        return STATUS_DEVICE_NOT_READY;
    }
}

static NTSTATUS Aw8692xPlayContinuous(PDEVICE_CONTEXT DevContext, ULONG Intensity)
{
    NTSTATUS Status;
    UCHAR Gain = AwScaleU8(AW_GAIN_MAX, Intensity);
    UCHAR Drv1 = AwScaleU8(DevContext->Settings.ContDrv1Lvl, Intensity);
    UCHAR Drv2 = AwScaleNonZeroU8(DevContext->Settings.ContDrv2Lvl, Intensity);
    UCHAR BstVol = AwScaleNonZeroU8(DevContext->Settings.BstVolDefault, Intensity);
    UCHAR Drv2Time = 0xFF;

#ifdef DEBUG
    Trace(TRACE_LEVEL_INFORMATION, TRACE_HAPTICS,
        "%!FUNC!: intensity=%lu gain=0x%02X drv1=0x%02X drv2=0x%02X bst=0x%02X",
        Intensity, Gain, Drv1, Drv2, BstVol);
#endif

    Status = Aw8692xSetBstVol(DevContext, BstVol);
    if (!NT_SUCCESS(Status)) return Status;
    Status = Aw8692xSetGain(DevContext, Gain);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8692X_REG_PLAYCFG3,
        (UCHAR)AW8692X_BIT_PLAYCFG3_PLAY_MODE_MASK, AW8692X_BIT_PLAYCFG3_PLAY_MODE_CONT);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8692X_REG_PLAYCFG3,
        (UCHAR)AW8692X_BIT_PLAYCFG3_BRK_EN_MASK, AW8692X_BIT_PLAYCFG3_BRK_ENABLE);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8692X_REG_PLAYCFG1,
        (UCHAR)AW8692X_BIT_PLAYCFG1_BST_MODE_MASK, AW8692X_BIT_PLAYCFG1_BST_MODE_BYPASS);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8692X_REG_VBATCTRL,
        (UCHAR)AW8692X_BIT_VBATCTRL_VBAT_MODE_MASK, AW8692X_BIT_VBATCTRL_VBAT_MODE_HW);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8692X_REG_CONTCFG6,
        (UCHAR)AW8692X_BIT_CONTCFG6_TRACK_EN_MASK,
        DevContext->Settings.TrackEnable ? AW8692X_BIT_CONTCFG6_TRACK_ENABLE : 0);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8692X_REG_CONTCFG6,
        (UCHAR)AW8692X_BIT_CONTCFG6_DRV1_LVL_MASK, Drv1);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW8692X_REG_CONTCFG7, Drv2);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW8692X_REG_CONTCFG9, Drv2Time);
    if (!NT_SUCCESS(Status)) return Status;
    return AwWriteByte(DevContext, AW8692X_REG_PLAYCFG4, AW8692X_BIT_PLAYCFG4_GO_ON);
}

static NTSTATUS Aw8671xPlayContinuous(PDEVICE_CONTEXT DevContext, ULONG Intensity)
{
    NTSTATUS Status;
    UCHAR Gain = AwScaleU8(AW_GAIN_MAX, Intensity);
    UCHAR Drv1 = AwScaleU8(DevContext->Settings.ContDrv1Lvl, Intensity);
    UCHAR Drv2 = AwScaleNonZeroU8(DevContext->Settings.ContDrv2Lvl, Intensity);
    UCHAR BstVol = AwScaleNonZeroU8(DevContext->Settings.BstVolDefault, Intensity);
    UCHAR Combined = (UCHAR)((DevContext->Settings.TrackEnable ? 0x80 : 0x00) | (Drv1 & 0x7F));
    UCHAR Drv2Time = 0xFF;

#ifdef DEBUG
    Trace(TRACE_LEVEL_INFORMATION, TRACE_HAPTICS,
        "%!FUNC!: intensity=%lu gain=0x%02X drv1=0x%02X drv2=0x%02X bst=0x%02X",
        Intensity, Gain, Drv1, Drv2, BstVol);
#endif

    Status = Aw8671xSetBstVol(DevContext, BstVol);
    if (!NT_SUCCESS(Status)) return Status;
    Status = Aw8671xSetGain(DevContext, Gain);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8671X_REG_PLAYCFG3,
        (UCHAR)AW8671X_BIT_PLAYCFG3_PLAY_MODE_MASK, AW8671X_BIT_PLAYCFG3_PLAY_MODE_CONT);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8671X_REG_PLAYCFG3,
        (UCHAR)AW8671X_BIT_PLAYCFG3_BRK_EN_MASK, AW8671X_BIT_PLAYCFG3_BRK_ENABLE);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8671X_REG_PLAYCFG1,
        (UCHAR)AW8671X_BIT_PLAYCFG1_CP_MODE_MASK, AW8671X_BIT_PLAYCFG1_CP_MODE_BYPASS);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW8671X_REG_VBATCTRL,
        (UCHAR)AW8671X_BIT_VBATCTRL_VBAT_MODE_MASK, AW8671X_BIT_VBATCTRL_VBAT_MODE_HW);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW8671X_REG_CONTCFG6, Combined);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW8671X_REG_CONTCFG7, Drv2);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW8671X_REG_CONTCFG9, Drv2Time);
    if (!NT_SUCCESS(Status)) return Status;
    return AwWriteByte(DevContext, AW8671X_REG_PLAYCFG4, AW8671X_BIT_PLAYCFG4_GO_ON);
}

static NTSTATUS Aw869xxPlayContinuous(PDEVICE_CONTEXT DevContext, ULONG Intensity)
{
    NTSTATUS Status;
    UCHAR Gain = AwScaleU8(AW_GAIN_MAX, Intensity);
    UCHAR Drv1 = AwScaleU8(DevContext->Settings.ContDrv1Lvl, Intensity);
    UCHAR Drv2 = AwScaleNonZeroU8(DevContext->Settings.ContDrv2Lvl, Intensity);
    UCHAR BstVol = AwScaleNonZeroU8(DevContext->Settings.BstVolDefault, Intensity);
    UCHAR Combined = (UCHAR)((DevContext->Settings.TrackEnable ? 0x80 : 0x00) | (Drv1 & 0x7F));
    UCHAR Drv2Time = 0xFF;

#ifdef DEBUG
    Trace(TRACE_LEVEL_INFORMATION, TRACE_HAPTICS,
        "%!FUNC!: intensity=%lu gain=0x%02X drv1=0x%02X drv2=0x%02X bst=0x%02X",
        Intensity, Gain, Drv1, Drv2, BstVol);
#endif

    Status = Aw869xxSetBstVol(DevContext, BstVol);
    if (!NT_SUCCESS(Status)) return Status;
    Status = Aw869xxSetGain(DevContext, Gain);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW869XX_REG_PLAYCFG3,
        (UCHAR)AW869XX_BIT_PLAYCFG3_PLAY_MODE_MASK, AW869XX_BIT_PLAYCFG3_PLAY_MODE_CONT);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW869XX_REG_PLAYCFG3,
        (UCHAR)AW869XX_BIT_PLAYCFG3_BRK_EN_MASK, AW869XX_BIT_PLAYCFG3_BRK_ENABLE);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW869XX_REG_PLAYCFG1,
        (UCHAR)AW869XX_BIT_PLAYCFG1_BST_MODE_MASK, AW869XX_BIT_PLAYCFG1_BST_MODE_BYPASS);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW869XX_REG_CONTCFG6, Combined);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW869XX_REG_CONTCFG7, Drv2);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW869XX_REG_CONTCFG9, Drv2Time);
    if (!NT_SUCCESS(Status)) return Status;
    return AwWriteByte(DevContext, AW869XX_REG_PLAYCFG4, AW869XX_BIT_PLAYCFG4_GO_ON);
}

static NTSTATUS Aw869xPlayContinuous(PDEVICE_CONTEXT DevContext, ULONG Intensity)
{
    NTSTATUS Status;
    UCHAR Gain = AwScaleU8(AW_GAIN_MAX, Intensity);
    UCHAR BstVol = AwScaleNonZeroU8(DevContext->Settings.MaxBstVol, Intensity);
    UCHAR DrvLevel[2];
    UCHAR Td[2];
    UCHAR Zc[2];
    UCHAR TimeNzc = 0x23;

    DrvLevel[0] = AwScaleU8(DevContext->Settings.ContDrvLvl, Intensity);
    DrvLevel[1] = AwScaleU8(DevContext->Settings.ContDrvLvlOv, Intensity);
    Td[0] = (UCHAR)((DevContext->Settings.ContTd >> 8) & 0xFF);
    Td[1] = (UCHAR)(DevContext->Settings.ContTd & 0xFF);
    Zc[0] = (UCHAR)((DevContext->Settings.ContZcThr >> 8) & 0xFF);
    Zc[1] = (UCHAR)(DevContext->Settings.ContZcThr & 0xFF);

#ifdef DEBUG
    Trace(TRACE_LEVEL_INFORMATION, TRACE_HAPTICS,
        "%!FUNC!: intensity=%lu gain=0x%02X drv1=0x%02X drv2=0x%02X bst=0x%02X",
        Intensity, Gain, DrvLevel[0], DrvLevel[1], BstVol);
#endif

    Status = Aw869xSetBstVol(DevContext, BstVol);
    if (!NT_SUCCESS(Status)) return Status;
    Status = Aw869xSetGain(DevContext, Gain);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW869X_REG_SYSCTRL,
        (UCHAR)(AW869X_BIT_SYSCTRL_PLAY_MODE_MASK & AW869X_BIT_SYSCTRL_BST_MODE_MASK),
        (UCHAR)(AW869X_BIT_SYSCTRL_PLAY_MODE_CONT | AW869X_BIT_SYSCTRL_BST_MODE_BYPASS));
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW869X_REG_SYSCTRL,
        (UCHAR)AW869X_BIT_SYSCTRL_WORK_MODE_MASK, AW869X_BIT_SYSCTRL_ACTIVE);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW869X_REG_SYSINTM,
        (UCHAR)AW869X_BIT_SYSINTM_UVLO_MASK, AW869X_BIT_SYSINTM_UVLO_EN);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW869X_REG_DATCTRL,
        (UCHAR)(AW869X_BIT_DATCTRL_FC_MASK & AW869X_BIT_DATCTRL_LPF_ENABLE_MASK),
        (UCHAR)(AW869X_BIT_DATCTRL_FC_1000HZ | AW869X_BIT_DATCTRL_LPF_ENABLE));
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW869X_REG_CONT_CTRL,
        0x00,
        (UCHAR)(AW869X_BIT_CONT_CTRL_ZC_DETEC_ENABLE |
            AW869X_BIT_CONT_CTRL_WAIT_1PERIOD |
            AW869X_BIT_CONT_CTRL_BY_GO_SIGNAL |
            AW869X_BIT_CONT_CTRL_CLOSE_PLAYBACK |
            AW869X_BIT_CONT_CTRL_F0_DETECT_DISABLE |
            AW869X_BIT_CONT_CTRL_O2C_DISABLE |
            AW869X_BIT_CONT_CTRL_AUTO_BRK_ENABLE));
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBytes(DevContext, AW869X_REG_TD_H, Td, sizeof(Td));
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW869X_REG_TSET, DevContext->Settings.Tset);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBytes(DevContext, AW869X_REG_ZC_THRSH_H, Zc, sizeof(Zc));
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBits(DevContext, AW869X_REG_BEMF_NUM,
        (UCHAR)AW869X_BIT_BEMF_NUM_BRK_MASK, DevContext->Settings.ContNumBrk);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteByte(DevContext, AW869X_REG_TIME_NZC, TimeNzc);
    if (!NT_SUCCESS(Status)) return Status;
    Status = AwWriteBytes(DevContext, AW869X_REG_DRV_LVL, DrvLevel, sizeof(DrvLevel));
    if (!NT_SUCCESS(Status)) return Status;
    return AwWriteByte(DevContext, AW869X_REG_GO, AW869X_BIT_GO_ENABLE);
}

NTSTATUS
AW8624VibrateUntilStopped(
    PDEVICE_CONTEXT DevContext,
    ULONG Intensity
)
{
    NTSTATUS Status;

    if (DevContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!DevContext->HapticsInitialized) {
        Status = AW8624Initialize(DevContext);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    Status = AW8624Stop(DevContext);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    switch (DevContext->Family) {
    case AwHapticFamily8692x:
        Status = Aw8692xPlayContinuous(DevContext, Intensity);
        break;
    case AwHapticFamily8671x:
        Status = Aw8671xPlayContinuous(DevContext, Intensity);
        break;
    case AwHapticFamily869xx:
        Status = Aw869xxPlayContinuous(DevContext, Intensity);
        break;
    case AwHapticFamily869x:
        Status = Aw869xPlayContinuous(DevContext, Intensity);
        break;
    default:
        Status = STATUS_DEVICE_NOT_READY;
        break;
    }

#ifdef DEBUG
    Trace(TRACE_LEVEL_INFORMATION, TRACE_HAPTICS,
        "%!FUNC!: intensity=%lu family=%lu chipId=0x%04X status=%!STATUS!",
        Intensity,
        (ULONG)DevContext->Family,
        DevContext->ChipId,
        Status);
#endif

    return Status;
}

NTSTATUS
AW8624Start(
    PDEVICE_CONTEXT DevContext
)
{
    NTSTATUS Status;
    BOOLEAN ShouldPulse;

    if (DevContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (!DevContext->HapticsInitialized) {
        Status = AW8624Initialize(DevContext);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    if (DevContext->BlinkTimer != NULL) {
        WdfTimerStop(DevContext->BlinkTimer, FALSE);
    }

    ShouldPulse = (DevContext->StartupPulseDone == FALSE);
    if (!ShouldPulse) {
#ifdef DEBUG
        Trace(TRACE_LEVEL_INFORMATION, TRACE_HAPTICS,
            "%!FUNC!: startup pulse already completed, skipping family=%lu chipId=0x%04X",
            (ULONG)DevContext->Family,
            DevContext->ChipId);
#endif
        return STATUS_SUCCESS;
    }

    Status = AW8624VibrateUntilStopped(DevContext, 50);
    if (NT_SUCCESS(Status)) {
        AwSleepMs(666);
        Status = AW8624Stop(DevContext);
        if (NT_SUCCESS(Status)) {
            DevContext->StartupPulseDone = TRUE;
        }
    }

#ifdef DEBUG
    Trace(TRACE_LEVEL_INFORMATION, TRACE_HAPTICS,
        "%!FUNC!: start-intensity=%lu durationMs=%lu family=%lu chipId=0x%04X status=%!STATUS!",
        50UL,
        666UL,
        (ULONG)DevContext->Family,
        DevContext->ChipId,
        Status);
#endif

    return Status;
}

NTSTATUS
AW8624Initialize(
    PDEVICE_CONTEXT DevContext
)
{
    NTSTATUS Status;

    if (DevContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Status = AwReadChipId(DevContext);
    if (!NT_SUCCESS(Status)) {
#ifdef DEBUG
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "%!FUNC!: AwReadChipId failed %!STATUS!", Status);
#endif
        return Status;
    }

#ifdef DEBUG
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "%!FUNC!: detected chipId=0x%04X family=%lu before reset",
        DevContext->ChipId,
        (ULONG)DevContext->Family);
#endif

    Status = AwSoftReset(DevContext);
    if (!NT_SUCCESS(Status)) {
#ifdef DEBUG
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "%!FUNC!: AwSoftReset failed %!STATUS!", Status);
#endif
        return Status;
    }

    Status = AwReadChipId(DevContext);
    if (!NT_SUCCESS(Status)) {
#ifdef DEBUG
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "%!FUNC!: chip re-read failed %!STATUS!", Status);
#endif
        return Status;
    }

    AwLoadNx729jLeftSettings(DevContext);
    Status = AwDoInitializeFamily(DevContext);
    if (!NT_SUCCESS(Status)) {
#ifdef DEBUG
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "%!FUNC!: AwDoInitializeFamily failed %!STATUS!", Status);
#endif
        return Status;
    }

    DevContext->HapticsInitialized = TRUE;

#ifdef DEBUG
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "%!FUNC!: compatible=awinic,haptic_hv_l chipId=0x%04X family=%lu interruptPresent=%lu",
        DevContext->ChipId,
        (ULONG)DevContext->Family,
        DevContext->InterruptPresent ? 1UL : 0UL);
#endif

    return STATUS_SUCCESS;
}
