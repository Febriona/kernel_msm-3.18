/*
 * Copyright (c) 2009-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/*
 * SOC Info Routines
 *
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/export.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sys_soc.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/system_misc.h>

#include <soc/qcom/socinfo.h>
#include <soc/qcom/smem.h>
#include <soc/qcom/boot_stats.h>

#define BUILD_ID_LENGTH 32
#define SMEM_IMAGE_VERSION_BLOCKS_COUNT 32
#define SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE 128
#define SMEM_IMAGE_VERSION_SIZE 4096
#define SMEM_IMAGE_VERSION_NAME_SIZE 75
#define SMEM_IMAGE_VERSION_VARIANT_SIZE 20
#define SMEM_IMAGE_VERSION_VARIANT_OFFSET 75
#define SMEM_IMAGE_VERSION_OEM_SIZE 32
#define SMEM_IMAGE_VERSION_OEM_OFFSET 96
#define SMEM_IMAGE_VERSION_PARTITION_APPS 10

enum {
	HW_PLATFORM_UNKNOWN = 0,
	HW_PLATFORM_SURF    = 1,
	HW_PLATFORM_FFA     = 2,
	HW_PLATFORM_FLUID   = 3,
	HW_PLATFORM_SVLTE_FFA	= 4,
	HW_PLATFORM_SVLTE_SURF	= 5,
	HW_PLATFORM_MTP_MDM = 7,
	HW_PLATFORM_MTP  = 8,
	HW_PLATFORM_LIQUID  = 9,
	/* Dragonboard platform id is assigned as 10 in CDT */
	HW_PLATFORM_DRAGON	= 10,
	HW_PLATFORM_QRD	= 11,
	HW_PLATFORM_HRD	= 13,
	HW_PLATFORM_DTV	= 14,
	HW_PLATFORM_RCM	= 21,
	HW_PLATFORM_STP = 23,
	HW_PLATFORM_SBC = 24,
	HW_PLATFORM_INVALID
};

const char *hw_platform[] = {
	[HW_PLATFORM_UNKNOWN] = "Unknown",
	[HW_PLATFORM_SURF] = "Surf",
	[HW_PLATFORM_FFA] = "FFA",
	[HW_PLATFORM_FLUID] = "Fluid",
	[HW_PLATFORM_SVLTE_FFA] = "SVLTE_FFA",
	[HW_PLATFORM_SVLTE_SURF] = "SLVTE_SURF",
	[HW_PLATFORM_MTP_MDM] = "MDM_MTP_NO_DISPLAY",
	[HW_PLATFORM_MTP] = "MTP",
	[HW_PLATFORM_RCM] = "RCM",
	[HW_PLATFORM_LIQUID] = "Liquid",
	[HW_PLATFORM_DRAGON] = "Dragon",
	[HW_PLATFORM_QRD] = "QRD",
	[HW_PLATFORM_HRD] = "HRD",
	[HW_PLATFORM_DTV] = "DTV",
	[HW_PLATFORM_STP] = "STP",
	[HW_PLATFORM_SBC] = "SBC",
};

enum {
	ACCESSORY_CHIP_UNKNOWN = 0,
	ACCESSORY_CHIP_CHARM = 58,
};

enum {
	PLATFORM_SUBTYPE_QRD = 0x0,
	PLATFORM_SUBTYPE_SKUAA = 0x1,
	PLATFORM_SUBTYPE_SKUF = 0x2,
	PLATFORM_SUBTYPE_SKUAB = 0x3,
	PLATFORM_SUBTYPE_SKUG = 0x5,
	PLATFORM_SUBTYPE_QRD_INVALID,
};

const char *qrd_hw_platform_subtype[] = {
	[PLATFORM_SUBTYPE_QRD] = "QRD",
	[PLATFORM_SUBTYPE_SKUAA] = "SKUAA",
	[PLATFORM_SUBTYPE_SKUF] = "SKUF",
	[PLATFORM_SUBTYPE_SKUAB] = "SKUAB",
	[PLATFORM_SUBTYPE_SKUG] = "SKUG",
	[PLATFORM_SUBTYPE_QRD_INVALID] = "INVALID",
};

enum {
	PLATFORM_SUBTYPE_UNKNOWN = 0x0,
	PLATFORM_SUBTYPE_CHARM = 0x1,
	PLATFORM_SUBTYPE_STRANGE = 0x2,
	PLATFORM_SUBTYPE_STRANGE_2A = 0x3,
	PLATFORM_SUBTYPE_INVALID,
};

const char *hw_platform_subtype[] = {
	[PLATFORM_SUBTYPE_UNKNOWN] = "Unknown",
	[PLATFORM_SUBTYPE_CHARM] = "charm",
	[PLATFORM_SUBTYPE_STRANGE] = "strange",
	[PLATFORM_SUBTYPE_STRANGE_2A] = "strange_2a,"
};

/* Used to parse shared memory.  Must match the modem. */
struct socinfo_v0_1 {
	uint32_t format;
	uint32_t id;
	uint32_t version;
	char build_id[BUILD_ID_LENGTH];
};

struct socinfo_v0_2 {
	struct socinfo_v0_1 v0_1;
	uint32_t raw_id;
	uint32_t raw_version;
};

struct socinfo_v0_3 {
	struct socinfo_v0_2 v0_2;
	uint32_t hw_platform;
};

struct socinfo_v0_4 {
	struct socinfo_v0_3 v0_3;
	uint32_t platform_version;
};

struct socinfo_v0_5 {
	struct socinfo_v0_4 v0_4;
	uint32_t accessory_chip;
};

struct socinfo_v0_6 {
	struct socinfo_v0_5 v0_5;
	uint32_t hw_platform_subtype;
};

struct socinfo_v0_7 {
	struct socinfo_v0_6 v0_6;
	uint32_t pmic_model;
	uint32_t pmic_die_revision;
};

struct socinfo_v0_8 {
	struct socinfo_v0_7 v0_7;
	uint32_t pmic_model_1;
	uint32_t pmic_die_revision_1;
	uint32_t pmic_model_2;
	uint32_t pmic_die_revision_2;
};

