/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SPBM_CORE_H
#define _SPBM_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/limits.h>
#define SPBM_U64_MAX U64_MAX
#else
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#define SPBM_U64_MAX UINT64_MAX
#endif

/*
 * DMI matching helper
 */
struct spbm_dmi_info {
	const char *sys_vendor;
	const char *product_name;
	const char *product_family;
};

static inline bool spbm_str_contains(const char *str, const char *sub)
{
	if (!str || !sub)
		return false;
	return strstr(str, sub) != NULL;
}

static inline bool spbm_dmi_is_supported(const struct spbm_dmi_info *info)
{
	if (!info || !info->sys_vendor || !info->product_name)
		return false;

	/* Whitelist known hardware vendors: NVIDIA, XFUSION */
	if (strcmp(info->sys_vendor, "NVIDIA") != 0 &&
	    strcmp(info->sys_vendor, "XFUSION") != 0)
		return false;

	/* Check for GB10 or DGX Spark platforms */
	if (spbm_str_contains(info->product_family, "DGX Spark") ||
	    spbm_str_contains(info->product_name, "DGX Spark") ||
	    spbm_str_contains(info->product_name, "GB10"))
		return true;

	return false;
}

/*
 * Memory boundary validation helper
 */
static inline int spbm_validate_bounds(uint64_t offset, size_t size, uint64_t res_size)
{
	if (res_size == 0 || size == 0)
		return -EINVAL;

	/* Check for unsigned wrap-around */
	if (offset > SPBM_U64_MAX - size)
		return -EOVERFLOW;

	if (offset + size > res_size)
		return -ERANGE;

	return 0;
}

/*
 * Channel key matching with bounds check
 */
struct spbm_chan_def {
	const char *dsm_key;
	const char *label;
};

static inline bool spbm_try_resolve_bounds(const char *key, uint64_t offset,
					   uint64_t res_size,
					   const struct spbm_chan_def *chans,
					   uint32_t *offsets, int n)
{
	int i;

	if (spbm_validate_bounds(offset, sizeof(uint32_t), res_size) != 0)
		return false;

	for (i = 0; i < n; i++) {
		if (strcmp(chans[i].dsm_key, key) == 0) {
			offsets[i] = (uint32_t)offset;
			return true;
		}
	}
	return false;
}

/*
 * 64-bit Monotonic Energy Accumulator
 */
struct spbm_energy_acc {
	uint64_t accumulated_uj;
	uint32_t last_raw_mj;
	bool initialized;
};

static inline void spbm_energy_acc_init(struct spbm_energy_acc *acc, uint32_t initial_raw_mj)
{
	acc->accumulated_uj = (uint64_t)initial_raw_mj * 1000ULL;
	acc->last_raw_mj = initial_raw_mj;
	acc->initialized = true;
}

static inline uint64_t spbm_energy_acc_update(struct spbm_energy_acc *acc, uint32_t raw_mj)
{
	if (!acc->initialized) {
		spbm_energy_acc_init(acc, raw_mj);
		return acc->accumulated_uj;
	}

	/* Unsigned 32-bit subtraction correctly handles roll-over */
	uint32_t delta_mj = raw_mj - acc->last_raw_mj;

	acc->accumulated_uj += (uint64_t)delta_mj * 1000ULL;
	acc->last_raw_mj = raw_mj;

	return acc->accumulated_uj;
}

/*
 * hwmon ABI validation helper (conforming to Documentation/hwmon/sysfs-interface.rst)
 */
static inline bool spbm_hwmon_attr_is_standard(const char *name)
{
	if (!name)
		return false;

	/* Must start with one of the standard sensor prefixes */
	const char *prefix = NULL;

	if (strncmp(name, "power", 5) == 0)
		prefix = name + 5;
	else if (strncmp(name, "energy", 6) == 0)
		prefix = name + 6;
	else if (strncmp(name, "temp", 4) == 0)
		prefix = name + 4;
	else
		return false;

	/* Must be followed by 1 or more digits */
	if (*prefix < '0' || *prefix > '9')
		return false;
	while (*prefix >= '0' && *prefix <= '9')
		prefix++;

	/* Must match a standard hwmon attribute suffix */
	if (strcmp(prefix, "_input") == 0 ||
	    strcmp(prefix, "_label") == 0 ||
	    strcmp(prefix, "_cap") == 0 ||
	    strcmp(prefix, "_max") == 0 ||
	    strcmp(prefix, "_min") == 0 ||
	    strcmp(prefix, "_crit_alarm") == 0 ||
	    strcmp(prefix, "_alarm") == 0)
		return true;

	return false;
}

/*
 * Concurrency & Mailbox write abstraction
 */
#ifndef __KERNEL__
#include <pthread.h>

struct spbm_mock_device {
	pthread_mutex_t lock;
	bool in_critical_section;
	uint32_t last_val;
	uint32_t poke_count;
};

static inline void spbm_mock_device_init(struct spbm_mock_device *dev)
{
	pthread_mutex_init(&dev->lock, NULL);
	dev->in_critical_section = false;
	dev->last_val = 0;
	dev->poke_count = 0;
}

static inline int spbm_mock_write_power_cap(struct spbm_mock_device *dev,
					    int ch, long val)
{
	(void)ch;
	pthread_mutex_lock(&dev->lock);
	dev->in_critical_section = true;

	uint32_t mw = (uint32_t)(val / 1000);

	dev->last_val = mw;
	dev->poke_count++;

	dev->in_critical_section = false;
	pthread_mutex_unlock(&dev->lock);
	return 0;
}
#endif

#endif /* _SPBM_CORE_H */
