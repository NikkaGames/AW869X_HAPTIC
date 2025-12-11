/*++
	Copyright (c) Microsoft Corporation. All Rights Reserved.
	Copyright (c) Bingxing Wang. All Rights Reserved.
	Copyright (c) LumiaWoA authors. All Rights Reserved.
	Copyright (c) DuoWoA authors. All Rights Reserved.
	Copyright (c) Aistop. All Rights Reserved.

	SPDX-License-Identifier: BSD-3-Clause

	Module Name:

		aw8624.c

	Abstract:

		The core haptics driver, responsible for 
		communicating with the haptics controller.

	Environment:

		Kernel mode

--*/

#include "Driver.h"
#include "Controller.h"
#include "aw8624.h"

#ifdef DEBUG
#include "aw8624.tmh"
#endif

#define AW8624ReadRegWithCheck(Context, Address, Data, Length)	\
	Status = AW8624SpbRead(Context, Address, Data, Length);		\
	if (!NT_SUCCESS(Status))									\
	{															\
		return Status;											\
	}

#define AW8624WriteRegWithCheck(Context, Address, Data)			\
	Status = AW8624SpbWrite(Context, Address, Data);			\
	if (!NT_SUCCESS(Status))									\
	{															\
		return Status;											\
	}

#define AW8624WriteBitsWithCheck(Context, Address, Mask, Data)	\
	Status = AW8624WriteBits(Context, Address, Mask, Data);		\
	if (!NT_SUCCESS(Status))									\
	{															\
		return Status;											\
	}

NTSTATUS
AW8624SpbRead(
	PDEVICE_CONTEXT pDevice,
	UCHAR Address,
	PUINT16 Data,
	ULONG Length
)
{
	NTSTATUS Status = STATUS_SUCCESS;

	Status = SpbReadDataSynchronously(&pDevice->I2CContext, Address, (PVOID)Data, Length);

#ifdef DEBUG
	if (!NT_SUCCESS(Status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error when reading data from the SPB device - 0x%08lX",
			Status);
	}
#endif

	return Status;
}

NTSTATUS
AW8624SpbWrite(
	PDEVICE_CONTEXT pDevice,
	UCHAR Address,
	UINT16 Data
)
{
	NTSTATUS Status = STATUS_SUCCESS;

	Status = SpbWriteDataSynchronously(&pDevice->I2CContext, Address, (PVOID)&Data, sizeof(Data));

#ifdef DEBUG
	if (!NT_SUCCESS(Status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error when writing data to the SPB device - 0x%08lX",
			Status);
	}
#endif

	return Status;
}

NTSTATUS
AW8624WriteBits(
	PDEVICE_CONTEXT pDevice,
	UCHAR Address,
	INT32 Mask,
	UINT8 Value
)
{
	UINT16 RegData = 0;
	NTSTATUS Status = STATUS_SUCCESS;

	AW8624ReadRegWithCheck(pDevice, Address, &RegData, sizeof(RegData));
	RegData &= Mask;
	RegData |= Value;
	AW8624WriteRegWithCheck(pDevice, Address, RegData);

	return Status;
}

NTSTATUS
AW8624Standby(
	PDEVICE_CONTEXT pDevice
)
{
	NTSTATUS Status = STATUS_SUCCESS;

	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_SYSINTM, AW8624_BIT_SYSINTM_UVLO_MASK, AW8624_BIT_SYSINTM_UVLO_OFF);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_SYSCTRL, AW8624_BIT_SYSCTRL_WORK_MODE_MASK, AW8624_BIT_SYSCTRL_STANDBY);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_DBGCTRL, AW8624_BIT_DBGCTRL_INTN_TRG_SEL_MASK, AW8624_BIT_DBGCTRL_TRG_SEL_ENABLE);

#ifdef DEBUG
	if (!NT_SUCCESS(Status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error when putting AW8624 to standby - 0x%08lX",
			Status);
	}
#endif

	return Status;
}

NTSTATUS
AW8624Activate(
	PDEVICE_CONTEXT pDevice
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	UINT16 RegData = 0;

	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_SYSCTRL, AW8624_BIT_SYSCTRL_WORK_MODE_MASK, AW8624_BIT_SYSCTRL_ACTIVE);
	AW8624ReadRegWithCheck(pDevice, AW8624_REG_SYSINT, &RegData, sizeof(RegData));
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_SYSINTM, AW8624_BIT_SYSINTM_UVLO_MASK, AW8624_BIT_SYSINTM_UVLO_EN);

#ifdef DEBUG
	if (!NT_SUCCESS(Status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error when trying to activate AW8624 - 0x%08lX",
			Status);
	}
#endif

	return Status;
}