struct socinfo_v0_9 {
	struct socinfo_v0_8 v0_8;
	uint32_t foundry_id;
};

struct socinfo_v0_10 {
	struct socinfo_v0_9 v0_9;
	uint32_t serial_number;
};

struct socinfo_v0_11 {
	struct socinfo_v0_10 v0_10;
	uint32_t num_pmics;
	uint32_t pmic_array_offset;
};

static union {
	struct socinfo_v0_1 v0_1;
	struct socinfo_v0_2 v0_2;
	struct socinfo_v0_3 v0_3;
	struct socinfo_v0_4 v0_4;
	struct socinfo_v0_5 v0_5;
	struct socinfo_v0_6 v0_6;
	struct socinfo_v0_7 v0_7;
	struct socinfo_v0_8 v0_8;
	struct socinfo_v0_9 v0_9;
	struct socinfo_v0_10 v0_10;
	struct socinfo_v0_11 v0_11;
} *socinfo;

/* max socinfo format version supported */
#define MAX_SOCINFO_FORMAT SOCINFO_VERSION(0, 11)

static struct msm_soc_info cpu_of_id[] = {

	/* 7x01 IDs */
	[0]  = {MSM_CPU_UNKNOWN, "Unknown CPU"},
	[1]  = {MSM_CPU_7X01, "MSM7X01"},
	[16] = {MSM_CPU_7X01, "MSM7X01"},
	[17] = {MSM_CPU_7X01, "MSM7X01"},
	[18] = {MSM_CPU_7X01, "MSM7X01"},
	[19] = {MSM_CPU_7X01, "MSM7X01"},
	[23] = {MSM_CPU_7X01, "MSM7X01"},
	[25] = {MSM_CPU_7X01, "MSM7X01"},
	[26] = {MSM_CPU_7X01, "MSM7X01"},
	[32] = {MSM_CPU_7X01, "MSM7X01"},
	[33] = {MSM_CPU_7X01, "MSM7X01"},
	[34] = {MSM_CPU_7X01, "MSM7X01"},
	[35] = {MSM_CPU_7X01, "MSM7X01"},

	/* 7x25 IDs */
	[20] = {MSM_CPU_7X25, "MSM7X25"},
	[21] = {MSM_CPU_7X25, "MSM7X25"},
	[24] = {MSM_CPU_7X25, "MSM7X25"},
	[27] = {MSM_CPU_7X25, "MSM7X25"},
	[39] = {MSM_CPU_7X25, "MSM7X25"},
	[40] = {MSM_CPU_7X25, "MSM7X25"},
	[41] = {MSM_CPU_7X25, "MSM7X25"},
	[42] = {MSM_CPU_7X25, "MSM7X25"},
	[62] = {MSM_CPU_7X25, "MSM7X25"},
	[63] = {MSM_CPU_7X25, "MSM7X25"},
	[66] = {MSM_CPU_7X25, "MSM7X25"},


	/* 7x27 IDs */
	[43] = {MSM_CPU_7X27, "MSM7X27"},
	[44] = {MSM_CPU_7X27, "MSM7X27"},
	[61] = {MSM_CPU_7X27, "MSM7X27"},
	[67] = {MSM_CPU_7X27, "MSM7X27"},
	[68] = {MSM_CPU_7X27, "MSM7X27"},
	[69] = {MSM_CPU_7X27, "MSM7X27"},


	/* 8x50 IDs */
	[30] = {MSM_CPU_8X50, "MSM8X50"},
	[36] = {MSM_CPU_8X50, "MSM8X50"},
	[37] = {MSM_CPU_8X50, "MSM8X50"},
	[38] = {MSM_CPU_8X50, "MSM8X50"},

	/* 7x30 IDs */
	[59] = {MSM_CPU_7X30, "MSM7X30"},
	[60] = {MSM_CPU_7X30, "MSM7X30"},

	/* 8x55 IDs */
	[74] = {MSM_CPU_8X55, "MSM8X55"},
	[75] = {MSM_CPU_8X55, "MSM8X55"},
	[85] = {MSM_CPU_8X55, "MSM8X55"},

	/* 8x60 IDs */
	[70] = {MSM_CPU_8X60, "MSM8X60"},
	[71] = {MSM_CPU_8X60, "MSM8X60"},
	[86] = {MSM_CPU_8X60, "MSM8X60"},

	/* 8960 IDs */
	[87] = {MSM_CPU_8960, "MSM8960"},

	/* 7x25A IDs */
	[88] = {MSM_CPU_7X25A, "MSM7X25A"},
	[89] = {MSM_CPU_7X25A, "MSM7X25A"},
	[96] = {MSM_CPU_7X25A, "MSM7X25A"},

	/* 7x27A IDs */
	[90] = {MSM_CPU_7X27A, "MSM7X27A"},
	[91] = {MSM_CPU_7X27A, "MSM7X27A"},
	[92] = {MSM_CPU_7X27A, "MSM7X27A"},
	[97] = {MSM_CPU_7X27A, "MSM7X27A"},

	/* FSM9xxx ID */
	[94] = {FSM_CPU_9XXX, "FSM9XXX"},
	[95] = {FSM_CPU_9XXX, "FSM9XXX"},

	/*  7x25AA ID */
	[98] = {MSM_CPU_7X25AA, "MSM7X25AA"},
	[99] = {MSM_CPU_7X25AA, "MSM7X25AA"},
	[100] = {MSM_CPU_7X25AA, "MSM7X25AA"},

	/*  7x27AA ID */
	[101] = {MSM_CPU_7X27AA, "MSM7X27AA"},
	[102] = {MSM_CPU_7X27AA, "MSM7X27AA"},
	[103] = {MSM_CPU_7X27AA, "MSM7X27AA"},
	[136] = {MSM_CPU_7X27AA, "MSM7X27AA"},

