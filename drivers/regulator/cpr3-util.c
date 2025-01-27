/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
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

/*
 * This file contains utility functions to be used by platform specific CPR3
 * regulator drivers.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "cpr3-regulator.h"

#define BYTES_PER_FUSE_ROW		8
#define MAX_FUSE_ROW_BIT		63

#define CPR3_CONSECUTIVE_UP_DOWN_MIN	0
#define CPR3_CONSECUTIVE_UP_DOWN_MAX	15
#define CPR3_UP_DOWN_THRESHOLD_MIN	0
#define CPR3_UP_DOWN_THRESHOLD_MAX	31
#define CPR3_STEP_QUOT_MIN		0
#define CPR3_STEP_QUOT_MAX		63
#define CPR3_IDLE_CLOCKS_MIN		0
#define CPR3_IDLE_CLOCKS_MAX		31

/* This constant has units of uV/mV so 1000 corresponds to 100%. */
#define CPR3_AGING_DERATE_UNITY		1000

/**
 * cpr3_allocate_regulators() - allocate and initialize CPR3 regulators for a
 *		given thread based upon device tree data
 * @thread:		Pointer to the CPR3 thread
 *
 * This function allocates the thread->vreg array based upon the number of
 * device tree regulator subnodes.  It also initializes generic elements of each
 * regulator struct such as name, of_node, and thread.
 *
 * Return: 0 on success, errno on failure
 */
static int cpr3_allocate_regulators(struct cpr3_thread *thread)
{
	struct device_node *node;
	int i, rc;

	thread->vreg_count = 0;

	for_each_available_child_of_node(thread->of_node, node) {
		thread->vreg_count++;
	}

	thread->vreg = devm_kcalloc(thread->ctrl->dev, thread->vreg_count,
			sizeof(*thread->vreg), GFP_KERNEL);
	if (!thread->vreg)
		return -ENOMEM;

	i = 0;
	for_each_available_child_of_node(thread->of_node, node) {
		thread->vreg[i].of_node = node;
		thread->vreg[i].thread = thread;

		rc = of_property_read_string(node, "regulator-name",
						&thread->vreg[i].name);
		if (rc) {
			dev_err(thread->ctrl->dev, "could not find regulator name, rc=%d\n",
				rc);
			return rc;
		}

		i++;
	}

	return 0;
}

/**
 * cpr3_allocate_threads() - allocate and initialize CPR3 threads for a given
 *			     controller based upon device tree data
 * @ctrl:		Pointer to the CPR3 controller
 * @min_thread_id:	Minimum allowed hardware thread ID for this controller
 * @max_thread_id:	Maximum allowed hardware thread ID for this controller
 *
 * This function allocates the ctrl->thread array based upon the number of
 * device tree thread subnodes.  It also initializes generic elements of each
 * thread struct such as thread_id, of_node, ctrl, and vreg array.
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_allocate_threads(struct cpr3_controller *ctrl, u32 min_thread_id,
			u32 max_thread_id)
{
	struct device *dev = ctrl->dev;
	struct device_node *thread_node;
	int i, j, rc;

	ctrl->thread_count = 0;

	for_each_available_child_of_node(dev->of_node, thread_node) {
		ctrl->thread_count++;
	}

	ctrl->thread = devm_kcalloc(dev, ctrl->thread_count,
			sizeof(*ctrl->thread), GFP_KERNEL);
	if (!ctrl->thread)
		return -ENOMEM;

	i = 0;
	for_each_available_child_of_node(dev->of_node, thread_node) {
		ctrl->thread[i].of_node = thread_node;
		ctrl->thread[i].ctrl = ctrl;

		rc = of_property_read_u32(thread_node, "qcom,cpr-thread-id",
					  &ctrl->thread[i].thread_id);
		if (rc) {
			dev_err(dev, "could not read DT property qcom,cpr-thread-id, rc=%d\n",
				rc);
			return rc;
		}

		if (ctrl->thread[i].thread_id < min_thread_id ||
				ctrl->thread[i].thread_id > max_thread_id) {
			dev_err(dev, "invalid thread id = %u; not within [%u, %u]\n",
				ctrl->thread[i].thread_id, min_thread_id,
				max_thread_id);
			return -EINVAL;
		}

		/* Verify that the thread ID is unique for all child nodes. */
		for (j = 0; j < i; j++) {
			if (ctrl->thread[j].thread_id
					== ctrl->thread[i].thread_id) {
				dev_err(dev, "duplicate thread id = %u found\n",
					ctrl->thread[i].thread_id);
				return -EINVAL;
			}
		}

		rc = cpr3_allocate_regulators(&ctrl->thread[i]);
		if (rc)
			return rc;

		i++;
	}

	return 0;
}

