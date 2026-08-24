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
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include <dt-bindings/pwm/pwm.h>

#define TEMP_CTRL		0
#define TEMP_CFG		4
#define  TEMP_CFG_CYCLES	GENMASK(24, 15)
#define  TEMP_CFG_ENA		BIT(0)
#define TEMP_STAT		8
#define  TEMP_STAT_VALID	BIT(12)
#define  TEMP_STAT_TEMP		GENMASK(11, 0)

#define FAN_CFG			0
#define  FAN_CFG_DUTY_CYCLE	GENMASK(23, 16)
#define  FAN_CFG_INV_POL	BIT(3)
#define  FAN_CFG_STAT_MODE	BIT(0)
#define FAN_PWM_FREQ		4
#define  FAN_PWM_FREQ_CYCLES	GENMASK(27, 16)
#define  FAN_PWM_FREQ_DIV	GENMASK(15, 0)
#define FAN_CNT			8
#define  FAN_CNT_DATA		GENMASK(15, 0)

#define FAN_TACH_INTERVAL_HZ	100000
#define FAN_PWM_FREQ_SCALE	256

struct s5_match_data {
	bool has_fan;
};

struct s5_hwmon {
	void __iomem *base;
	void __iomem *fan;
	struct clk *clk;
	struct mutex lock; /* protects fan controller state and registers */
	unsigned long clk_rate;
	u32 pulses_per_revolution;
	u32 pwm_frequency;
	bool pwm_inverted;
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
	val &= ~(FAN_CFG_STAT_MODE | FAN_CFG_INV_POL);
	if (hwmon->pwm_inverted)
		val |= FAN_CFG_INV_POL;
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

static int s5_read_pwm(struct s5_hwmon *hwmon, long *val)
{
	u32 data;

	mutex_lock(&hwmon->lock);
	data = readl_relaxed(hwmon->fan + FAN_CFG);
	mutex_unlock(&hwmon->lock);
	*val = FIELD_GET(FAN_CFG_DUTY_CYCLE, data);

	return 0;
}

static int s5_read_pwm_freq(struct s5_hwmon *hwmon, long *val)
{
	unsigned long rate;
	u32 divider;

	divider = readl_relaxed(hwmon->fan + FAN_PWM_FREQ);
	divider = FIELD_GET(FAN_PWM_FREQ_DIV, divider);
	if (!divider)
		return -ENODATA;

	rate = DIV_ROUND_CLOSEST(hwmon->clk_rate, FAN_PWM_FREQ_SCALE);
	*val = DIV_ROUND_CLOSEST(rate, divider);

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
	case hwmon_pwm:
		if (!hwmon->fan)
			break;
		switch (attr) {
		case hwmon_pwm_input:
			return s5_read_pwm(hwmon, val);
		case hwmon_pwm_freq:
			return s5_read_pwm_freq(hwmon, val);
		default:
			break;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int s5_write_pwm(struct s5_hwmon *hwmon, long val)
{
	u32 data;

	if (val < 0 || val > 255)
		return -EINVAL;

	mutex_lock(&hwmon->lock);

	data = readl_relaxed(hwmon->fan + FAN_CFG);
	data &= ~FAN_CFG_DUTY_CYCLE;
	data |= FIELD_PREP(FAN_CFG_DUTY_CYCLE, val);
	writel_relaxed(data, hwmon->fan + FAN_CFG);

	mutex_unlock(&hwmon->lock);

	return 0;
}

static int s5_write_pwm_freq(struct s5_hwmon *hwmon, long val)
{
	unsigned long rate;
	u32 data, divider;

	if (val <= 0)
		return -EINVAL;

	rate = DIV_ROUND_CLOSEST(hwmon->clk_rate, FAN_PWM_FREQ_SCALE);
	divider = DIV_ROUND_CLOSEST(rate, val);
	divider = clamp_val(divider, 1, FIELD_MAX(FAN_PWM_FREQ_DIV));

	mutex_lock(&hwmon->lock);
	data = readl_relaxed(hwmon->fan + FAN_PWM_FREQ);
	data &= ~FAN_PWM_FREQ_DIV;
	data |= FIELD_PREP(FAN_PWM_FREQ_DIV, divider);
	writel_relaxed(data, hwmon->fan + FAN_PWM_FREQ);
	mutex_unlock(&hwmon->lock);

	return 0;
}

static int s5_write(struct device *dev, enum hwmon_sensor_types type,
		    u32 attr, int channel, long val)
{
	struct s5_hwmon *hwmon = dev_get_drvdata(dev);

	if (type != hwmon_pwm || !hwmon->fan)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_pwm_input:
		return s5_write_pwm(hwmon, val);
	case hwmon_pwm_freq:
		return s5_write_pwm_freq(hwmon, val);
	default:
		return -EOPNOTSUPP;
	}
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
	case hwmon_pwm:
		if (!hwmon->fan)
			return 0;
		if (attr == hwmon_pwm_input || attr == hwmon_pwm_freq)
			return 0644;
		return 0;
	default:
		return 0;
	}
}

static const struct hwmon_channel_info * const s5_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_FREQ),
	NULL
};