	/* 9x15 ID */
	[104] = {MSM_CPU_9615, "MSM9615"},
	[105] = {MSM_CPU_9615, "MSM9615"},
	[106] = {MSM_CPU_9615, "MSM9615"},
	[107] = {MSM_CPU_9615, "MSM9615"},
	[171] = {MSM_CPU_9615, "MSM9615"},

	/* 8064 IDs */
	[109] = {MSM_CPU_8064, "APQ8064"},

	/* 8930 IDs */
	[116] = {MSM_CPU_8930, "MSM8930"},
	[117] = {MSM_CPU_8930, "MSM8930"},
	[118] = {MSM_CPU_8930, "MSM8930"},
	[119] = {MSM_CPU_8930, "MSM8930"},
	[179] = {MSM_CPU_8930, "MSM8930"},

	/* 8627 IDs */
	[120] = {MSM_CPU_8627, "MSM8627"},
	[121] = {MSM_CPU_8627, "MSM8627"},

	/* 8660A ID */
	[122] = {MSM_CPU_8960, "MSM8960"},

	/* 8260A ID */
	[123] = {MSM_CPU_8960, "MSM8960"},

	/* 8060A ID */
	[124] = {MSM_CPU_8960, "MSM8960"},

	/* 8974 IDs */
	[126] = {MSM_CPU_8974, "MSM8974"},
	[184] = {MSM_CPU_8974, "MSM8974"},
	[185] = {MSM_CPU_8974, "MSM8974"},
	[186] = {MSM_CPU_8974, "MSM8974"},

	/* 8974AA IDs */
	[208] = {MSM_CPU_8974PRO_AA, "MSM8974PRO-AA"},
	[211] = {MSM_CPU_8974PRO_AA, "MSM8974PRO-AA"},
	[214] = {MSM_CPU_8974PRO_AA, "MSM8974PRO-AA"},
	[217] = {MSM_CPU_8974PRO_AA, "MSM8974PRO-AA"},

	/* 8974AB IDs */
	[209] = {MSM_CPU_8974PRO_AB, "MSM8974PRO-AB"},
	[212] = {MSM_CPU_8974PRO_AB, "MSM8974PRO-AB"},
	[215] = {MSM_CPU_8974PRO_AB, "MSM8974PRO-AB"},
	[218] = {MSM_CPU_8974PRO_AB, "MSM8974PRO-AB"},

	/* 8974AC IDs */
	[194] = {MSM_CPU_8974PRO_AC, "MSM8974PRO-AC"},
	[210] = {MSM_CPU_8974PRO_AC, "MSM8974PRO-AC"},
	[213] = {MSM_CPU_8974PRO_AC, "MSM8974PRO-AC"},
	[216] = {MSM_CPU_8974PRO_AC, "MSM8974PRO-AC"},

	/* 8625 IDs */
	[127] = {MSM_CPU_8625, "MSM8625"},
	[128] = {MSM_CPU_8625, "MSM8625"},
	[129] = {MSM_CPU_8625, "MSM8625"},
	[137] = {MSM_CPU_8625, "MSM8625"},
	[167] = {MSM_CPU_8625, "MSM8625"},

	/* 8064 MPQ ID */
	[130] = {MSM_CPU_8064, "APQ8064"},

	/* 7x25AB IDs */
	[131] = {MSM_CPU_7X25AB, "MSM7X25AB"},
	[132] = {MSM_CPU_7X25AB, "MSM7X25AB"},
	[133] = {MSM_CPU_7X25AB, "MSM7X25AB"},
	[135] = {MSM_CPU_7X25AB, "MSM7X25AB"},

	/* 9625 IDs */
	[134] = {MSM_CPU_9625, "MSM9625"},
	[148] = {MSM_CPU_9625, "MSM9625"},
	[149] = {MSM_CPU_9625, "MSM9625"},
	[150] = {MSM_CPU_9625, "MSM9625"},
	[151] = {MSM_CPU_9625, "MSM9625"},
	[152] = {MSM_CPU_9625, "MSM9625"},
	[173] = {MSM_CPU_9625, "MSM9625"},
	[174] = {MSM_CPU_9625, "MSM9625"},
	[175] = {MSM_CPU_9625, "MSM9625"},

	/* 8960AB IDs */
	[138] = {MSM_CPU_8960AB, "MSM8960AB"},
	[139] = {MSM_CPU_8960AB, "MSM8960AB"},
	[140] = {MSM_CPU_8960AB, "MSM8960AB"},
	[141] = {MSM_CPU_8960AB, "MSM8960AB"},

	/* 8930AA IDs */
	[142] = {MSM_CPU_8930AA, "MSM8930AA"},
	[143] = {MSM_CPU_8930AA, "MSM8930AA"},
	[144] = {MSM_CPU_8930AA, "MSM8930AA"},
	[160] = {MSM_CPU_8930AA, "MSM8930AA"},
	[180] = {MSM_CPU_8930AA, "MSM8930AA"},

	/* 8226 IDs */
	[145] = {MSM_CPU_8226, "MSM8626"},
	[158] = {MSM_CPU_8226, "MSM8226"},
	[159] = {MSM_CPU_8226, "MSM8526"},
	[198] = {MSM_CPU_8226, "MSM8126"},
	[199] = {MSM_CPU_8226, "APQ8026"},
	[200] = {MSM_CPU_8226, "MSM8926"},
	[205] = {MSM_CPU_8226, "MSM8326"},
	[219] = {MSM_CPU_8226, "APQ8028"},
	[220] = {MSM_CPU_8226, "MSM8128"},
	[221] = {MSM_CPU_8226, "MSM8228"},
	[222] = {MSM_CPU_8226, "MSM8528"},
	[223] = {MSM_CPU_8226, "MSM8628"},
	[224] = {MSM_CPU_8226, "MSM8928"},

	/* 8610 IDs */
	[147] = {MSM_CPU_8610, "MSM8610"},
	[161] = {MSM_CPU_8610, "MSM8110"},
	[162] = {MSM_CPU_8610, "MSM8210"},
	[163] = {MSM_CPU_8610, "MSM8810"},
	[164] = {MSM_CPU_8610, "MSM8212"},
	[165] = {MSM_CPU_8610, "MSM8612"},
	[166] = {MSM_CPU_8610, "MSM8112"},
	[225] = {MSM_CPU_8610, "MSM8510"},
	[226] = {MSM_CPU_8610, "MSM8512"},