/**
 * cpr3_map_fuse_base() - ioremap the base address of the fuse region
 * @ctrl:	Pointer to the CPR3 controller
 * @pdev:	Platform device pointer for the CPR3 controller
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_map_fuse_base(struct cpr3_controller *ctrl,
			struct platform_device *pdev)
{
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "fuse_base");
	if (!res || !res->start) {
		dev_err(&pdev->dev, "fuse base address is missing\n");
		return -ENXIO;
	}

	ctrl->fuse_base = devm_ioremap(&pdev->dev, res->start,
						resource_size(res));

	return 0;
}

/**
 * cpr3_read_fuse_param() - reads a CPR3 fuse parameter out of eFuses
 * @fuse_base_addr:	Virtual memory address of the eFuse base address
 * @param:		Null terminated array of fuse param segments to read
 *			from
 * @param_value:	Output with value read from the eFuses
 *
 * This function reads from each of the parameter segments listed in the param
 * array and concatenates their values together.  Reading stops when an element
 * is reached which has all 0 struct values.  The total number of bits specified
 * for the fuse parameter across all segments must be less than or equal to 64.
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_read_fuse_param(void __iomem *fuse_base_addr,
		const struct cpr3_fuse_param *param, u64 *param_value)
{
	u64 fuse_val, val;
	int bits;
	int bits_total = 0;

	*param_value = 0;

	while (param->row || param->bit_start || param->bit_end) {
		if (param->bit_start > param->bit_end
		    || param->bit_end > MAX_FUSE_ROW_BIT) {
			pr_err("Invalid fuse parameter segment: row=%u, start=%u, end=%u\n",
				param->row, param->bit_start, param->bit_end);
			return -EINVAL;
		}

		bits = param->bit_end - param->bit_start + 1;
		if (bits_total + bits > 64) {
			pr_err("Invalid fuse parameter segments; total bits = %d\n",
				bits_total + bits);
			return -EINVAL;
		}

		fuse_val = readq_relaxed(fuse_base_addr
					 + param->row * BYTES_PER_FUSE_ROW);
		val = (fuse_val >> param->bit_start) & ((1ULL << bits) - 1);
		*param_value |= val << bits_total;
		bits_total += bits;

		param++;
	}

	return 0;
}

/**
 * cpr3_convert_open_loop_voltage_fuse() - converts an open loop voltage fuse
 *		value into an absolute voltage with units of microvolts
 * @ref_volt:		Reference voltage in microvolts
 * @step_volt:		The step size in microvolts of the fuse LSB
 * @fuse:		Open loop voltage fuse value
 * @fuse_len:		The bit length of the fuse value
 *
 * The MSB of the fuse parameter corresponds to a sign bit.  If it is set, then
 * the lower bits correspond to the number of steps to go down from the
 * reference voltage.  If it is not set, then the lower bits correspond to the
 * number of steps to go up from the reference voltage.
 */
int cpr3_convert_open_loop_voltage_fuse(int ref_volt, int step_volt, u32 fuse,
					int fuse_len)
{
	int sign, steps;

	sign = (fuse & (1 << (fuse_len - 1))) ? -1 : 1;
	steps = fuse & ((1 << (fuse_len - 1)) - 1);

	return ref_volt + sign * steps * step_volt;
}

/**
 * cpr3_interpolate() - performs linear interpolation
 * @x1		Lower known x value
 * @y1		Lower known y value
 * @x2		Upper known x value
 * @y2		Upper known y value
 * @x		Intermediate x value
 *
 * Returns y where (x, y) falls on the line between (x1, y1) and (x2, y2).
 * It is required that x1 < x2, y1 <= y2, and x1 <= x <= x2.  If these
 * conditions are not met, then y2 will be returned.
 */
u64 cpr3_interpolate(u64 x1, u64 y1, u64 x2, u64 y2, u64 x)
{
	u64 temp;

	if (x1 >= x2 || y1 > y2 || x1 > x || x > x2)
		return y2;

	temp = (x2 - x) * (y2 - y1);
	do_div(temp, (u32)(x2 - x1));

	return y2 - temp;
}

