/* Copyright (c) 2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include "core.h"
#include "pdata.h"
#include "wcd9xxx-utils.h"
#include "wcd9335_registers.h"
#include "wcd9335_irq.h"
#include <asoc/wcd934x_registers.h>

/* wcd9335 interrupt table  */
static const struct intr_data wcd9335_intr_table[] = {
	{WCD9XXX_IRQ_SLIMBUS, false},
	{WCD9335_IRQ_MBHC_SW_DET, true},
	{WCD9335_IRQ_MBHC_BUTTON_PRESS_DET, true},
	{WCD9335_IRQ_MBHC_BUTTON_RELEASE_DET, true},
	{WCD9335_IRQ_MBHC_ELECT_INS_REM_DET, true},
	{WCD9335_IRQ_MBHC_ELECT_INS_REM_LEG_DET, true},
	{WCD9335_IRQ_FLL_LOCK_LOSS, false},
	{WCD9335_IRQ_HPH_PA_CNPL_COMPLETE, false},
	{WCD9335_IRQ_HPH_PA_CNPR_COMPLETE, false},
	{WCD9335_IRQ_EAR_PA_CNP_COMPLETE, false},
	{WCD9335_IRQ_LINE_PA1_CNP_COMPLETE, false},
	{WCD9335_IRQ_LINE_PA2_CNP_COMPLETE, false},
	{WCD9335_IRQ_LINE_PA3_CNP_COMPLETE, false},
	{WCD9335_IRQ_LINE_PA4_CNP_COMPLETE, false},
	{WCD9335_IRQ_HPH_PA_OCPL_FAULT, false},
	{WCD9335_IRQ_HPH_PA_OCPR_FAULT, false},
	{WCD9335_IRQ_EAR_PA_OCP_FAULT, false},
	{WCD9335_IRQ_SOUNDWIRE, false},
	{WCD9335_IRQ_VDD_DIG_RAMP_COMPLETE, false},
	{WCD9335_IRQ_RCO_ERROR, false},
	{WCD9335_IRQ_SVA_ERROR, false},
	{WCD9335_IRQ_MAD_AUDIO, false},
	{WCD9335_IRQ_MAD_BEACON, false},
	{WCD9335_IRQ_SVA_OUTBOX1, true},
	{WCD9335_IRQ_SVA_OUTBOX2, true},
	{WCD9335_IRQ_MAD_ULTRASOUND, false},
	{WCD9335_IRQ_VBAT_ATTACK, false},
	{WCD9335_IRQ_VBAT_RESTORE, false},
};