	/* 8064AB IDs */
	[153] = {MSM_CPU_8064AB, "APQ8064AB"},

	/* 8930AB IDs */
	[154] = {MSM_CPU_8930AB, "MSM8930AB"},
	[155] = {MSM_CPU_8930AB, "MSM8930AB"},
	[156] = {MSM_CPU_8930AB, "MSM8930AB"},
	[157] = {MSM_CPU_8930AB, "MSM8930AB"},
	[181] = {MSM_CPU_8930AB, "MSM8930AB"},

	/* 8625Q IDs */
	[168] = {MSM_CPU_8625Q, "MSM8225Q"},
	[169] = {MSM_CPU_8625Q, "MSM8625Q"},
	[170] = {MSM_CPU_8625Q, "MSM8125Q"},

	/* 8064AA IDs */
	[172] = {MSM_CPU_8064AA, "APQ8064AA"},

	/* 8084 IDs */
	[178] = {MSM_CPU_8084, "APQ8084"},

	/* 9630 IDs */
	[187] = {MSM_CPU_9630, "MDM9630"},
	[227] = {MSM_CPU_9630, "MDM9630"},
	[228] = {MSM_CPU_9630, "MDM9630"},
	[229] = {MSM_CPU_9630, "MDM9630"},
	[230] = {MSM_CPU_9630, "MDM9630"},
	[231] = {MSM_CPU_9630, "MDM9630"},

	/* FSM9900 ID */
	[188] = {FSM_CPU_9900, "FSM9900"},
	[189] = {FSM_CPU_9900, "FSM9900"},
	[190] = {FSM_CPU_9900, "FSM9900"},
	[191] = {FSM_CPU_9900, "FSM9900"},
	[192] = {FSM_CPU_9900, "FSM9900"},
	[193] = {FSM_CPU_9900, "FSM9900"},

	/* 8916 IDs */
	[206] = {MSM_CPU_8916, "MSM8916"},
	[247] = {MSM_CPU_8916, "APQ8016"},
	[248] = {MSM_CPU_8916, "MSM8216"},
	[249] = {MSM_CPU_8916, "MSM8116"},
	[250] = {MSM_CPU_8916, "MSM8616"},

	/* 8936 IDs */
	[233] = {MSM_CPU_8936, "MSM8936"},
	[240] = {MSM_CPU_8936, "APQ8036"},
	[242] = {MSM_CPU_8936, "MSM8236"},

	/* 8939 IDs */
	[239] = {MSM_CPU_8939, "MSM8939"},
	[241] = {MSM_CPU_8939, "APQ8039"},
	[263] = {MSM_CPU_8939, "MSM8239"},

	/* 8909 IDs */
	[245] = {MSM_CPU_8909, "MSM8909"},
	[258] = {MSM_CPU_8909, "MSM8209"},
	[259] = {MSM_CPU_8909, "MSM8208"},
	[265] = {MSM_CPU_8909, "APQ8009"},
	[260] = {MSM_CPU_8909, "MDMFERRUM"},
	[261] = {MSM_CPU_8909, "MDMFERRUM"},
	[262] = {MSM_CPU_8909, "MDMFERRUM"},

	/* ZIRC IDs */
	[234] = {MSM_CPU_ZIRC, "MSMZIRC"},
	[235] = {MSM_CPU_ZIRC, "MSMZIRC"},
	[236] = {MSM_CPU_ZIRC, "MSMZIRC"},
	[237] = {MSM_CPU_ZIRC, "MSMZIRC"},
	[238] = {MSM_CPU_ZIRC, "MSMZIRC"},

	/* 8994 ID */
	[207] = {MSM_CPU_8994, "MSM8994"},
	[253] = {MSM_CPU_8994, "APQ8094"},

	/* 8992 ID */
	[251] = {MSM_CPU_8992, "MSM8992"},

	/* FSM9010 ID */
	[254] = {FSM_CPU_9010, "FSM9010"},
	[255] = {FSM_CPU_9010, "FSM9010"},
	[256] = {FSM_CPU_9010, "FSM9010"},
	[257] = {FSM_CPU_9010, "FSM9010"},

	/* 8952 ID */
	[264] = {MSM_CPU_8952, "MSM8952"},
	[289] = {MSM_CPU_8952, "APQ8952"},

	/* 8996 IDs */
	[246] = {MSM_CPU_8996, "MSM8996"},
	[291] = {MSM_CPU_8996, "APQ8096"},

	/* 8976 ID */
	[266] = {MSM_CPU_8976, "MSM8976"},

	/* 8929 IDs */
	[268] = {MSM_CPU_8929, "MSM8929"},
	[269] = {MSM_CPU_8929, "MSM8629"},
	[270] = {MSM_CPU_8929, "MSM8229"},
	[271] = {MSM_CPU_8929, "APQ8029"},

	[293] = {MSM_CPU_TITANIUM, "MSMTITANIUM"},
	/* FERMIUM ID */
	[290] = {MSM_CPU_FERMIUM, "MDMFERMIUM"},
	[296] = {MSM_CPU_FERMIUM, "MDMFERMIUM"},
	[297] = {MSM_CPU_FERMIUM, "MDMFERMIUM"},
	[298] = {MSM_CPU_FERMIUM, "MDMFERMIUM"},
	[299] = {MSM_CPU_FERMIUM, "MDMFERMIUM"},

	/* Californium IDs */
	[283] = {MSM_CPU_CALIFORNIUM, "MDMCALIFORNIUM"},
	[284] = {MSM_CPU_CALIFORNIUM, "MDMCALIFORNIUM"},
	[285] = {MSM_CPU_CALIFORNIUM, "MDMCALIFORNIUM"},
	[286] = {MSM_CPU_CALIFORNIUM, "MDMCALIFORNIUM"},