static const struct hwmon_ops s5_hwmon_ops = {
	.is_visible = s5_is_visible,
	.read = s5_read,
	.write = s5_write,
};

static const struct hwmon_chip_info s5_chip_info = {
	.ops = &s5_hwmon_ops,
	.info = s5_info,
};

static int s5_get_fan_data(struct device *dev, struct device_node *fan_np,
			   struct s5_hwmon *hwmon)
{
	struct of_phandle_args args;
	int ret;

	ret = of_parse_phandle_with_args(fan_np, "pwms", "#pwm-cells", 0,
					 &args);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse fan PWM\n");

	if (args.np != dev->of_node || args.args_count != 2 || !args.args[0] ||
	    args.args[1] & ~PWM_POLARITY_INVERTED) {
		of_node_put(args.np);
		return dev_err_probe(dev, -EINVAL, "invalid fan PWM specifier\n");
	}

	hwmon->pwm_frequency = DIV_ROUND_CLOSEST(NSEC_PER_SEC, args.args[0]);
	hwmon->pwm_inverted = args.args[1] & PWM_POLARITY_INVERTED;
	of_node_put(args.np);

	hwmon->pulses_per_revolution = 2;
	of_property_read_u32(fan_np, "pulses-per-revolution",
			     &hwmon->pulses_per_revolution);
	if (!hwmon->pulses_per_revolution ||
	    hwmon->pulses_per_revolution > 4)
		return dev_err_probe(dev, -EINVAL,
				     "invalid pulses-per-revolution\n");

	return 0;
}

static int s5_temp_probe(struct platform_device *pdev)
{
	const struct s5_match_data *match_data;
	struct device *dev = &pdev->dev;
	struct device *hwmon_dev;
	struct s5_hwmon *hwmon;
	struct device_node *fan_np __free(device_node) = NULL;
	int ret;

	match_data = device_get_match_data(dev);

	hwmon = devm_kzalloc(dev, sizeof(*hwmon), GFP_KERNEL);
	if (!hwmon)
		return -ENOMEM;
	mutex_init(&hwmon->lock);

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
		if (fan_np) {
			ret = s5_get_fan_data(dev, fan_np, hwmon);
			if (ret)
				return ret;
		}
	}

	hwmon->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(hwmon->clk))
		return PTR_ERR(hwmon->clk);
	hwmon->clk_rate = clk_get_rate(hwmon->clk);

	s5_temp_enable(hwmon);
	if (hwmon->fan) {
		s5_fan_enable(hwmon);

		if (hwmon->pwm_frequency) {
			ret = s5_write_pwm_freq(hwmon, hwmon->pwm_frequency);
			if (ret)
				return ret;
		}
	}

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