NTSTATUS
AW8624RamMode(
	PDEVICE_CONTEXT pDevice
)
{
	NTSTATUS Status = STATUS_SUCCESS;

	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_SYSCTRL, AW8624_BIT_SYSCTRL_PLAY_MODE_MASK, AW8624_BIT_SYSCTRL_PLAY_MODE_RAM);

	Status = AW8624Activate(pDevice);

#ifdef DEBUG
	if (!NT_SUCCESS(Status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error when trying to switch AW8624 to RAM mode - 0x%08lX",
			Status);
	}
#endif

	return Status;
}

NTSTATUS
AW8624Stop(
	PDEVICE_CONTEXT pDevice
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	UINT16 RegData = 0;
	UINT8 Count = 100;

	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_GO, AW8624_BIT_GO_MASK, AW8624_BIT_GO_DISABLE);

	do
	{
		AW8624ReadRegWithCheck(pDevice, AW8624_REG_GLB_STATE, &RegData, sizeof(RegData));
		Count--;
	} while ((Count != 0) && ((RegData & 0x0F) != 0));

	Status = AW8624Standby(pDevice);
#ifdef DEBUG
	if (!NT_SUCCESS(Status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error when trying to stop AW8624 - 0x%08lX",
			Status);
	}
#endif
	return Status;
}

NTSTATUS
AW8624Start(
	PDEVICE_CONTEXT pDevice
)
{
	NTSTATUS Status = STATUS_SUCCESS;

	Status = AW8624Activate(pDevice);

	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_GO, AW8624_BIT_GO_MASK, AW8624_BIT_GO_ENABLE);

#ifdef DEBUG
	if (!NT_SUCCESS(Status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error when trying to start AW8624 - 0x%08lX",
			Status);
	}
#endif

	return Status;
}

NTSTATUS
AW8624VibrateUntilStopped(
	PDEVICE_CONTEXT pDevice,
	ULONG Intensity
)
{
	NTSTATUS Status = STATUS_SUCCESS;

	Intensity = Intensity * 0x9B / 100;
	if (Intensity > 0x9B)
		Intensity = 0x9B;

	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_SYSCTRL, AW8624_BIT_SYSCTRL_PLAY_MODE_MASK, AW8624_BIT_SYSCTRL_PLAY_MODE_CONT);
	Status = AW8624Activate(pDevice);

	if (!NT_SUCCESS(Status))
	{
		return Status;
	}
	
	// 0x754 is retrieved from the following formula: 0x3B9ACA00 / 0x802 / 0x104,
	// where 0x802 and 0x104 are from DTS (vib_f0_pre and vib_f0_coeff respectively)
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_F_PRE_H, (0x754 >> 8) & 0xFF);
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_F_PRE_L, 0x754 & 0xFF);

	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_DATCTRL, AW8624_BIT_DATCTRL_FC_MASK, AW8624_BIT_DATCTRL_FC_1000HZ);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_DATCTRL, AW8624_BIT_DATCTRL_LPF_ENABLE_MASK, AW8624_BIT_DATCTRL_LPF_ENABLE);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_CONT_CTRL, AW8624_BIT_CONT_CTRL_ZC_DETEC_MASK, AW8624_BIT_CONT_CTRL_ZC_DETEC_ENABLE);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_CONT_CTRL, AW8624_BIT_CONT_CTRL_WAIT_PERIOD_MASK, AW8624_BIT_CONT_CTRL_WAIT_1PERIOD);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_CONT_CTRL, AW8624_BIT_CONT_CTRL_MODE_MASK, AW8624_BIT_CONT_CTRL_BY_GO_SIGNAL);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_CONT_CTRL, AW8624_BIT_CONT_CTRL_EN_CLOSE_MASK, AW8624_BIT_CONT_CTRL_CLOSE_PLAYBACK);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_CONT_CTRL, AW8624_BIT_CONT_CTRL_F0_DETECT_MASK, AW8624_BIT_CONT_CTRL_F0_DETECT_DISABLE);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_CONT_CTRL, AW8624_BIT_CONT_CTRL_O2C_MASK, AW8624_BIT_CONT_CTRL_O2C_DISABLE);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_CONT_CTRL, AW8624_BIT_CONT_CTRL_AUTO_BRK_MASK, AW8624_BIT_CONT_CTRL_AUTO_BRK_ENABLE);

	// from DTS (vib_cont_td)
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_TD_H, 0xF06C >> 8);
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_TD_L, 0xF06C & 0xFF);

	// from DTS (vib_tset)
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_TSET, 0x11);

	// from DTS (vib_cont_zc_thr)
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_ZC_THRSH_H, 0x8F8 >> 8);
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_ZC_THRSH_L, 0x8F8 & 0xFF);

	// from DTS (vib_cont_num_brk)
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_CONT_CTRL, AW8624_BIT_BEMF_NUM_BRK_MASK, 0x03);

	AW8624WriteRegWithCheck(pDevice, AW8624_REG_TIME_NZC, 0x23);

	AW8624WriteRegWithCheck(pDevice, AW8624_REG_DRV_LVL, (UINT16)Intensity);

	// from DTS (vib_cont_drv_lvl_ov)
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_DRV_LVL_OV, 0x9B);

	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_GO, AW8624_BIT_GO_MASK, AW8624_BIT_GO_ENABLE);

	return Status;
}

