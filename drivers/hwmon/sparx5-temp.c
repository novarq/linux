// SPDX-License-Identifier: GPL-2.0-or-later
/* Sparx5 SoC temperature sensor driver
 *
 * Copyright (C) 2020 Lars Povlsen <lars.povlsen@microchip.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define TEMP_CTRL		0
#define TEMP_CFG		4
#define  TEMP_CFG_CYCLES	GENMASK(24, 15)
#define  TEMP_CFG_ENA		BIT(0)
#define TEMP_STAT		8
#define  TEMP_STAT_VALID	BIT(12)
#define  TEMP_STAT_TEMP		GENMASK(11, 0)

#define FAN_CFG			0
#define  FAN_CFG_STAT_MODE	BIT(0)
#define FAN_PWM_FREQ		4
#define  FAN_PWM_FREQ_CYCLES	GENMASK(27, 16)
#define FAN_CNT			8
#define  FAN_CNT_DATA		GENMASK(15, 0)

#define FAN_TACH_INTERVAL_HZ	100000

struct s5_match_data {
	bool has_fan;
};

struct s5_hwmon {
	void __iomem *base;
	void __iomem *fan;
	struct clk *clk;
	unsigned long clk_rate;
	u32 pulses_per_revolution;
};

static void s5_temp_enable(struct s5_hwmon *hwmon)
{
	u32 val = readl(hwmon->base + TEMP_CFG);
	u32 clk = clk_get_rate(hwmon->clk) / USEC_PER_SEC;

	val &= ~TEMP_CFG_CYCLES;
	val |= FIELD_PREP(TEMP_CFG_CYCLES, clk);
	val |= TEMP_CFG_ENA;

	writel(val, hwmon->base + TEMP_CFG);
}

static void s5_fan_enable(struct s5_hwmon *hwmon)
{
	u32 cycles, val;

	cycles = DIV_ROUND_CLOSEST(hwmon->clk_rate, FAN_TACH_INTERVAL_HZ);
	cycles = clamp_val(cycles, 1, FIELD_MAX(FAN_PWM_FREQ_CYCLES));

	val = readl(hwmon->fan + FAN_PWM_FREQ);
	val &= ~FAN_PWM_FREQ_CYCLES;
	val |= FIELD_PREP(FAN_PWM_FREQ_CYCLES, cycles);
	writel(val, hwmon->fan + FAN_PWM_FREQ);

	/* Count tachometer pulses over one-second intervals. */
	val = readl(hwmon->fan + FAN_CFG);
	val &= ~FAN_CFG_STAT_MODE;
	writel(val, hwmon->fan + FAN_CFG);
}

static int s5_read_temp(struct s5_hwmon *hwmon, long *temp)
{
	int value;
	u32 stat;

	stat = readl_relaxed(hwmon->base + TEMP_STAT);
	if (!(stat & TEMP_STAT_VALID))
		return -EAGAIN;
	value = stat & TEMP_STAT_TEMP;
	/*
	 * From register documentation:
	 * Temp(C) = TEMP_SENSOR_STAT.TEMP / 4096 * 352.2 - 109.4
	 */
	value = DIV_ROUND_CLOSEST(value * 3522, 4096) - 1094;
	/*
	 * Scale down by 10 from above and multiply by 1000 to
	 * have millidegrees as specified by the hwmon sysfs
	 * interface.
	 */
	value *= 100;
	*temp = value;

	return 0;
}

static int s5_read_fan(struct s5_hwmon *hwmon, long *val)
{
	u32 count;

	count = readl_relaxed(hwmon->fan + FAN_CNT);
	count = FIELD_GET(FAN_CNT_DATA, count);
	*val = count * 60 / hwmon->pulses_per_revolution;

	return 0;
}

static int s5_read(struct device *dev, enum hwmon_sensor_types type,
		   u32 attr, int channel, long *val)
{
	struct s5_hwmon *hwmon = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		if (attr == hwmon_temp_input)
			return s5_read_temp(hwmon, val);
		break;
	case hwmon_fan:
		if (attr == hwmon_fan_input && hwmon->fan)
			return s5_read_fan(hwmon, val);
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static umode_t s5_is_visible(const void *_data, enum hwmon_sensor_types type,
			     u32 attr, int channel)
{
	const struct s5_hwmon *hwmon = _data;

	switch (type) {
	case hwmon_temp:
		return attr == hwmon_temp_input ? 0444 : 0;
	case hwmon_fan:
		if (hwmon->fan && attr == hwmon_fan_input)
			return 0444;
		return 0;
	default:
		return 0;
	}
}

static const struct hwmon_channel_info * const s5_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	NULL
};

static const struct hwmon_ops s5_hwmon_ops = {
	.is_visible = s5_is_visible,
	.read = s5_read,
};

static const struct hwmon_chip_info s5_chip_info = {
	.ops = &s5_hwmon_ops,
	.info = s5_info,
};

static int s5_temp_probe(struct platform_device *pdev)
{
	const struct s5_match_data *match_data;
	struct device *dev = &pdev->dev;
	struct device *hwmon_dev;
	struct s5_hwmon *hwmon;
	struct device_node *fan_np __free(device_node) = NULL;

	match_data = device_get_match_data(dev);

	hwmon = devm_kzalloc(dev, sizeof(*hwmon), GFP_KERNEL);
	if (!hwmon)
		return -ENOMEM;

	if (match_data && match_data->has_fan)
		hwmon->base = devm_platform_ioremap_resource_byname(pdev, "temp");
	else
		hwmon->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(hwmon->base))
		return PTR_ERR(hwmon->base);

	if (match_data && match_data->has_fan) {
		hwmon->fan = devm_platform_ioremap_resource_byname(pdev, "fan");
		if (IS_ERR(hwmon->fan))
			return PTR_ERR(hwmon->fan);

		hwmon->pulses_per_revolution = 2;
		fan_np = of_get_child_by_name(dev->of_node, "fan");
		if (fan_np)
			of_property_read_u32(fan_np, "pulses-per-revolution",
					     &hwmon->pulses_per_revolution);
		if (!hwmon->pulses_per_revolution ||
		    hwmon->pulses_per_revolution > 4)
			return dev_err_probe(dev, -EINVAL,
					     "invalid pulses-per-revolution\n");
	}

	hwmon->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(hwmon->clk))
		return PTR_ERR(hwmon->clk);
	hwmon->clk_rate = clk_get_rate(hwmon->clk);

	s5_temp_enable(hwmon);
	if (hwmon->fan)
		s5_fan_enable(hwmon);

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "s5_temp",
							 hwmon,
							 &s5_chip_info,
							 NULL);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct s5_match_data lan969x_match_data = {
	.has_fan = true,
};

static const struct of_device_id s5_temp_match[] = {
	{
		.compatible = "microchip,lan9691-hwmon",
		.data = &lan969x_match_data,
	},
	{ .compatible = "microchip,sparx5-temp" },
	{},
};
MODULE_DEVICE_TABLE(of, s5_temp_match);

static struct platform_driver s5_temp_driver = {
	.probe = s5_temp_probe,
	.driver = {
		.name = "sparx5-temp",
		.of_match_table = s5_temp_match,
	},
};

module_platform_driver(s5_temp_driver);

MODULE_AUTHOR("Lars Povlsen <lars.povlsen@microchip.com>");
MODULE_DESCRIPTION("Sparx5 SoC hardware monitoring driver");
MODULE_LICENSE("GPL");