/**
 * cpr3_parse_array_property() - fill an array from a portion of the values
 *		specified for a device tree property
 * @vreg:		Pointer to the CPR3 regulator
 * @prop_name:		The name of the device tree property to read from
 * @tuple_size:		The number of elements in each tuple
 * @out:		Output data array which must be of size tuple_size
 *
 * cpr3_parse_common_corner_data() must be called for vreg before this function
 * is called so that fuse combo and speed bin size elements are initialized.
 *
 * Three formats are supported for the device tree property:
 * 1. Length == tuple_size
 *	(reading begins at index 0)
 * 2. Length == tuple_size * vreg->fuse_combos_supported
 *	(reading begins at index tuple_size * vreg->fuse_combo)
 * 3. Length == tuple_size * vreg->speed_bins_supported
 *	(reading begins at index tuple_size * vreg->speed_bin_fuse)
 *
 * All other property lengths are treated as errors.
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_parse_array_property(struct cpr3_regulator *vreg,
		const char *prop_name, int tuple_size, u32 *out)
{
	struct device_node *node = vreg->of_node;
	int len = 0;
	int i, offset, rc;

	if (!of_find_property(node, prop_name, &len)) {
		cpr3_err(vreg, "property %s is missing\n", prop_name);
		return -EINVAL;
	}

	if (len == tuple_size * sizeof(u32)) {
		offset = 0;
	} else if (len == tuple_size * vreg->fuse_combos_supported
				     * sizeof(u32)) {
		offset = tuple_size * vreg->fuse_combo;
	} else if (vreg->speed_bins_supported > 0 &&
		 len == tuple_size * vreg->speed_bins_supported * sizeof(u32)) {
		offset = tuple_size * vreg->speed_bin_fuse;
	} else {
		if (vreg->speed_bins_supported > 0)
			cpr3_err(vreg, "property %s has invalid length=%d, should be %lu, %lu, or %lu\n",
				prop_name, len,
				tuple_size * sizeof(u32),
				tuple_size * vreg->speed_bins_supported
					   * sizeof(u32),
				tuple_size * vreg->fuse_combos_supported
					   * sizeof(u32));
		else
			cpr3_err(vreg, "property %s has invalid length=%d, should be %lu or %lu\n",
				prop_name, len,
				tuple_size * sizeof(u32),
				tuple_size * vreg->fuse_combos_supported
					   * sizeof(u32));
		return -EINVAL;
	}

	for (i = 0; i < tuple_size; i++) {
		rc = of_property_read_u32_index(node, prop_name, offset + i,
						&out[i]);
		if (rc) {
			cpr3_err(vreg, "error reading property %s, rc=%d\n",
				prop_name, rc);
			return rc;
		}
	}

	return 0;
}

/**
 * cpr3_parse_corner_array_property() - fill a per-corner array from a portion
 *		of the values specified for a device tree property
 * @vreg:		Pointer to the CPR3 regulator
 * @prop_name:		The name of the device tree property to read from
 * @tuple_size:		The number of elements in each per-corner tuple
 * @out:		Output data array which must be of size:
 *			tuple_size * vreg->corner_count
 *
 * cpr3_parse_common_corner_data() must be called for vreg before this function
 * is called so that fuse combo and speed bin size elements are initialized.
 *
 * Three formats are supported for the device tree property:
 * 1. Length == tuple_size * vreg->corner_count
 *	(reading begins at index 0)
 * 2. Length == tuple_size * vreg->fuse_combo_corner_sum
 *	(reading begins at index tuple_size * vreg->fuse_combo_offset)
 * 3. Length == tuple_size * vreg->speed_bin_corner_sum
 *	(reading begins at index tuple_size * vreg->speed_bin_offset)
 *
 * All other property lengths are treated as errors.
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_parse_corner_array_property(struct cpr3_regulator *vreg,
		const char *prop_name, int tuple_size, u32 *out)
{
	struct device_node *node = vreg->of_node;
	int len = 0;
	int i, offset, rc;

	if (!of_find_property(node, prop_name, &len)) {
		cpr3_err(vreg, "property %s is missing\n", prop_name);
		return -EINVAL;
	}

	if (len == tuple_size * vreg->corner_count * sizeof(u32)) {
		offset = 0;
	} else if (len == tuple_size * vreg->fuse_combo_corner_sum
				     * sizeof(u32)) {
		offset = tuple_size * vreg->fuse_combo_offset;
	} else if (vreg->speed_bin_corner_sum > 0 &&
		 len == tuple_size * vreg->speed_bin_corner_sum * sizeof(u32)) {
		offset = tuple_size * vreg->speed_bin_offset;
	} else {
		if (vreg->speed_bin_corner_sum > 0)
			cpr3_err(vreg, "property %s has invalid length=%d, should be %lu, %lu, or %lu\n",
				prop_name, len,
				tuple_size * vreg->corner_count * sizeof(u32),
				tuple_size * vreg->speed_bin_corner_sum
					   * sizeof(u32),
				tuple_size * vreg->fuse_combo_corner_sum
					   * sizeof(u32));
		else
			cpr3_err(vreg, "property %s has invalid length=%d, should be %lu or %lu\n",
				prop_name, len,
				tuple_size * vreg->corner_count * sizeof(u32),
				tuple_size * vreg->fuse_combo_corner_sum
					   * sizeof(u32));
		return -EINVAL;
	}

	for (i = 0; i < tuple_size * vreg->corner_count; i++) {
		rc = of_property_read_u32_index(node, prop_name, offset + i,
						&out[i]);
		if (rc) {
			cpr3_err(vreg, "error reading property %s, rc=%d\n",
				prop_name, rc);
			return rc;
		}
	}

	return 0;
}

/**
 * cpr3_parse_common_corner_data() - parse common CPR3 properties relating to
 *		the corners supported by a CPR3 regulator from device tree
 * @vreg:		Pointer to the CPR3 regulator
 *
 * This function reads, validates, and utilizes the following device tree
 * properties: qcom,cpr-fuse-corners, qcom,cpr-fuse-combos, qcom,cpr-speed-bins,
 * qcom,cpr-speed-bin-corners, qcom,cpr-corners, qcom,cpr-voltage-ceiling,
 * qcom,cpr-voltage-floor, qcom,corner-frequencies,
 * and qcom,cpr-corner-fmax-map.
 *
 * It initializes these CPR3 regulator elements: corner, corner_count,
 * fuse_combos_supported, and speed_bins_supported.  It initializes these
 * elements for each corner: ceiling_volt, floor_volt, proc_freq, and
 * cpr_fuse_corner.
 *
 * It requires that the following CPR3 regulator elements be initialized before
 * being called: fuse_corner_count, fuse_combo, and speed_bin_fuse.
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_parse_common_corner_data(struct cpr3_regulator *vreg)
{
	struct device_node *node = vreg->of_node;
	struct cpr3_controller *ctrl = vreg->thread->ctrl;
	u32 max_fuse_combos, fuse_corners, aging_allowed = 0;
	u32 max_speed_bins = 0;
	u32 *combo_corners;
	u32 *speed_bin_corners;
	u32 *temp;
	int i, j, rc;

	rc = of_property_read_u32(node, "qcom,cpr-fuse-corners", &fuse_corners);
	if (rc) {
		cpr3_err(vreg, "error reading property qcom,cpr-fuse-corners, rc=%d\n",
			rc);
		return rc;
	}

	if (vreg->fuse_corner_count != fuse_corners) {
		cpr3_err(vreg, "device tree config supports %d fuse corners but the hardware has %d fuse corners\n",
			fuse_corners, vreg->fuse_corner_count);
		return -EINVAL;
	}

	rc = of_property_read_u32(node, "qcom,cpr-fuse-combos",
				&max_fuse_combos);
	if (rc) {
		cpr3_err(vreg, "error reading property qcom,cpr-fuse-combos, rc=%d\n",
			rc);
		return rc;
	}

	/*
	 * Sanity check against arbitrarily large value to avoid excessive
	 * memory allocation.
	 */
	if (max_fuse_combos > 100 || max_fuse_combos == 0) {
		cpr3_err(vreg, "qcom,cpr-fuse-combos is invalid: %u\n",
			max_fuse_combos);
		return -EINVAL;
	}

	if (vreg->fuse_combo >= max_fuse_combos) {
		cpr3_err(vreg, "device tree config supports fuse combos 0-%u but the hardware has combo %d\n",
			max_fuse_combos - 1, vreg->fuse_combo);
		BUG_ON(1);
		return -EINVAL;
	}

	vreg->fuse_combos_supported = max_fuse_combos;

	of_property_read_u32(node, "qcom,cpr-speed-bins", &max_speed_bins);

	/*
	 * Sanity check against arbitrarily large value to avoid excessive
	 * memory allocation.
	 */
	if (max_speed_bins > 100) {
		cpr3_err(vreg, "qcom,cpr-speed-bins is invalid: %u\n",
			max_speed_bins);
		return -EINVAL;
	}

	if (max_speed_bins && vreg->speed_bin_fuse >= max_speed_bins) {
		cpr3_err(vreg, "device tree config supports speed bins 0-%u but the hardware has speed bin %d\n",
			max_speed_bins - 1, vreg->speed_bin_fuse);
		BUG();
		return -EINVAL;
	}

	vreg->speed_bins_supported = max_speed_bins;

	combo_corners = kcalloc(vreg->fuse_combos_supported,
				sizeof(*combo_corners), GFP_KERNEL);
	if (!combo_corners)
		return -ENOMEM;

	rc = of_property_read_u32_array(node, "qcom,cpr-corners", combo_corners,
					vreg->fuse_combos_supported);
	if (rc == -EOVERFLOW) {
		/* Single value case */
		rc = of_property_read_u32(node, "qcom,cpr-corners",
					combo_corners);
		for (i = 1; i < vreg->fuse_combos_supported; i++)
			combo_corners[i] = combo_corners[0];
	}
	if (rc) {
		cpr3_err(vreg, "error reading property qcom,cpr-corners, rc=%d\n",
			rc);
		kfree(combo_corners);
		return rc;
	}

	vreg->fuse_combo_offset = 0;
	vreg->fuse_combo_corner_sum = 0;
	for (i = 0; i < vreg->fuse_combos_supported; i++) {
		vreg->fuse_combo_corner_sum += combo_corners[i];
		if (i < vreg->fuse_combo)
			vreg->fuse_combo_offset += combo_corners[i];
	}

	vreg->corner_count = combo_corners[vreg->fuse_combo];

	kfree(combo_corners);

	vreg->speed_bin_offset = 0;
	vreg->speed_bin_corner_sum = 0;
	if (vreg->speed_bins_supported > 0) {
		speed_bin_corners = kcalloc(vreg->speed_bins_supported,
					sizeof(*speed_bin_corners), GFP_KERNEL);
		if (!speed_bin_corners)
			return -ENOMEM;

		rc = of_property_read_u32_array(node,
				"qcom,cpr-speed-bin-corners", speed_bin_corners,
				vreg->speed_bins_supported);
		if (rc) {
			cpr3_err(vreg, "error reading property qcom,cpr-speed-bin-corners, rc=%d\n",
				rc);
			kfree(speed_bin_corners);
			return rc;
		}

		for (i = 0; i < vreg->speed_bins_supported; i++) {
			vreg->speed_bin_corner_sum += speed_bin_corners[i];
			if (i < vreg->speed_bin_fuse)
				vreg->speed_bin_offset += speed_bin_corners[i];
		}

		if (speed_bin_corners[vreg->speed_bin_fuse]
		    != vreg->corner_count) {
			cpr3_err(vreg, "qcom,cpr-corners and qcom,cpr-speed-bin-corners conflict on number of corners: %d vs %u\n",
				vreg->corner_count,
				speed_bin_corners[vreg->speed_bin_fuse]);
			kfree(speed_bin_corners);
			return -EINVAL;
		}

		kfree(speed_bin_corners);
	}

	vreg->corner = devm_kcalloc(ctrl->dev, vreg->corner_count,
			sizeof(*vreg->corner), GFP_KERNEL);
	temp = kcalloc(vreg->corner_count, sizeof(*temp), GFP_KERNEL);
	if (!vreg->corner || !temp)
		return -ENOMEM;

	rc = cpr3_parse_corner_array_property(vreg, "qcom,cpr-voltage-ceiling",
			1, temp);
	if (rc)
		goto free_temp;
	for (i = 0; i < vreg->corner_count; i++)
		vreg->corner[i].ceiling_volt
			= CPR3_ROUND(temp[i], ctrl->step_volt);

	rc = cpr3_parse_corner_array_property(vreg, "qcom,cpr-voltage-floor",
			1, temp);
	if (rc)
		goto free_temp;
	for (i = 0; i < vreg->corner_count; i++)
		vreg->corner[i].floor_volt
			= CPR3_ROUND(temp[i], ctrl->step_volt);

	/* Validate ceiling and floor values */
	for (i = 0; i < vreg->corner_count; i++) {
		if (vreg->corner[i].floor_volt
		    > vreg->corner[i].ceiling_volt) {
			cpr3_err(vreg, "CPR floor[%d]=%d > ceiling[%d]=%d uV\n",
				i, vreg->corner[i].floor_volt,
				i, vreg->corner[i].ceiling_volt);
			rc = -EINVAL;
			goto free_temp;
		}
	}

	/* Load optional system-supply voltages */
	if (of_find_property(vreg->of_node, "qcom,system-voltage", NULL)) {
		rc = cpr3_parse_corner_array_property(vreg,
			"qcom,system-voltage", 1, temp);
		if (rc)
			goto free_temp;
		for (i = 0; i < vreg->corner_count; i++)
			vreg->corner[i].system_volt = temp[i];
	}

	rc = cpr3_parse_corner_array_property(vreg, "qcom,corner-frequencies",
			1, temp);
	if (rc)
		goto free_temp;
	for (i = 0; i < vreg->corner_count; i++)
		vreg->corner[i].proc_freq = temp[i];

	/* Validate frequencies */
	for (i = 1; i < vreg->corner_count; i++) {
		if (vreg->corner[i].proc_freq
		    < vreg->corner[i - 1].proc_freq) {
			cpr3_err(vreg, "invalid frequency: freq[%d]=%u < freq[%d]=%u\n",
				i, vreg->corner[i].proc_freq, i - 1,
				vreg->corner[i - 1].proc_freq);
			rc = -EINVAL;
			goto free_temp;
		}
	}

	rc = cpr3_parse_array_property(vreg, "qcom,cpr-corner-fmax-map",
		vreg->fuse_corner_count, temp);
	if (rc)
		goto free_temp;
	for (i = 0; i < vreg->fuse_corner_count; i++) {
		if (temp[i] < CPR3_CORNER_OFFSET
		    || temp[i] > vreg->corner_count + CPR3_CORNER_OFFSET) {
			cpr3_err(vreg, "invalid corner value specified in qcom,cpr-corner-fmax-map: %u\n",
				temp[i]);
			rc = -EINVAL;
			goto free_temp;
		} else if (i > 0 && temp[i - 1] >= temp[i]) {
			cpr3_err(vreg, "invalid corner %u less than or equal to previous corner %u\n",
				temp[i], temp[i - 1]);
			rc = -EINVAL;
			goto free_temp;
		}
	}
	if (temp[vreg->fuse_corner_count - 1] != vreg->corner_count) {
		cpr3_err(vreg, "highest Fmax corner %u in qcom,cpr-corner-fmax-map does not match highest supported corner %d\n",
			temp[vreg->fuse_corner_count - 1],
			vreg->corner_count);
		rc = -EINVAL;
		goto free_temp;
	}
	for (i = 0; i < vreg->corner_count; i++) {
		for (j = 0; j < vreg->fuse_corner_count; j++) {
			if (i + CPR3_CORNER_OFFSET <= temp[j]) {
				vreg->corner[i].cpr_fuse_corner = j;
				break;
			}
		}
	}

	if (of_find_property(vreg->of_node,
				"qcom,allow-aging-voltage-adjustment", NULL)) {
		rc = cpr3_parse_array_property(vreg,
			"qcom,allow-aging-voltage-adjustment",
			1, &aging_allowed);
		if (rc)
			goto free_temp;

		vreg->aging_allowed = aging_allowed;
	}

	if (vreg->aging_allowed) {
		if (ctrl->aging_ref_volt <= 0) {
			cpr3_err(ctrl, "qcom,cpr-aging-ref-voltage must be specified\n");
			rc = -EINVAL;
			goto free_temp;
		}

		rc = cpr3_parse_array_property(vreg,
			"qcom,cpr-aging-max-voltage-adjustment",
			1, &vreg->aging_max_adjust_volt);
		if (rc)
			goto free_temp;

		rc = cpr3_parse_array_property(vreg,
			"qcom,cpr-aging-ref-corner", 1, &vreg->aging_corner);
		if (rc) {
			goto free_temp;
		} else if (vreg->aging_corner < CPR3_CORNER_OFFSET
			   || vreg->aging_corner > vreg->corner_count - 1
							+ CPR3_CORNER_OFFSET) {
			cpr3_err(vreg, "aging reference corner=%d not in range [%d, %d]\n",
				vreg->aging_corner, CPR3_CORNER_OFFSET,
				vreg->corner_count - 1 + CPR3_CORNER_OFFSET);
			rc = -EINVAL;
			goto free_temp;
		}
		vreg->aging_corner -= CPR3_CORNER_OFFSET;

		if (of_find_property(vreg->of_node, "qcom,cpr-aging-derate",
					NULL)) {
			rc = cpr3_parse_corner_array_property(vreg,
				"qcom,cpr-aging-derate", 1, temp);
			if (rc)
				goto free_temp;

			for (i = 0; i < vreg->corner_count; i++)
				vreg->corner[i].aging_derate = temp[i];
		} else {
			for (i = 0; i < vreg->corner_count; i++)
				vreg->corner[i].aging_derate
					= CPR3_AGING_DERATE_UNITY;
		}
	}