NTSTATUS
AW8624HapticsInit(
	PDEVICE_CONTEXT pDevice
)
{
	NTSTATUS Status = STATUS_SUCCESS;

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_SPB, "%!FUNC!: Entry");
#endif

	Status = AW8624Standby(pDevice);

	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_PWMDBG, AW8624_BIT_PWMDBG_PWM_MODE_MASK, AW8624_BIT_PWMDBG_PWM_24K);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_DETCTRL, AW8624_BIT_DETCTRL_PROTECT_MASK, AW8624_BIT_DETCTRL_PROTECT_NO_ACTION);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_PWMPRC, AW8624_BIT_PWMPRC_PRC_EN_MASK, AW8624_BIT_PWMPRC_PRC_DISABLE);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_PRLVL, AW8624_BIT_PRLVL_PR_EN_MASK, AW8624_BIT_PRLVL_PR_DISABLE);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_ADCTEST, AW8624_BIT_DETCTRL_VBAT_MODE_MASK, AW8624_BIT_DETCTRL_VBAT_HW_COMP);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_R_SPARE, AW8624_BIT_R_SPARE_MASK, AW8624_BIT_R_SPARE_ENABLE);

	AW8624WriteRegWithCheck(pDevice, AW8624_REG_TRIM_LRA, 0x00);

	Status = AW8624Standby(pDevice);
	Status = AW8624RamMode(pDevice);
	Status = AW8624Stop(pDevice);

	// from DTS (vib_sw_brake)
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_SW_BRAKE, 0x2C);

	AW8624WriteRegWithCheck(pDevice, AW8624_REG_THRS_BRA_END, 0x00);

	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_WAVECTRL, AW8624_BIT_WAVECTRL_NUM_OV_DRIVER_MASK, AW8624_BIT_WAVECTRL_NUM_OV_DRIVER);

	// from DTS (vib_cont_zc_thr)
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_ZC_THRSH_L, 0x8F8);
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_ZC_THRSH_H, 0x8F8 >> 8);

	// from DTS (vib_tset)
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_TSET, 0x11);

	// from DTS (vib_bemf_config[0..3])
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_BEMF_VTHH_H, 0x00);
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_BEMF_VTHH_L, 0x08);
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_BEMF_VTHL_H, 0x03);
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_BEMF_VTHL_L, 0xF8);

	return Status;
}

NTSTATUS
AW8624SetupInterrupts(
	PDEVICE_CONTEXT pDevice
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	UINT16 RegData = 0;

#ifdef DEBUG
	Trace(TRACE_LEVEL_INFORMATION, TRACE_SPB, "%!FUNC!: Entry");
#endif

	AW8624ReadRegWithCheck(pDevice, AW8624_REG_SYSINT, &RegData, sizeof(RegData));

	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_DBGCTRL, AW8624_BIT_DBGCTRL_INTMODE_MASK, AW8624_BIT_DBGCTRL_INTN_EDGE_MODE);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_SYSINTM, AW8624_BIT_SYSINTM_UVLO_MASK, AW8624_BIT_SYSINTM_UVLO_EN);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_SYSINTM, AW8624_BIT_SYSINTM_OCD_MASK, AW8624_BIT_SYSINTM_OCD_EN);
	AW8624WriteBitsWithCheck(pDevice, AW8624_REG_SYSINTM, AW8624_BIT_SYSINTM_OT_MASK, AW8624_BIT_SYSINTM_OT_EN);

	return Status;
}

NTSTATUS
AW8624Initialize(
	PDEVICE_CONTEXT pDevice
)
{
	NTSTATUS Status = STATUS_SUCCESS;
#ifdef DEBUG
	UINT16 RegData = 0;

	Trace(TRACE_LEVEL_INFORMATION, TRACE_SPB, "%!FUNC!: Entry");

	AW8624ReadRegWithCheck(pDevice, AW8624_REG_ID, &RegData, sizeof(RegData));
	Trace(TRACE_LEVEL_INFORMATION, TRACE_SPB, "%!FUNC!: Chip ID = 0x%X", (RegData & 0xFF));
#endif

	// Soft reset
	AW8624WriteRegWithCheck(pDevice, AW8624_REG_ID, 0xAA);

	Status = AW8624SetupInterrupts(pDevice);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	Status = AW8624HapticsInit(pDevice);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	return Status;
}