	/*MSMTHORIUM ID  */
	[294] = {MSM_CPU_THORIUM, "MSMTHORIUM"},

	/* Uninitialized IDs are not known to run Linux.
	   MSM_CPU_UNKNOWN is set to 0 to ensure these IDs are
	   considered as unknown CPU. */
};

static enum msm_cpu cur_cpu;
static int current_image;
static uint32_t socinfo_format;

static struct socinfo_v0_1 dummy_socinfo = {
	.format = SOCINFO_VERSION(0, 1),
	.version = 1,
};

uint32_t socinfo_get_id(void)
{
	return (socinfo) ? socinfo->v0_1.id : 0;
}
EXPORT_SYMBOL_GPL(socinfo_get_id);

static char *socinfo_get_id_string(void)
{
	return (socinfo) ? cpu_of_id[socinfo->v0_1.id].soc_id_string : NULL;
}

uint32_t socinfo_get_version(void)
{
	return (socinfo) ? socinfo->v0_1.version : 0;
}

char *socinfo_get_build_id(void)
{
	return (socinfo) ? socinfo->v0_1.build_id : NULL;
}

static char *msm_read_hardware_id(void)
{
	static char msm_soc_str[256] = "Qualcomm Technologies, Inc ";
	static bool string_generated;
	int ret = 0;

	if (string_generated)
		return msm_soc_str;
	if (!socinfo)
		goto err_path;
	if (!cpu_of_id[socinfo->v0_1.id].soc_id_string)
		goto err_path;

	ret = strlcat(msm_soc_str, cpu_of_id[socinfo->v0_1.id].soc_id_string,
			sizeof(msm_soc_str));
	if (ret > sizeof(msm_soc_str))
		goto err_path;

	string_generated = true;
	return msm_soc_str;
err_path:
	return "UNKNOWN SOC TYPE";
}

uint32_t socinfo_get_raw_id(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 2) ?
			socinfo->v0_2.raw_id : 0)
		: 0;
}

uint32_t socinfo_get_raw_version(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 2) ?
			socinfo->v0_2.raw_version : 0)
		: 0;
}

uint32_t socinfo_get_platform_type(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 3) ?
			socinfo->v0_3.hw_platform : 0)
		: 0;
}


uint32_t socinfo_get_platform_version(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 4) ?
			socinfo->v0_4.platform_version : 0)
		: 0;
}

/* This information is directly encoded by the machine id */
/* Thus no external callers rely on this information at the moment */
static uint32_t socinfo_get_accessory_chip(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 5) ?
			socinfo->v0_5.accessory_chip : 0)
		: 0;
}

uint32_t socinfo_get_platform_subtype(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 6) ?
			socinfo->v0_6.hw_platform_subtype : 0)
		: 0;
}

static uint32_t socinfo_get_foundry_id(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 9) ?
			socinfo->v0_9.foundry_id : 0)
		: 0;
}

uint32_t socinfo_get_serial_number(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 10) ?
			socinfo->v0_10.serial_number : 0)
		: 0;
}
EXPORT_SYMBOL(socinfo_get_serial_number);

enum pmic_model socinfo_get_pmic_model(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 7) ?
			socinfo->v0_7.pmic_model : PMIC_MODEL_UNKNOWN)
		: PMIC_MODEL_UNKNOWN;
}

uint32_t socinfo_get_pmic_die_revision(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 7) ?
			socinfo->v0_7.pmic_die_revision : 0)
		: 0;
}

static char *socinfo_get_image_version_base_address(void)
{
	return smem_find(SMEM_IMAGE_VERSION_TABLE,
				SMEM_IMAGE_VERSION_SIZE, 0, SMEM_ANY_HOST_FLAG);
}

enum msm_cpu socinfo_get_msm_cpu(void)
{
	return cur_cpu;
}
EXPORT_SYMBOL_GPL(socinfo_get_msm_cpu);

static ssize_t
msm_get_vendor(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	return snprintf(buf, PAGE_SIZE, "Qualcomm\n");
}

static ssize_t
msm_get_raw_id(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_raw_id());
}

static ssize_t
msm_get_raw_version(struct device *dev,
		     struct device_attribute *attr,
		     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_raw_version());
}

static ssize_t
msm_get_build_id(struct device *dev,
		   struct device_attribute *attr,
		   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%-.32s\n",
			socinfo_get_build_id());
}

static ssize_t
msm_get_hw_platform(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	uint32_t hw_type;
	hw_type = socinfo_get_platform_type();

	return snprintf(buf, PAGE_SIZE, "%-.32s\n",
			hw_platform[hw_type]);
}

static ssize_t
msm_get_platform_version(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_platform_version());
}

static ssize_t
msm_get_accessory_chip(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_accessory_chip());
}

static ssize_t
msm_get_platform_subtype(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	uint32_t hw_subtype;
	hw_subtype = socinfo_get_platform_subtype();
	if (HW_PLATFORM_QRD == socinfo_get_platform_type()) {
		if (hw_subtype >= PLATFORM_SUBTYPE_QRD_INVALID) {
			pr_err("Invalid hardware platform sub type for qrd found\n");
			hw_subtype = PLATFORM_SUBTYPE_QRD_INVALID;
		}
		return snprintf(buf, PAGE_SIZE, "%-.32s\n",
					qrd_hw_platform_subtype[hw_subtype]);
	}

	return snprintf(buf, PAGE_SIZE, "%-.32s\n",
		hw_platform_subtype[hw_subtype]);
}

static ssize_t
msm_get_platform_subtype_id(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	uint32_t hw_subtype;
	hw_subtype = socinfo_get_platform_subtype();
	return snprintf(buf, PAGE_SIZE, "%u\n",
		hw_subtype);
}

static ssize_t
msm_get_foundry_id(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_foundry_id());
}

static ssize_t
msm_get_serial_number(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_serial_number());
}

static ssize_t
msm_get_pmic_model(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_pmic_model());
}

static ssize_t
msm_get_pmic_die_revision(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
			 socinfo_get_pmic_die_revision());
}