free_temp:
	kfree(temp);
	return rc;
}

/**
 * cpr3_parse_thread_u32() - parse the specified property from the CPR3 thread's
 *		device tree node and verify that it is within the allowed limits
 * @thread:		Pointer to the CPR3 thread
 * @propname:		The name of the device tree property to read
 * @out_value:		The output pointer to fill with the value read
 * @value_min:		The minimum allowed property value
 * @value_max:		The maximum allowed property value
 *
 * This function prints a verbose error message if the property is missing or
 * has a value which is not within the specified range.
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_parse_thread_u32(struct cpr3_thread *thread, const char *propname,
		       u32 *out_value, u32 value_min, u32 value_max)
{
	int rc;

	rc = of_property_read_u32(thread->of_node, propname, out_value);
	if (rc) {
		cpr3_err(thread->ctrl, "thread %u error reading property %s, rc=%d\n",
			thread->thread_id, propname, rc);
		return rc;
	}

	if (*out_value < value_min || *out_value > value_max) {
		cpr3_err(thread->ctrl, "thread %u %s=%u is invalid; allowed range: [%u, %u]\n",
			thread->thread_id, propname, *out_value, value_min,
			value_max);
		return -EINVAL;
	}

	return 0;
}

/**
 * cpr3_parse_ctrl_u32() - parse the specified property from the CPR3
 *		controller's device tree node and verify that it is within the
 *		allowed limits
 * @ctrl:		Pointer to the CPR3 controller
 * @propname:		The name of the device tree property to read
 * @out_value:		The output pointer to fill with the value read
 * @value_min:		The minimum allowed property value
 * @value_max:		The maximum allowed property value
 *
 * This function prints a verbose error message if the property is missing or
 * has a value which is not within the specified range.
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_parse_ctrl_u32(struct cpr3_controller *ctrl, const char *propname,
		       u32 *out_value, u32 value_min, u32 value_max)
{
	int rc;

	rc = of_property_read_u32(ctrl->dev->of_node, propname, out_value);
	if (rc) {
		cpr3_err(ctrl, "error reading property %s, rc=%d\n",
			propname, rc);
		return rc;
	}

	if (*out_value < value_min || *out_value > value_max) {
		cpr3_err(ctrl, "%s=%u is invalid; allowed range: [%u, %u]\n",
			propname, *out_value, value_min, value_max);
		return -EINVAL;
	}

	return 0;
}

/**
 * cpr3_parse_common_thread_data() - parse common CPR3 thread properties from
 *		device tree
 * @thread:		Pointer to the CPR3 thread
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_parse_common_thread_data(struct cpr3_thread *thread)
{
	int rc;

	rc = cpr3_parse_thread_u32(thread, "qcom,cpr-consecutive-up",
			&thread->consecutive_up, CPR3_CONSECUTIVE_UP_DOWN_MIN,
			CPR3_CONSECUTIVE_UP_DOWN_MAX);
	if (rc)
		return rc;

	rc = cpr3_parse_thread_u32(thread, "qcom,cpr-consecutive-down",
			&thread->consecutive_down, CPR3_CONSECUTIVE_UP_DOWN_MIN,
			CPR3_CONSECUTIVE_UP_DOWN_MAX);
	if (rc)
		return rc;

	rc = cpr3_parse_thread_u32(thread, "qcom,cpr-up-threshold",
			&thread->up_threshold, CPR3_UP_DOWN_THRESHOLD_MIN,
			CPR3_UP_DOWN_THRESHOLD_MAX);
	if (rc)
		return rc;

	rc = cpr3_parse_thread_u32(thread, "qcom,cpr-down-threshold",
			&thread->down_threshold, CPR3_UP_DOWN_THRESHOLD_MIN,
			CPR3_UP_DOWN_THRESHOLD_MAX);
	if (rc)
		return rc;

	return rc;
}

/**
 * cpr3_parse_common_ctrl_data() - parse common CPR3 controller properties from
 *		device tree
 * @ctrl:		Pointer to the CPR3 controller
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_parse_common_ctrl_data(struct cpr3_controller *ctrl)
{
	int rc;

	rc = cpr3_parse_ctrl_u32(ctrl, "qcom,cpr-sensor-time",
			&ctrl->sensor_time, 0, UINT_MAX);
	if (rc)
		return rc;

	rc = cpr3_parse_ctrl_u32(ctrl, "qcom,cpr-loop-time",
			&ctrl->loop_time, 0, UINT_MAX);
	if (rc)
		return rc;

	rc = cpr3_parse_ctrl_u32(ctrl, "qcom,cpr-idle-cycles",
			&ctrl->idle_clocks, CPR3_IDLE_CLOCKS_MIN,
			CPR3_IDLE_CLOCKS_MAX);
	if (rc)
		return rc;

	rc = cpr3_parse_ctrl_u32(ctrl, "qcom,cpr-step-quot-init-min",
			&ctrl->step_quot_init_min, CPR3_STEP_QUOT_MIN,
			CPR3_STEP_QUOT_MAX);
	if (rc)
		return rc;

	rc = cpr3_parse_ctrl_u32(ctrl, "qcom,cpr-step-quot-init-max",
			&ctrl->step_quot_init_max, CPR3_STEP_QUOT_MIN,
			CPR3_STEP_QUOT_MAX);
	if (rc)
		return rc;

	rc = of_property_read_u32(ctrl->dev->of_node, "qcom,voltage-step",
				&ctrl->step_volt);
	if (rc) {
		cpr3_err(ctrl, "error reading property qcom,voltage-step, rc=%d\n",
			rc);
		return rc;
	}
	if (ctrl->step_volt <= 0) {
		cpr3_err(ctrl, "qcom,voltage-step=%d is invalid\n",
			ctrl->step_volt);
		return -EINVAL;
	}

	rc = cpr3_parse_ctrl_u32(ctrl, "qcom,cpr-count-mode",
			&ctrl->count_mode, CPR3_COUNT_MODE_ALL_AT_ONCE_MIN,
			CPR3_COUNT_MODE_STAGGERED);
	if (rc)
		return rc;

	/* Count repeat is optional */
	ctrl->count_repeat = 0;
	of_property_read_u32(ctrl->dev->of_node, "qcom,cpr-count-repeat",
			&ctrl->count_repeat);

	ctrl->cpr_allowed_sw = of_property_read_bool(ctrl->dev->of_node,
			"qcom,cpr-enable");

	/* Aging reference voltage is optional */
	ctrl->aging_ref_volt = 0;
	of_property_read_u32(ctrl->dev->of_node, "qcom,cpr-aging-ref-voltage",
			&ctrl->aging_ref_volt);

	ctrl->vdd_regulator = devm_regulator_get(ctrl->dev, "vdd");
	if (IS_ERR(ctrl->vdd_regulator)) {
		rc = PTR_ERR(ctrl->vdd_regulator);
		if (rc != -EPROBE_DEFER)
			cpr3_err(ctrl, "unable request vdd regulator, rc=%d\n",
				rc);
		return rc;
	}

	ctrl->core_clk = devm_clk_get(ctrl->dev, "core_clk");
	if (IS_ERR(ctrl->core_clk)) {
		rc = PTR_ERR(ctrl->core_clk);
		if (rc != -EPROBE_DEFER)
			cpr3_err(ctrl, "unable request core clock, rc=%d\n",
				rc);
		return rc;
	}

	ctrl->system_regulator = devm_regulator_get_optional(ctrl->dev,
								"system");
	if (IS_ERR(ctrl->system_regulator)) {
		rc = PTR_ERR(ctrl->system_regulator);
		if (rc != -EPROBE_DEFER) {
			rc = 0;
			ctrl->system_regulator = NULL;
		} else {
			return rc;
		}
	}

	return rc;
}

