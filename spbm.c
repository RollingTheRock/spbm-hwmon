// SPDX-License-Identifier: GPL-2.0
/*
 * NVIDIA DGX Spark (GB10) SPBM Power Telemetry hwmon driver
 *
 * Author: Andrew Wang <RollingTheRock>
 * Based on initial reverse-engineering draft prototype by Antheas Kapenekakis.
 *
 * Exposes the System Power Budget Manager (SPBM) shared memory as
 * standard Linux hwmon sensors. The MTEL (NVDA8800) ACPI device
 * provides a _DSM that describes its memory resources; this driver
 * queries _DSM function 1 at probe time to locate the "SPBM" region
 * by name, and _DSM function 2 to discover register offsets by their
 * canonical names rather than hard-coding addresses.
 *
 * The SPBM firmware (running on MediaTek SSPM) continuously updates
 * these registers with live power telemetry in milliwatts,
 * cumulative energy counters in millijoules, and thermal zone
 * temperatures in centidegrees Celsius.
 *
 * Architecture & Roadmap:
 * Currently binds as an acpi_driver to the NVDA8800 device on the
 * ACPI bus because DSDT lacks _UID/_STA.
 * [RollingTheRock] ##RollingTheRock Planned Evolution:
 * Next phase will abstract this into a self-instantiating platform_driver
 * using acpi_create_platform_device(), decoupling hardware monitoring
 * into modern Linux Device Model (LDM) platform_driver architecture.
 */

#include <linux/module.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/io.h>
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/uuid.h>

#include "spbm_core.h"

#define DRIVER_NAME		"spbm"
#define OFF_UNKNOWN		U32_MAX
#define SPBM_MAX_DSM_INDICES	16

/*
 * [RollingTheRock] ##RollingTheRock
 * DMI hardware whitelist to prevent false loading on non-GB10 ARM64 systems.
 */