static ssize_t
msm_get_image_version(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	char *string_address;

	string_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(string_address)) {
		pr_err("Failed to get image version base address");
		return snprintf(buf, SMEM_IMAGE_VERSION_NAME_SIZE, "Unknown");
	}
	string_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	return snprintf(buf, SMEM_IMAGE_VERSION_NAME_SIZE, "%-.75s\n",
			string_address);
}

static ssize_t
msm_set_image_version(struct device *dev,
			struct device_attribute *attr,
			const char *buf,
			size_t count)
{
	char *store_address;

	if (current_image != SMEM_IMAGE_VERSION_PARTITION_APPS)
		return count;
	store_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(store_address)) {
		pr_err("Failed to get image version base address");
		return count;
	}
	store_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	snprintf(store_address, SMEM_IMAGE_VERSION_NAME_SIZE, "%-.75s", buf);
	return count;
}

static ssize_t
msm_get_image_variant(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	char *string_address;

	string_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(string_address)) {
		pr_err("Failed to get image version base address");
		return snprintf(buf, SMEM_IMAGE_VERSION_VARIANT_SIZE,
		"Unknown");
	}
	string_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	string_address += SMEM_IMAGE_VERSION_VARIANT_OFFSET;
	return snprintf(buf, SMEM_IMAGE_VERSION_VARIANT_SIZE, "%-.20s\n",
			string_address);
}

static ssize_t
msm_set_image_variant(struct device *dev,
			struct device_attribute *attr,
			const char *buf,
			size_t count)
{
	char *store_address;

	if (current_image != SMEM_IMAGE_VERSION_PARTITION_APPS)
		return count;
	store_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(store_address)) {
		pr_err("Failed to get image version base address");
		return count;
	}
	store_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	store_address += SMEM_IMAGE_VERSION_VARIANT_OFFSET;
	snprintf(store_address, SMEM_IMAGE_VERSION_VARIANT_SIZE, "%-.20s", buf);
	return count;
}

static ssize_t
msm_get_image_crm_version(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	char *string_address;

	string_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(string_address)) {
		pr_err("Failed to get image version base address");
		return snprintf(buf, SMEM_IMAGE_VERSION_OEM_SIZE, "Unknown");
	}
	string_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	string_address += SMEM_IMAGE_VERSION_OEM_OFFSET;
	return snprintf(buf, SMEM_IMAGE_VERSION_OEM_SIZE, "%-.32s\n",
			string_address);
}

static ssize_t
msm_set_image_crm_version(struct device *dev,
			struct device_attribute *attr,
			const char *buf,
			size_t count)
{
	char *store_address;

	if (current_image != SMEM_IMAGE_VERSION_PARTITION_APPS)
		return count;
	store_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(store_address)) {
		pr_err("Failed to get image version base address");
		return count;
	}
	store_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	store_address += SMEM_IMAGE_VERSION_OEM_OFFSET;
	snprintf(store_address, SMEM_IMAGE_VERSION_OEM_SIZE, "%-.32s", buf);
	return count;
}

static ssize_t
msm_get_image_number(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n",
			current_image);
}

static ssize_t
msm_select_image(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	int ret, digit;

	ret = kstrtoint(buf, 10, &digit);
	if (ret)
		return ret;
	if (0 <= digit && digit < SMEM_IMAGE_VERSION_BLOCKS_COUNT)
		current_image = digit;
	else
		current_image = 0;
	return count;
}

static ssize_t
msm_get_images(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int pos = 0;
	int image;
	char *image_address;

	image_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(image_address))
		return snprintf(buf, PAGE_SIZE, "Unavailable\n");

	*buf = '\0';
	for (image = 0; image < SMEM_IMAGE_VERSION_BLOCKS_COUNT; image++) {
		if (*image_address == '\0') {
			image_address += SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
			continue;
		}

		pos += snprintf(buf + pos, PAGE_SIZE - pos, "%d:\n",
				image);
		pos += snprintf(buf + pos, PAGE_SIZE - pos, "\tCRM:\t\t%-.75s\n",
				image_address);
		pos += snprintf(buf + pos, PAGE_SIZE - pos, "\tVariant:\t%-.20s\n",
				image_address + SMEM_IMAGE_VERSION_VARIANT_OFFSET);
		pos += snprintf(buf + pos, PAGE_SIZE - pos, "\tVersion:\t%-.32s\n\n",
				image_address + SMEM_IMAGE_VERSION_OEM_OFFSET);

		image_address += SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	}

	return pos;
}

static struct device_attribute msm_soc_attr_raw_version =
	__ATTR(raw_version, S_IRUGO, msm_get_raw_version,  NULL);

static struct device_attribute msm_soc_attr_raw_id =
	__ATTR(raw_id, S_IRUGO, msm_get_raw_id,  NULL);

static struct device_attribute msm_soc_attr_vendor =
	__ATTR(vendor, S_IRUGO, msm_get_vendor,  NULL);

static struct device_attribute msm_soc_attr_build_id =
	__ATTR(build_id, S_IRUGO, msm_get_build_id, NULL);

static struct device_attribute msm_soc_attr_hw_platform =
	__ATTR(hw_platform, S_IRUGO, msm_get_hw_platform, NULL);


static struct device_attribute msm_soc_attr_platform_version =
	__ATTR(platform_version, S_IRUGO,
			msm_get_platform_version, NULL);

static struct device_attribute msm_soc_attr_accessory_chip =
	__ATTR(accessory_chip, S_IRUGO,
			msm_get_accessory_chip, NULL);

static struct device_attribute msm_soc_attr_platform_subtype =
	__ATTR(platform_subtype, S_IRUGO,
			msm_get_platform_subtype, NULL);

/* Platform Subtype String is being deprecated. Use Platform
 * Subtype ID instead.
 */
static struct device_attribute msm_soc_attr_platform_subtype_id =
	__ATTR(platform_subtype_id, S_IRUGO,
			msm_get_platform_subtype_id, NULL);