/**
 * cpr3_limit_open_loop_voltages() - modify the open-loop voltage of each corner
 *				so that it fits within the floor to ceiling
 *				voltage range of the corner
 * @vreg:		Pointer to the CPR3 regulator
 *
 * This function clips the open-loop voltage for each corner so that it is
 * limited to the floor to ceiling range.  It also rounds each open-loop voltage
 * so that it corresponds to a set point available to the underlying regulator.
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_limit_open_loop_voltages(struct cpr3_regulator *vreg)
{
	int i, volt;

	cpr3_debug(vreg, "open-loop voltages after trimming and rounding:\n");
	for (i = 0; i < vreg->corner_count; i++) {
		volt = CPR3_ROUND(vreg->corner[i].open_loop_volt,
					vreg->thread->ctrl->step_volt);
		if (volt < vreg->corner[i].floor_volt)
			volt = vreg->corner[i].floor_volt;
		else if (volt > vreg->corner[i].ceiling_volt)
			volt = vreg->corner[i].ceiling_volt;
		vreg->corner[i].open_loop_volt = volt;
		cpr3_debug(vreg, "corner[%2d]: open-loop=%d uV\n", i, volt);
	}

	return 0;
}

/**
 * cpr3_open_loop_voltage_as_ceiling() - configures the ceiling voltage for each
 *		corner to equal the open-loop voltage if the relevant device
 *		tree property is found for the CPR3 regulator
 * @vreg:		Pointer to the CPR3 regulator
 *
 * This function assumes that the the open-loop voltage for each corner has
 * already been rounded to the nearest allowed set point and that it falls
 * within the floor to ceiling range.
 *
 * Return: none
 */