static const struct dmi_system_id spbm_dmi_table[] = {
	{
		.ident = "NVIDIA DGX Spark",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "NVIDIA"),
			DMI_MATCH(DMI_PRODUCT_FAMILY, "DGX Spark"),
		},
	},
	{
		.ident = "NVIDIA DGX Spark (Product Name)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "NVIDIA"),
			DMI_MATCH(DMI_PRODUCT_NAME, "DGX Spark"),
		},
	},
	{
		.ident = "XFUSION FusionXpark GB10",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "XFUSION"),
			DMI_MATCH(DMI_PRODUCT_NAME, "FusionXpark GB10"),
		},
	},
	{
		.ident = "GB10 Generic Match",
		.matches = {
			DMI_MATCH(DMI_PRODUCT_FAMILY, "DGX Spark"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, spbm_dmi_table);

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "Force driver load even if DMI check fails");

/*
 * _DSM UUID for NVDA8800 MTEL device.

 * Function 1 returns resource names, function 2 returns register maps.
 */
static const guid_t mtel_dsm_guid =
	GUID_INIT(0x12345678, 0x1234, 0x1234,
		  0x12, 0x34, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc);

/* Channel definition: _DSM register name -> hwmon label */
struct spbm_chan {
	const char *dsm_key;	/* _DSM register name to match */
	const char *label;	/* hwmon label */
};

/* Power channels (mW in firmware, uW in hwmon) */
static const struct spbm_chan pwr_chans[] = {
	{ "SPBM_TE_SYS_TOTAL_TELEMETRY_OFFSET",		"sys_total" },
	{ "SPBM_TE_SOC_PKG_TELEMETRY_OFFSET",		"soc_pkg" },
	{ "SPBM_TE_C_AND_G_TELEMETRY_OFFSET",		"cpu_gpu" },
	{ "SPBM_TE_CPU_P_TELEMETRY_OFFSET",		"cpu_p" },
	{ "SPBM_TE_CPU_E_TELEMETRY_OFFSET",		"cpu_e" },
	{ "SPBM_TE_VCORE_TELEMETRY_OFFSET",		"vcore" },
	{ "SPBM_TE_CHR_TELEMETRY_OFFSET",		"dc_input" },
	{ "SPBM_TE_TOTAL_GPU_OUT_OFFSET",		"gpu" },
	{ "SPBM_TE_PREREG_IN_OFFSET",			"prereg" },
	{ "SPBM_TE_DLA_IN_OFFSET",			"dla" },
	/* PL channels: input = EWMA-smoothed power, cap/max = limits */
	{ "SPBM_PWR_AVG_EWMA_S_PL1_OFFSET",		"pl1" },
	{ "SPBM_PWR_AVG_EWMA_S_PL2_OFFSET",		"pl2" },
	{ "SPBM_PWR_AVG_EWMA_S_SYSPL1_OFFSET",		"syspl1" },
	{ "SPBM_PWR_AVG_EWMA_S_SYSPL2_OFFSET",		"syspl2" },
};
#define N_PWR ARRAY_SIZE(pwr_chans)

/* Energy channels (mJ in firmware, uJ in hwmon) */
static const struct spbm_chan nrg_chans[] = {
	{ "SPBM_PKG_ENERGY_VALUE_ACCUMULATE_OFFSET",	"pkg" },
	{ "SPBM_CPU_E_ENERGY_VALUE_ACCUMULATE_OFFSET",	"cpu_e" },
	{ "SPBM_CPU_P_ENERGY_VALUE_ACCUMULATE_OFFSET",	"cpu_p" },
	{ "SPBM_GPM_ENERGY_VALUE_ACCUMULATE_OFFSET",	"gpu" },
};
#define N_NRG ARRAY_SIZE(nrg_chans)

/* Temperature channels (centidegrees C in firmware, millidegrees C in hwmon) */
static const struct spbm_chan temp_chans[] = {
	{ "SPBM_PKG_TJ_MAX_OFFSET",			"tj_max" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_CPU_E_CLU_0_OFFSET", "cpu_e_clu0" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_CPU_P_CLU_0_OFFSET", "cpu_p_clu0" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_CPU_E_CLU_1_OFFSET", "cpu_e_clu1" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_CPU_P_CLU_1_OFFSET", "cpu_p_clu1" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_GPU_OFFSET",	"gpu" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_SOC_OFFSET",	"soc" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_DLA_OFFSET",	"dla" },
};
#define N_TEMP ARRAY_SIZE(temp_chans)

/* OS-writable power limit registers (mW) */
static const struct spbm_chan pl_os_chans[] = {
	{ "SPBM_PL1_VAL_OS_OFFSET",			"pl1_os" },
	{ "SPBM_PL2_VAL_OS_OFFSET",			"pl2_os" },
	{ "SPBM_SYSPL1_VAL_OS_OFFSET",			"syspl1_os" },
	{ "SPBM_SYSPL2_VAL_OS_OFFSET",			"syspl2_os" },
};
#define N_PL_OS ARRAY_SIZE(pl_os_chans)

/* Firmware power limit ceiling (read-only, for power_max) */
static const struct spbm_chan pwr_high_chans[] = {
	{ "SPBM_PL1_LIMIT_HIGH_OFFSET",			"pl1" },
	{ "SPBM_PL2_LIMIT_HIGH_OFFSET",			"pl2" },
	{ "SPBM_SYSPL1_LIMIT_HIGH_OFFSET",		"syspl1" },
	{ "SPBM_SYSPL2_LIMIT_HIGH_OFFSET",		"syspl2" },
};
#define N_PWR_HIGH ARRAY_SIZE(pwr_high_chans)

/* Firmware power limit floor (read-only, for power_min) */
static const struct spbm_chan pwr_low_chans[] = {
	{ "SPBM_PL1_LIMIT_LOW_OFFSET",			"pl1" },
	{ "SPBM_PL2_LIMIT_LOW_OFFSET",			"pl2" },
	{ "SPBM_SYSPL1_LIMIT_LOW_OFFSET",		"syspl1" },
	{ "SPBM_SYSPL2_LIMIT_LOW_OFFSET",		"syspl2" },
};
#define N_PWR_LOW ARRAY_SIZE(pwr_low_chans)

/* Effective power limit registers (for power_cap readback when OS=0) */
static const struct spbm_chan pwr_eff_chans[] = {
	{ "SPBM_PL1_VAL_OFFSET",			"pl1" },
	{ "SPBM_PL2_VAL_OFFSET",			"pl2" },
	{ "SPBM_SYSPL1_VAL_OFFSET",			"syspl1" },
	{ "SPBM_SYSPL2_VAL_OFFSET",			"syspl2" },
};
#define N_PWR_EFF ARRAY_SIZE(pwr_eff_chans)

struct spbm_priv {
	void __iomem *base;
	resource_size_t res_size;
	struct mutex lock;
	u32 pwr_off[N_PWR];
	u32 pwr_cap_off[N_PWR];
	u32 pwr_max_off[N_PWR];
	u32 pwr_min_off[N_PWR];
	u32 pwr_eff_off[N_PWR];
	u32 nrg_off[N_NRG];
	struct spbm_energy_acc energy_acc[N_NRG];
	u32 temp_off[N_TEMP];
	u32 prochot_off;
	u32 pl_os_off[N_PL_OS];
	u32 pwr_high_off[N_PWR_HIGH];
	u32 pwr_low_off[N_PWR_LOW];
	u32 pwr_eff_resolve[N_PWR_EFF];
};

/* hwmon callbacks */

static umode_t spbm_visible(const void *data, enum hwmon_sensor_types type,
			    u32 attr, int ch)
{
	const struct spbm_priv *p = data;

	if (type == hwmon_power && ch < N_PWR) {
		if (p->pwr_off[ch] != OFF_UNKNOWN &&
		    (attr == hwmon_power_input || attr == hwmon_power_label))
			return 0444;
		if (attr == hwmon_power_cap && p->pwr_cap_off[ch] != OFF_UNKNOWN)
			return 0644;
		if (attr == hwmon_power_max && p->pwr_max_off[ch] != OFF_UNKNOWN)
			return 0444;
		if (attr == hwmon_power_min && p->pwr_min_off[ch] != OFF_UNKNOWN)
			return 0444;
	}

	if (type == hwmon_energy && ch < N_NRG &&
	    p->nrg_off[ch] != OFF_UNKNOWN &&
	    (attr == hwmon_energy_input || attr == hwmon_energy_label))
		return 0444;

	if (type == hwmon_temp && ch < N_TEMP) {
		if (p->temp_off[ch] != OFF_UNKNOWN &&
		    (attr == hwmon_temp_input || attr == hwmon_temp_label))
			return 0444;
		/* Standard hwmon crit alarm mapping for PROCHOT on channel 0 (tj_max) */
		if (ch == 0 && attr == hwmon_temp_crit_alarm &&
		    p->prochot_off != OFF_UNKNOWN)
			return 0444;
	}

	return 0;
}

static int spbm_read(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int ch, long *val)
{
	struct spbm_priv *p = dev_get_drvdata(dev);
	u32 raw;

	if (type == hwmon_power && ch < N_PWR) {
		if (attr == hwmon_power_input &&
		    p->pwr_off[ch] != OFF_UNKNOWN) {
			raw = ioread32(p->base + p->pwr_off[ch]);
			*val = (long)raw * 1000; /* mW -> uW */
			return 0;
		}
		if (attr == hwmon_power_cap &&
		    p->pwr_cap_off[ch] != OFF_UNKNOWN) {
			if (p->pwr_eff_off[ch] != OFF_UNKNOWN)
				raw = ioread32(p->base + p->pwr_eff_off[ch]);
			else
				raw = ioread32(p->base + p->pwr_cap_off[ch]);
			*val = (long)raw * 1000; /* mW -> uW */
			return 0;
		}
		if (attr == hwmon_power_max &&
		    p->pwr_max_off[ch] != OFF_UNKNOWN) {
			raw = ioread32(p->base + p->pwr_max_off[ch]);
			*val = (long)raw * 1000; /* mW -> uW */
			return 0;
		}
		if (attr == hwmon_power_min &&
		    p->pwr_min_off[ch] != OFF_UNKNOWN) {
			raw = ioread32(p->base + p->pwr_min_off[ch]);
			*val = (long)raw * 1000; /* mW -> uW */
			return 0;
		}
	}

	if (type == hwmon_energy && attr == hwmon_energy_input && ch < N_NRG &&
	    p->nrg_off[ch] != OFF_UNKNOWN) {
		uint64_t uj;

		/*
		 * [RollingTheRock] ##RollingTheRock:
		 * 64-bit monotonic energy accumulation to prevent 32-bit roll-over cliffs.
		 */
		mutex_lock(&p->lock);
		raw = ioread32(p->base + p->nrg_off[ch]);
		uj = spbm_energy_acc_update(&p->energy_acc[ch], raw);
		*val = (long)uj;
		mutex_unlock(&p->lock);
		return 0;
	}

	if (type == hwmon_temp && ch < N_TEMP) {
		if (attr == hwmon_temp_input &&
		    p->temp_off[ch] != OFF_UNKNOWN) {
			raw = ioread32(p->base + p->temp_off[ch]);
			*val = (long)raw * 10; /* centidegrees -> millidegrees C */
			return 0;
		}
		if (ch == 0 && attr == hwmon_temp_crit_alarm &&
		    p->prochot_off != OFF_UNKNOWN) {
			*val = !!ioread32(p->base + p->prochot_off);
			return 0;
		}
	}

	return -EOPNOTSUPP;
}

static int spbm_read_string(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int ch, const char **str)
{
	(void)attr;
	if (type == hwmon_power && ch < N_PWR) {
		*str = pwr_chans[ch].label;
		return 0;
	}
	if (type == hwmon_energy && ch < N_NRG) {
		*str = nrg_chans[ch].label;
		return 0;
	}
	if (type == hwmon_temp && ch < N_TEMP) {
		*str = temp_chans[ch].label;
		return 0;
	}
	return -EOPNOTSUPP;
}

static int spbm_write(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int ch, long val)
{
	struct spbm_priv *p = dev_get_drvdata(dev);
	int ret = 0;

	if (type == hwmon_power && attr == hwmon_power_cap && ch < N_PWR &&
	    p->pwr_cap_off[ch] != OFF_UNKNOWN) {
		u32 mw = (u32)(val / 1000);

		/*
		 * [RollingTheRock] ##RollingTheRock:
		 * Mutex critical section protecting register write and firmware handshake.
		 */
		mutex_lock(&p->lock);
		/* Enforce firmware floor <= cap <= ceiling; 0 = reset */
		if (mw > 0 && p->pwr_max_off[ch] != OFF_UNKNOWN &&
		    mw > ioread32(p->base + p->pwr_max_off[ch])) {
			ret = -EINVAL;
			goto unlock;
		}
		if (mw > 0 && p->pwr_min_off[ch] != OFF_UNKNOWN &&
		    mw < ioread32(p->base + p->pwr_min_off[ch])) {
			ret = -EINVAL;
			goto unlock;
		}
		iowrite32(mw, p->base + p->pwr_cap_off[ch]);
		iowrite32(1, p->base);	/* poke UPDATE_SPBM */

unlock:
		mutex_unlock(&p->lock);
		return ret;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_ops spbm_ops = {
	.is_visible = spbm_visible,
	.read = spbm_read,
	.write = spbm_write,
	.read_string = spbm_read_string,
};

/* Build config arrays with trailing sentinels */

static const u32 pwr_cfg[N_PWR + 1] = {
	[0 ... N_PWR - 1] = HWMON_P_INPUT | HWMON_P_LABEL | HWMON_P_CAP |
			    HWMON_P_MAX | HWMON_P_MIN,
	[N_PWR] = 0,
};

static const u32 nrg_cfg[N_NRG + 1] = {
	[0 ... N_NRG - 1] = HWMON_E_INPUT | HWMON_E_LABEL,
	[N_NRG] = 0,
};

static const u32 temp_cfg[N_TEMP + 1] = {
	[0] = HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_CRIT_ALARM,
	[1 ... N_TEMP - 1] = HWMON_T_INPUT | HWMON_T_LABEL,
	[N_TEMP] = 0,
};

static const struct hwmon_channel_info pwr_info = {
	.type = hwmon_power,
	.config = pwr_cfg,
};

static const struct hwmon_channel_info nrg_info = {
	.type = hwmon_energy,
	.config = nrg_cfg,
};

static const struct hwmon_channel_info temp_info = {
	.type = hwmon_temp,
	.config = temp_cfg,
};

static const struct hwmon_channel_info * const spbm_info[] = {
	&pwr_info, &nrg_info, &temp_info, NULL,
};

static const struct hwmon_chip_info spbm_chip = {
	.ops = &spbm_ops,
	.info = spbm_info,
};

/* ACPI _DSM helpers */

static int spbm_dsm_find_resource(acpi_handle handle, const char *name)
{
	union acpi_object *out, *elem;
	int i, ret = -ENOENT;

	out = acpi_evaluate_dsm(handle, &mtel_dsm_guid, 0, 1, NULL);
	if (!out)
		return -ENODEV;

	if (out->type != ACPI_TYPE_PACKAGE) {
		ret = -EINVAL;
		goto free;
	}

	for (i = 0; i < out->package.count; i++) {
		elem = &out->package.elements[i];
		if (elem->type == ACPI_TYPE_STRING &&
		    !strcmp(elem->string.pointer, name)) {
			ret = i;
			break;
		}
	}

free:
	ACPI_FREE(out);
	return ret;
}

static bool spbm_try_resolve(const char *key, u64 offset,
			     resource_size_t res_size,
			     const struct spbm_chan *chans, u32 *offsets,
			     int n)
{
	int i;

	/* [RollingTheRock] ##RollingTheRock: Enforce MMIO bounds defense */
	if (spbm_validate_bounds(offset, sizeof(u32), res_size) != 0)
		return false;

	for (i = 0; i < n; i++) {
		if (!strcmp(chans[i].dsm_key, key)) {
			offsets[i] = (u32)offset;
			return true;
		}
	}
	return false;
}

static int spbm_dsm_resolve_offsets(struct device *dev, acpi_handle handle,
				    int sub_idx, struct spbm_priv *p)
{
	union acpi_object arg_elem, arg_pkg;
	union acpi_object *out, *sub, *elem;
	int i, j, count, resolved = 0;

	(void)dev;
	arg_elem.type = ACPI_TYPE_INTEGER;
	arg_elem.integer.value = sub_idx;
	arg_pkg.type = ACPI_TYPE_PACKAGE;
	arg_pkg.package.count = 1;
	arg_pkg.package.elements = &arg_elem;

	out = acpi_evaluate_dsm(handle, &mtel_dsm_guid, 0, 2, &arg_pkg);
	if (!out)
		return -ENODEV;

	if (out->type != ACPI_TYPE_PACKAGE) {
		ACPI_FREE(out);
		return -EINVAL;
	}

	for (i = 0; i < out->package.count; i++) {
		sub = &out->package.elements[i];
		if (sub->type != ACPI_TYPE_PACKAGE || sub->package.count < 3)
			continue;

		elem = sub->package.elements;
		if (elem[0].type != ACPI_TYPE_INTEGER)
			continue;
		count = elem[0].integer.value;

		for (j = 0; j < count; j++) {
			int ni = 1 + j * 2;
			int oi = 2 + j * 2;

			if (oi >= sub->package.count)
				break;
			if (elem[ni].type != ACPI_TYPE_STRING ||
			    elem[oi].type != ACPI_TYPE_INTEGER)
				continue;

			if (spbm_try_resolve(elem[ni].string.pointer,
					     elem[oi].integer.value,
					     p->res_size,
					     pwr_chans, p->pwr_off, N_PWR) ||
			    spbm_try_resolve(elem[ni].string.pointer,
					     elem[oi].integer.value,
					     p->res_size,
					     nrg_chans, p->nrg_off, N_NRG) ||
			    spbm_try_resolve(elem[ni].string.pointer,
					     elem[oi].integer.value,
					     p->res_size,
					     temp_chans, p->temp_off, N_TEMP) ||
			    spbm_try_resolve(elem[ni].string.pointer,
					     elem[oi].integer.value,
					     p->res_size,
					     pl_os_chans, p->pl_os_off,
					     N_PL_OS) ||
			    spbm_try_resolve(elem[ni].string.pointer,
					     elem[oi].integer.value,
					     p->res_size,
					     pwr_high_chans, p->pwr_high_off,
					     N_PWR_HIGH) ||
			    spbm_try_resolve(elem[ni].string.pointer,
					     elem[oi].integer.value,
					     p->res_size,
					     pwr_low_chans, p->pwr_low_off,
					     N_PWR_LOW) ||
			    spbm_try_resolve(elem[ni].string.pointer,
					     elem[oi].integer.value,
					     p->res_size,
					     pwr_eff_chans, p->pwr_eff_resolve,
					     N_PWR_EFF))
				resolved++;

			if (!strcmp(elem[ni].string.pointer, "SPBM_PROCHOT_STATUS_OFFSET") &&
			    spbm_validate_bounds(elem[oi].integer.value, sizeof(u32), p->res_size) == 0) {
				p->prochot_off = (u32)elem[oi].integer.value;
				resolved++;
			}
		}
	}

	ACPI_FREE(out);
	return resolved;
}

/* ACPI driver */

static int spbm_add(struct acpi_device *adev)
{
	struct device *dev = &adev->dev;
	struct list_head res_list;
	struct resource_entry *re;
	resource_size_t phys = 0;
	struct spbm_priv *p;
	struct device *hwdev;
	int spbm_idx, idx = 0, ret, resolved, i, j;

	/* Stage 1: DMI Whitelist Verification */
	if (!force && !dmi_check_system(spbm_dmi_table)) {
		dev_warn(dev, "Platform not recognized by DMI whitelist (use force=1 to override)\n");
		return -ENODEV;
	}

	if (force && !dmi_check_system(spbm_dmi_table))
		dev_info(dev, "Forcing driver load on unrecognized DMI platform\n");


	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	mutex_init(&p->lock);

	memset(p->pwr_off, 0xFF, sizeof(p->pwr_off));
	memset(p->pwr_cap_off, 0xFF, sizeof(p->pwr_cap_off));
	memset(p->pwr_max_off, 0xFF, sizeof(p->pwr_max_off));
	memset(p->pwr_min_off, 0xFF, sizeof(p->pwr_min_off));
	memset(p->pwr_eff_off, 0xFF, sizeof(p->pwr_eff_off));
	memset(p->pwr_high_off, 0xFF, sizeof(p->pwr_high_off));
	memset(p->pwr_low_off, 0xFF, sizeof(p->pwr_low_off));
	memset(p->pwr_eff_resolve, 0xFF, sizeof(p->pwr_eff_resolve));
	memset(p->nrg_off, 0xFF, sizeof(p->nrg_off));
	memset(p->temp_off, 0xFF, sizeof(p->temp_off));
	p->prochot_off = OFF_UNKNOWN;
	memset(p->pl_os_off, 0xFF, sizeof(p->pl_os_off));

	spbm_idx = spbm_dsm_find_resource(adev->handle, "SPBM");
	if (spbm_idx < 0) {
		dev_dbg(dev, "_DSM did not advertise SPBM resource (%d)\n",
			spbm_idx);
		return spbm_idx;
	}

	INIT_LIST_HEAD(&res_list);
	ret = acpi_dev_get_resources(adev, &res_list, NULL, NULL);
	if (ret < 0)
		return ret;

	list_for_each_entry(re, &res_list, node) {
		if (resource_type(re->res) == IORESOURCE_MEM) {
			if (idx == spbm_idx) {
				phys = re->res->start;
				p->res_size = resource_size(re->res);
				break;
			}
			idx++;
		}
	}
	acpi_dev_free_resource_list(&res_list);

	if (!phys || p->res_size < sizeof(u32)) {
		dev_err(dev, "SPBM memory resource invalid in _CRS\n");
		return -ENODEV;
	}

	p->base = devm_ioremap(dev, phys, p->res_size);
	if (!p->base)
		return -ENOMEM;

	resolved = 0;
	for (i = 0; i < SPBM_MAX_DSM_INDICES; i++) {
		ret = spbm_dsm_resolve_offsets(dev, adev->handle, i, p);
		if (ret == -ENODEV)
			break;
		if (ret > 0)
			resolved += ret;
	}

	dev_dbg(dev, "resolved %d register offsets from _DSM\n", resolved);

	for (i = 0; i < N_PL_OS; i++) {
		size_t plen;

		if (p->pl_os_off[i] == OFF_UNKNOWN)
			continue;
		for (j = 0; j < N_PWR; j++) {
			plen = strlen(pwr_chans[j].label);
			if (!strncmp(pl_os_chans[i].label,
				     pwr_chans[j].label, plen) &&
			    !strcmp(pl_os_chans[i].label + plen, "_os")) {
				p->pwr_cap_off[j] = p->pl_os_off[i];
				break;
			}
		}
	}

	for (i = 0; i < N_PWR_HIGH; i++) {
		if (p->pwr_high_off[i] == OFF_UNKNOWN)
			continue;
		for (j = 0; j < N_PWR; j++) {
			if (!strcmp(pwr_high_chans[i].label,
				   pwr_chans[j].label)) {
				p->pwr_max_off[j] = p->pwr_high_off[i];
				break;
			}
		}
	}
	for (i = 0; i < N_PWR_LOW; i++) {
		if (p->pwr_low_off[i] == OFF_UNKNOWN)
			continue;
		for (j = 0; j < N_PWR; j++) {
			if (!strcmp(pwr_low_chans[i].label,
				   pwr_chans[j].label)) {
				p->pwr_min_off[j] = p->pwr_low_off[i];
				break;
			}
		}
	}

	for (i = 0; i < N_PWR_EFF; i++) {
		if (p->pwr_eff_resolve[i] == OFF_UNKNOWN)
			continue;
		for (j = 0; j < N_PWR; j++) {
			if (!strcmp(pwr_eff_chans[i].label,
				   pwr_chans[j].label)) {
				p->pwr_eff_off[j] = p->pwr_eff_resolve[i];
				break;
			}
		}
	}

	/* Initialize 64-bit energy accumulators with initial readings */
	for (i = 0; i < N_NRG; i++) {
		if (p->nrg_off[i] != OFF_UNKNOWN) {
			u32 init_raw;

			init_raw = ioread32(p->base + p->nrg_off[i]);
			spbm_energy_acc_init(&p->energy_acc[i], init_raw);
		}
	}

	hwdev = devm_hwmon_device_register_with_info(dev, DRIVER_NAME, p,
						     &spbm_chip, NULL);
	if (IS_ERR(hwdev))
		return PTR_ERR(hwdev);

	dev_info(dev, "probed %zu power, %zu energy, %zu temp channels\n",
		 N_PWR, N_NRG, N_TEMP);

	return 0;
}

static const struct acpi_device_id spbm_acpi_ids[] = {
	{ "NVDA8800", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, spbm_acpi_ids);

static struct acpi_driver spbm_driver = {
	.name = DRIVER_NAME,
	.ids = spbm_acpi_ids,
	.ops = {
		.add = spbm_add,
	},
};
module_acpi_driver(spbm_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrew Wang <RollingTheRock>");
MODULE_DESCRIPTION("NVIDIA DGX Spark (GB10) SPBM power hwmon driver");