static struct device_attribute msm_soc_attr_foundry_id =
	__ATTR(foundry_id, S_IRUGO,
			msm_get_foundry_id, NULL);

static struct device_attribute msm_soc_attr_serial_number =
	__ATTR(serial_number, S_IRUGO,
			msm_get_serial_number, NULL);

static struct device_attribute msm_soc_attr_pmic_model =
	__ATTR(pmic_model, S_IRUGO,
			msm_get_pmic_model, NULL);

static struct device_attribute msm_soc_attr_pmic_die_revision =
	__ATTR(pmic_die_revision, S_IRUGO,
			msm_get_pmic_die_revision, NULL);

static struct device_attribute image_version =
	__ATTR(image_version, S_IRUGO | S_IWUSR,
			msm_get_image_version, msm_set_image_version);

static struct device_attribute image_variant =
	__ATTR(image_variant, S_IRUGO | S_IWUSR,
			msm_get_image_variant, msm_set_image_variant);

static struct device_attribute image_crm_version =
	__ATTR(image_crm_version, S_IRUGO | S_IWUSR,
			msm_get_image_crm_version, msm_set_image_crm_version);

static struct device_attribute select_image =
	__ATTR(select_image, S_IRUGO | S_IWUSR,
			msm_get_image_number, msm_select_image);

static struct device_attribute images =
	__ATTR(images, S_IRUGO, msm_get_images, NULL);