void cpr3_open_loop_voltage_as_ceiling(struct cpr3_regulator *vreg)
{
	int i;

	if (!of_property_read_bool(vreg->of_node,
				"qcom,cpr-scaled-open-loop-voltage-as-ceiling"))
		return;

	for (i = 0; i < vreg->corner_count; i++)
		vreg->corner[i].ceiling_volt
			= vreg->corner[i].open_loop_volt;
}

/**
 * cpr3_limit_floor_voltages() - raise the floor voltage of each corner so that
 *		the optional maximum floor to ceiling voltage range specified in
 *		device tree is satisfied
 * @vreg:		Pointer to the CPR3 regulator
 *
 * This function also ensures that the open-loop voltage for each corner falls
 * within the final floor to ceiling voltage range and that floor voltages
 * increase monotonically.
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_limit_floor_voltages(struct cpr3_regulator *vreg)
{
	char *prop = "qcom,cpr-floor-to-ceiling-max-range";
	int i, floor_new;
	u32 *floor_range;
	int rc = 0;

	if (!of_find_property(vreg->of_node, prop, NULL))
		goto enforce_monotonicity;

	floor_range = kcalloc(vreg->corner_count, sizeof(*floor_range),
				GFP_KERNEL);
	if (!floor_range)
		return -ENOMEM;

	rc = cpr3_parse_corner_array_property(vreg, prop, 1, floor_range);
	if (rc)
		goto free_floor_adjust;

	for (i = 0; i < vreg->corner_count; i++) {
		if ((s32)floor_range[i] >= 0) {
			floor_new = CPR3_ROUND(vreg->corner[i].ceiling_volt
							- floor_range[i],
						vreg->thread->ctrl->step_volt);

			vreg->corner[i].floor_volt = max(floor_new,
						vreg->corner[i].floor_volt);
			if (vreg->corner[i].open_loop_volt
			    < vreg->corner[i].floor_volt)
				vreg->corner[i].open_loop_volt
					= vreg->corner[i].floor_volt;
		}
	}

free_floor_adjust:
	kfree(floor_range);

enforce_monotonicity:
	/* Ensure that floor voltages increase monotonically. */
	for (i = 1; i < vreg->corner_count; i++) {
		if (vreg->corner[i].floor_volt
		    < vreg->corner[i - 1].floor_volt) {
			cpr3_debug(vreg, "corner %d floor voltage=%d uV < corner %d voltage=%d uV; overriding: corner %d voltage=%d\n",
				i, vreg->corner[i].floor_volt,
				i - 1, vreg->corner[i - 1].floor_volt,
				i, vreg->corner[i - 1].floor_volt);
			vreg->corner[i].floor_volt
				= vreg->corner[i - 1].floor_volt;

			if (vreg->corner[i].open_loop_volt
			    < vreg->corner[i].floor_volt)
				vreg->corner[i].open_loop_volt
					= vreg->corner[i].floor_volt;
			if (vreg->corner[i].ceiling_volt
			    < vreg->corner[i].floor_volt)
				vreg->corner[i].ceiling_volt
					= vreg->corner[i].floor_volt;
		}
	}

	return rc;
}