static void * __init setup_dummy_socinfo(void)
{
	if (early_machine_is_apq8084()) {
		dummy_socinfo.id = 178;
		strlcpy(dummy_socinfo.build_id, "apq8084 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_mdm9630()) {
		dummy_socinfo.id = 187;
		strlcpy(dummy_socinfo.build_id, "mdm9630 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8909()) {
		dummy_socinfo.id = 245;
		strlcpy(dummy_socinfo.build_id, "msm8909 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8916()) {
		dummy_socinfo.id = 206;
		strlcpy(dummy_socinfo.build_id, "msm8916 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8939()) {
		dummy_socinfo.id = 239;
		strlcpy(dummy_socinfo.build_id, "msm8939 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8936()) {
		dummy_socinfo.id = 233;
		strlcpy(dummy_socinfo.build_id, "msm8936 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_mdm9640()) {
		dummy_socinfo.id = 238;
		strlcpy(dummy_socinfo.build_id, "mdm9640 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_mdmcalifornium()) {
		dummy_socinfo.id = 286;
		strlcpy(dummy_socinfo.build_id, "mdmcalifornium - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8994()) {
		dummy_socinfo.id = 207;
		strlcpy(dummy_socinfo.build_id, "msm8994 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8992()) {
		dummy_socinfo.id = 251;
		strlcpy(dummy_socinfo.build_id, "msm8992 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8976()) {
		dummy_socinfo.id = 266;
		strlcpy(dummy_socinfo.build_id, "msm8976 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8952()) {
		dummy_socinfo.id = 264;
		strlcpy(dummy_socinfo.build_id, "msm8952 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8996()) {
		dummy_socinfo.id = 246;
		strlcpy(dummy_socinfo.build_id, "msm8996 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8929()) {
		dummy_socinfo.id = 268;
		strlcpy(dummy_socinfo.build_id, "msm8929 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msmtitanium()) {
		dummy_socinfo.id = 293;
		strlcpy(dummy_socinfo.build_id, "msmtitanium - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_mdmfermium()) {
		dummy_socinfo.id = 290;
		strlcpy(dummy_socinfo.build_id, "mdmfermium - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msmthorium()) {
		dummy_socinfo.id = 294;
		strlcpy(dummy_socinfo.build_id, "msmthorium - ",
			sizeof(dummy_socinfo.build_id));
	}

	strlcat(dummy_socinfo.build_id, "Dummy socinfo",
		sizeof(dummy_socinfo.build_id));
	return (void *) &dummy_socinfo;
}

static void __init populate_soc_sysfs_files(struct device *msm_soc_device)
{
	device_create_file(msm_soc_device, &msm_soc_attr_vendor);
	device_create_file(msm_soc_device, &image_version);
	device_create_file(msm_soc_device, &image_variant);
	device_create_file(msm_soc_device, &image_crm_version);
	device_create_file(msm_soc_device, &select_image);
	device_create_file(msm_soc_device, &images);

	switch (socinfo_format) {
	case SOCINFO_VERSION(0, 11):
	case SOCINFO_VERSION(0, 10):
		 device_create_file(msm_soc_device,
					&msm_soc_attr_serial_number);
	case SOCINFO_VERSION(0, 9):
		 device_create_file(msm_soc_device,
					&msm_soc_attr_foundry_id);
	case SOCINFO_VERSION(0, 8):
	case SOCINFO_VERSION(0, 7):
		device_create_file(msm_soc_device,
					&msm_soc_attr_pmic_model);
		device_create_file(msm_soc_device,
					&msm_soc_attr_pmic_die_revision);
	case SOCINFO_VERSION(0, 6):
		device_create_file(msm_soc_device,
					&msm_soc_attr_platform_subtype);
		device_create_file(msm_soc_device,
					&msm_soc_attr_platform_subtype_id);
	case SOCINFO_VERSION(0, 5):
		device_create_file(msm_soc_device,
					&msm_soc_attr_accessory_chip);
	case SOCINFO_VERSION(0, 4):
		device_create_file(msm_soc_device,
					&msm_soc_attr_platform_version);
	case SOCINFO_VERSION(0, 3):
		device_create_file(msm_soc_device,
					&msm_soc_attr_hw_platform);
	case SOCINFO_VERSION(0, 2):
		device_create_file(msm_soc_device,
					&msm_soc_attr_raw_id);
		device_create_file(msm_soc_device,
					&msm_soc_attr_raw_version);
	case SOCINFO_VERSION(0, 1):
		device_create_file(msm_soc_device,
					&msm_soc_attr_build_id);
		break;
	default:
		pr_err("Unknown socinfo format: v%u.%u\n",
				SOCINFO_VERSION_MAJOR(socinfo_format),
				SOCINFO_VERSION_MINOR(socinfo_format));
		break;
	}

	return;
}

static void  __init soc_info_populate(struct soc_device_attribute *soc_dev_attr)
{
	uint32_t soc_version = socinfo_get_version();

	soc_dev_attr->soc_id   = kasprintf(GFP_KERNEL, "%d", socinfo_get_id());
	soc_dev_attr->family  =  "Snapdragon";
	soc_dev_attr->machine  = socinfo_get_id_string();
	soc_dev_attr->revision = kasprintf(GFP_KERNEL, "%u.%u",
			SOCINFO_VERSION_MAJOR(soc_version),
			SOCINFO_VERSION_MINOR(soc_version));
	return;

}

static int __init socinfo_init_sysfs(void)
{
	struct device *msm_soc_device;
	struct soc_device *soc_dev;
	struct soc_device_attribute *soc_dev_attr;

	if (!socinfo) {
		pr_err("No socinfo found!\n");
		return -ENODEV;
	}

	soc_dev_attr = kzalloc(sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr) {
		pr_err("Soc Device alloc failed!\n");
		return -ENOMEM;
	}

	soc_info_populate(soc_dev_attr);
	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR_OR_NULL(soc_dev)) {
		kfree(soc_dev_attr);
		 pr_err("Soc device register failed\n");
		 return -EIO;
	}

	msm_soc_device = soc_device_to_device(soc_dev);
	populate_soc_sysfs_files(msm_soc_device);
	return 0;
}

late_initcall(socinfo_init_sysfs);

static void socinfo_print(void)
{
	uint32_t f_maj = SOCINFO_VERSION_MAJOR(socinfo_format);
	uint32_t f_min = SOCINFO_VERSION_MINOR(socinfo_format);
	uint32_t v_maj = SOCINFO_VERSION_MAJOR(socinfo->v0_1.version);
	uint32_t v_min = SOCINFO_VERSION_MINOR(socinfo->v0_1.version);

	switch (socinfo_format) {
	case SOCINFO_VERSION(0, 1):
		pr_info("v%u.%u, id=%u, ver=%u.%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min);
		break;
	case SOCINFO_VERSION(0, 2):
		pr_info("v%u.%u, id=%u, ver=%u.%u, "
			 "raw_id=%u, raw_ver=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version);
		break;
	case SOCINFO_VERSION(0, 3):
		pr_info("v%u.%u, id=%u, ver=%u.%u, "
			 "raw_id=%u, raw_ver=%u, hw_plat=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform);
		break;
	case SOCINFO_VERSION(0, 4):
		pr_info("v%u.%u, id=%u, ver=%u.%u, "
			 "raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version);
		break;
	case SOCINFO_VERSION(0, 5):
		pr_info("v%u.%u, id=%u, ver=%u.%u, "
			 "raw_id=%u, raw_ver=%u, hw_plat=%u,  hw_plat_ver=%u\n"
			" accessory_chip=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip);
		break;
	case SOCINFO_VERSION(0, 6):
		pr_info("v%u.%u, id=%u, ver=%u.%u, "
			 "raw_id=%u, raw_ver=%u, hw_plat=%u,  hw_plat_ver=%u\n"
			" accessory_chip=%u hw_plat_subtype=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype);
		break;
	case SOCINFO_VERSION(0, 7):
	case SOCINFO_VERSION(0, 8):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision);
		break;
	case SOCINFO_VERSION(0, 9):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u foundry_id=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id);
		break;
	case SOCINFO_VERSION(0, 10):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u foundry_id=%u serial_number=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id,
			socinfo->v0_10.serial_number);
		break;
	case SOCINFO_VERSION(0, 11):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u foundry_id=%u serial_number=%u num_pmics=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id,
			socinfo->v0_10.serial_number,
			socinfo->v0_11.num_pmics);
		break;
	default:
		pr_err("Unknown format found: v%u.%u\n", f_maj, f_min);
		break;
	}
}

static void socinfo_select_format(void)
{
	uint32_t f_maj = SOCINFO_VERSION_MAJOR(socinfo->v0_1.format);
	uint32_t f_min = SOCINFO_VERSION_MINOR(socinfo->v0_1.format);

	if (f_maj != 0) {
		pr_err("Unsupported format v%u.%u. Falling back to dummy values.\n",
			f_maj, f_min);
		socinfo = setup_dummy_socinfo();
	}

	if (socinfo->v0_1.format > MAX_SOCINFO_FORMAT) {
		pr_warn("Unsupported format v%u.%u. Falling back to v%u.%u.\n",
			f_maj, f_min, SOCINFO_VERSION_MAJOR(MAX_SOCINFO_FORMAT),
			SOCINFO_VERSION_MINOR(MAX_SOCINFO_FORMAT));
		socinfo_format = MAX_SOCINFO_FORMAT;
	} else {
		socinfo_format = socinfo->v0_1.format;
	}
}

int __init socinfo_init(void)
{
	static bool socinfo_init_done;
	unsigned size;

	if (socinfo_init_done)
		return 0;

	socinfo = smem_get_entry(SMEM_HW_SW_BUILD_ID, &size, 0,
				 SMEM_ANY_HOST_FLAG);
	if (IS_ERR_OR_NULL(socinfo)) {
		pr_warn("Can't find SMEM_HW_SW_BUILD_ID; falling back on dummy values.\n");
		socinfo = setup_dummy_socinfo();
	}

	socinfo_select_format();

	WARN(!socinfo_get_id(), "Unknown SOC ID!\n");

	if (socinfo_get_id() >= ARRAY_SIZE(cpu_of_id))
		BUG_ON("New IDs added! ID => CPU mapping needs an update.\n");
	else
		cur_cpu = cpu_of_id[socinfo->v0_1.id].generic_soc_type;

	boot_stats_init();
	socinfo_print();
	arch_read_hardware_id = msm_read_hardware_id;
	socinfo_init_done = true;

	return 0;
}
subsys_initcall(socinfo_init);