/**
 * cpr3_print_quots() - print CPR target quotients into the kernel log for
 *		debugging purposes
 * @vreg:		Pointer to the CPR3 regulator
 *
 * Return: none
 */
void cpr3_print_quots(struct cpr3_regulator *vreg)
{
	int i, j, pos;
	size_t buflen;
	char *buf;

	buflen = sizeof(*buf) * CPR3_RO_COUNT * (MAX_CHARS_PER_INT + 2);
	buf = kzalloc(buflen, GFP_KERNEL);
	if (!buf)
		return;

	for (i = 0; i < vreg->corner_count; i++) {
		for (j = 0, pos = 0; j < CPR3_RO_COUNT; j++)
			pos += scnprintf(buf + pos, buflen - pos, " %u",
				vreg->corner[i].target_quot[j]);
		cpr3_debug(vreg, "target quots[%2d]:%s\n", i, buf);
	}

	kfree(buf);
}

/**
 * cpr3_adjust_fused_open_loop_voltages() - adjust the fused open-loop voltages
 *		for each fuse corner according to device tree values
 * @vreg:		Pointer to the CPR3 regulator
 * @fuse_volt:		Pointer to an array of the fused open-loop voltage
 *			values
 *
 * Voltage values in fuse_volt are modified in place.
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_adjust_fused_open_loop_voltages(struct cpr3_regulator *vreg,
		int *fuse_volt)
{
	int i, rc, prev_volt;
	int *volt_adjust;

	if (!of_find_property(vreg->of_node,
			"qcom,cpr-open-loop-voltage-fuse-adjustment", NULL)) {
		/* No adjustment required. */
		return 0;
	}

	volt_adjust = kcalloc(vreg->fuse_corner_count, sizeof(*volt_adjust),
				GFP_KERNEL);
	if (!volt_adjust)
		return -ENOMEM;

	rc = cpr3_parse_array_property(vreg,
		"qcom,cpr-open-loop-voltage-fuse-adjustment",
		vreg->fuse_corner_count, volt_adjust);
	if (rc) {
		cpr3_err(vreg, "could not load open-loop fused voltage adjustments, rc=%d\n",
			rc);
		goto done;
	}

	for (i = 0; i < vreg->fuse_corner_count; i++) {
		if (volt_adjust[i]) {
			prev_volt = fuse_volt[i];
			fuse_volt[i] += volt_adjust[i];
			cpr3_info(vreg, "adjusted fuse corner %d open-loop voltage: %d --> %d uV\n",
				i, prev_volt, fuse_volt[i]);
		}
	}

done:
	kfree(volt_adjust);
	return rc;
}

/**
 * cpr3_adjust_open_loop_voltages() - adjust the open-loop voltages for each
 *		corner according to device tree values
 * @vreg:		Pointer to the CPR3 regulator
 *
 * Return: 0 on success, errno on failure
 */
int cpr3_adjust_open_loop_voltages(struct cpr3_regulator *vreg)
{
	int i, rc, prev_volt, min_volt;
	int *volt_adjust, *volt_diff;

	if (!of_find_property(vreg->of_node,
			"qcom,cpr-open-loop-voltage-adjustment", NULL)) {
		/* No adjustment required. */
		return 0;
	}

	volt_adjust = kcalloc(vreg->corner_count, sizeof(*volt_adjust),
				GFP_KERNEL);
	volt_diff = kcalloc(vreg->corner_count, sizeof(*volt_diff), GFP_KERNEL);
	if (!volt_adjust || !volt_diff) {
		rc = -ENOMEM;
		goto done;
	}

	rc = cpr3_parse_corner_array_property(vreg,
		"qcom,cpr-open-loop-voltage-adjustment", 1, volt_adjust);
	if (rc) {
		cpr3_err(vreg, "could not load open-loop voltage adjustments, rc=%d\n",
			rc);
		goto done;
	}

	for (i = 0; i < vreg->corner_count; i++) {
		if (volt_adjust[i]) {
			prev_volt = vreg->corner[i].open_loop_volt;
			vreg->corner[i].open_loop_volt += volt_adjust[i];
			cpr3_info(vreg, "adjusted corner %d open-loop voltage: %d --> %d uV\n",
				i, prev_volt, vreg->corner[i].open_loop_volt);
		}
	}

	if (of_find_property(vreg->of_node,
			"qcom,cpr-open-loop-voltage-min-diff", NULL)) {
		rc = cpr3_parse_corner_array_property(vreg,
			"qcom,cpr-open-loop-voltage-min-diff", 1, volt_diff);
		if (rc) {
			cpr3_err(vreg, "could not load minimum open-loop voltage differences, rc=%d\n",
				rc);
			goto done;
		}
	}

	/*
	 * Ensure that open-loop voltages increase monotonically with respect
	 * to configurable minimum allowed differences.
	 */
	for (i = 1; i < vreg->corner_count; i++) {
		min_volt = vreg->corner[i - 1].open_loop_volt + volt_diff[i];
		if (vreg->corner[i].open_loop_volt < min_volt) {
			cpr3_info(vreg, "adjusted corner %d open-loop voltage=%d uV < corner %d voltage=%d uV + min diff=%d uV; overriding: corner %d voltage=%d\n",
				i, vreg->corner[i].open_loop_volt,
				i - 1, vreg->corner[i - 1].open_loop_volt,
				volt_diff[i], i, min_volt);
			vreg->corner[i].open_loop_volt = min_volt;
		}
	}

done:
	kfree(volt_diff);
	kfree(volt_adjust);
	return rc;
}

/**
 * cpr3_quot_adjustment() - returns the quotient adjustment value resulting from
 *		the specified voltage adjustment and RO scaling factor
 * @ro_scale:		The CPR ring oscillator (RO) scaling factor with units
 *			of QUOT/V
 * @volt_adjust:	The amount to adjust the voltage by in units of
 *			microvolts.  This value may be positive or negative.
 */
int cpr3_quot_adjustment(int ro_scale, int volt_adjust)
{
	unsigned long long temp;
	int quot_adjust;
	int sign = 1;

	if (ro_scale < 0) {
		sign = -sign;
		ro_scale = -ro_scale;
	}

	if (volt_adjust < 0) {
		sign = -sign;
		volt_adjust = -volt_adjust;
	}

	temp = (unsigned long long)ro_scale * (unsigned long long)volt_adjust;
	do_div(temp, 1000000);

	quot_adjust = temp;
	quot_adjust *= sign;

	return quot_adjust;
}

/**
 * cpr3_voltage_adjustment() - returns the voltage adjustment value resulting
 *		from the specified quotient adjustment and RO scaling factor
 * @ro_scale:		The CPR ring oscillator (RO) scaling factor with units
 *			of QUOT/V
 * @quot_adjust:	The amount to adjust the quotient by in units of
 *			QUOT.  This value may be positive or negative.
 */
int cpr3_voltage_adjustment(int ro_scale, int quot_adjust)
{
	unsigned long long temp;
	int volt_adjust;
	int sign = 1;

	if (ro_scale < 0) {
		sign = -sign;
		ro_scale = -ro_scale;
	}

	if (quot_adjust < 0) {
		sign = -sign;
		quot_adjust = -quot_adjust;
	}

	if (ro_scale == 0)
		return 0;

	temp = (unsigned long long)quot_adjust * 1000000;
	do_div(temp, ro_scale);

	volt_adjust = temp;
	volt_adjust *= sign;

	return volt_adjust;
}
