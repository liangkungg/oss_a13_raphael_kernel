/*
 * Copyright (c) 2011-2015, 2017-2019 The Linux Foundation. All rights reserved.
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

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/iio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/thermal.h>

#define QPNP_TM_REG_DIG_MAJOR		0x01
#define QPNP_TM_REG_TYPE		0x04
#define QPNP_TM_REG_SUBTYPE		0x05
#define QPNP_TM_REG_STATUS		0x08
#define QPNP_TM_REG_SHUTDOWN_CTRL1	0x40
#define QPNP_TM_REG_ALARM_CTRL		0x46

#define QPNP_TM_TYPE			0x09
#define QPNP_TM_SUBTYPE_GEN1		0x08
#define QPNP_TM_SUBTYPE_GEN2		0x09

#define STATUS_GEN1_STAGE_MASK		GENMASK(1, 0)
#define STATUS_GEN2_STATE_MASK		GENMASK(6, 4)
#define STATUS_GEN2_STATE_SHIFT		4

#define SHUTDOWN_CTRL1_OVERRIDE_MASK	GENMASK(7, 6)
#define SHUTDOWN_CTRL1_THRESHOLD_MASK	GENMASK(1, 0)

#define ALARM_CTRL_FORCE_ENABLE		BIT(7)

#define THRESH_COUNT			4
#define STAGE_COUNT			3

/* Over-temperature trip point values in mC */
static const long temp_map_gen1[THRESH_COUNT][STAGE_COUNT] = {
	{105000, 125000, 145000},
	{110000, 130000, 150000},
	{115000, 135000, 155000},
	{120000, 140000, 160000},
};

static const long temp_map_gen2_v1[THRESH_COUNT][STAGE_COUNT] = {
	{ 90000, 110000, 140000},
	{ 95000, 115000, 145000},
	{100000, 120000, 150000},
	{105000, 125000, 155000},
};

#define TEMP_STAGE_HYSTERESIS		2000

#define THRESH_MIN			0
#define THRESH_MAX			3

/* Temperature in Milli Celsius reported during stage 0 if no ADC is present */
#define DEFAULT_TEMP			37000

struct qpnp_tm_chip {
	struct regmap			*map;
	struct thermal_zone_device	*tz_dev;
	unsigned int			subtype;
	long				temp;
	unsigned int			thresh;
	unsigned int			stage;
	unsigned int			prev_stage;
	unsigned int			base;
	int				irq;
	u32				init_thresh;
	struct iio_channel		*adc;
	const long			(*temp_map)[THRESH_COUNT][STAGE_COUNT];
};

/* This array maps from GEN2 alarm state to GEN1 alarm stage */
static const unsigned int alarm_state_map[8] = {0, 1, 1, 2, 2, 3, 3, 3};

static int qpnp_tm_read(struct qpnp_tm_chip *chip, u16 addr, u8 *data)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chip->map, chip->base + addr, &val);
	if (ret < 0)
		return ret;

	*data = val;
	return 0;
}

static int qpnp_tm_write(struct qpnp_tm_chip *chip, u16 addr, u8 data)
{
	return regmap_write(chip->map, chip->base + addr, data);
}

/**
 * qpnp_tm_decode_temp() - return temperature in mC corresponding to the
 *		specified over-temperature stage
 * @chip:		Pointer to the qpnp_tm chip
 * @stage:		Over-temperature stage
 *
 * Return: temperature in mC
 */
static long qpnp_tm_decode_temp(struct qpnp_tm_chip *chip, unsigned int stage)
{
	if (!chip->temp_map || chip->thresh >= THRESH_COUNT || stage == 0
	    || stage > STAGE_COUNT)
		return 0;

	return (*chip->temp_map)[chip->thresh][stage - 1];
}

/**
 * qpnp_tm_get_temp_stage() - return over-temperature stage
 * @chip:		Pointer to the qpnp_tm chip
 *
 * Return: stage (GEN1) or state (GEN2) on success, or errno on failure.
 */
static int qpnp_tm_get_temp_stage(struct qpnp_tm_chip *chip)
{
	int ret;
	u8 reg = 0;

	ret = qpnp_tm_read(chip, QPNP_TM_REG_STATUS, &reg);
	if (ret < 0)
		return ret;

	if (chip->subtype == QPNP_TM_SUBTYPE_GEN1)
		ret = reg & STATUS_GEN1_STAGE_MASK;
	else
		ret = (reg & STATUS_GEN2_STATE_MASK) >> STATUS_GEN2_STATE_SHIFT;

	return ret;
}

/*
 * This function updates the internal temp value based on the
 * current thermal stage and threshold as well as the previous stage
 */
static int qpnp_tm_update_temp_no_adc(struct qpnp_tm_chip *chip)
{
	unsigned int stage, stage_new, stage_old;
	int ret;

	ret = qpnp_tm_get_temp_stage(chip);
	if (ret < 0)
		return ret;
	stage = ret;

	if (chip->subtype == QPNP_TM_SUBTYPE_GEN1) {
		stage_new = stage;
		stage_old = chip->stage;
	} else {
		stage_new = alarm_state_map[stage];
		stage_old = alarm_state_map[chip->stage];
	}

	if (stage_new > stage_old) {
		/* increasing stage, use lower bound */
		chip->temp = qpnp_tm_decode_temp(chip, stage_new)
				+ TEMP_STAGE_HYSTERESIS;
	} else if (stage_new < stage_old) {
		/* decreasing stage, use upper bound */
		chip->temp = qpnp_tm_decode_temp(chip, stage_new + 1)
				- TEMP_STAGE_HYSTERESIS;
	}

	chip->stage = stage;

	return 0;
}

static int qpnp_tm_get_temp(void *data, int *temp)
{
	struct qpnp_tm_chip *chip = data;
	int ret, mili_celsius;

	if (!temp)
		return -EINVAL;

	if (IS_ERR(chip->adc)) {
		ret = qpnp_tm_update_temp_no_adc(chip);
		if (ret < 0)
			return ret;
	} else {
		ret = iio_read_channel_processed(chip->adc, &mili_celsius);
		if (ret < 0)
			return ret;

		chip->temp = mili_celsius;
	}

	*temp = chip->temp;

	return 0;
}

static const struct thermal_zone_of_device_ops qpnp_tm_sensor_ops = {
	.get_temp = qpnp_tm_get_temp,
};

static irqreturn_t qpnp_tm_isr(int irq, void *data)
{
	struct qpnp_tm_chip *chip = data;

	thermal_zone_device_update(chip->tz_dev, THERMAL_EVENT_UNSPECIFIED);

	return IRQ_HANDLED;
}

/*
 * This function initializes the internal temp value based on only the
 * current thermal stage and threshold. Setup threshold control and
 * disable shutdown override.
 */
static int qpnp_tm_init(struct qpnp_tm_chip *chip)
{
	unsigned int stage;
	int ret;
	u8 reg = 0;

	ret = qpnp_tm_read(chip, QPNP_TM_REG_SHUTDOWN_CTRL1, &reg);
	if (ret < 0)
		return ret;

	chip->thresh = reg & SHUTDOWN_CTRL1_THRESHOLD_MASK;
	chip->temp = DEFAULT_TEMP;

	ret = qpnp_tm_get_temp_stage(chip);
	if (ret < 0)
		return ret;
	chip->stage = ret;

	stage = chip->subtype == QPNP_TM_SUBTYPE_GEN1
		? chip->stage : alarm_state_map[chip->stage];

	if (stage)
		chip->temp = qpnp_tm_decode_temp(chip, stage);

	/*
	 * Set threshold and disable software override of stage 2 and 3
	 * shutdowns.
	 */
	chip->thresh = chip->init_thresh;
	reg &= ~(SHUTDOWN_CTRL1_OVERRIDE_MASK | SHUTDOWN_CTRL1_THRESHOLD_MASK);
	reg |= chip->thresh & SHUTDOWN_CTRL1_THRESHOLD_MASK;
	ret = qpnp_tm_write(chip, QPNP_TM_REG_SHUTDOWN_CTRL1, reg);
	if (ret < 0)
		return ret;

	/* Enable the thermal alarm PMIC module in always-on mode. */
	reg = ALARM_CTRL_FORCE_ENABLE;
	ret = qpnp_tm_write(chip, QPNP_TM_REG_ALARM_CTRL, reg);

	return ret;
}

static int qpnp_tm_probe(struct platform_device *pdev)
{
	struct qpnp_tm_chip *chip;
	struct device_node *node;
	u8 type, subtype, dig_major;
	unsigned long int flags;
	u32 res;
	int ret;

	node = pdev->dev.of_node;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, chip);

	chip->map = dev_get_regmap(pdev->dev.parent, NULL);
	if (!chip->map)
		return -ENXIO;

	ret = of_property_read_u32(node, "reg", &res);
	if (ret < 0)
		return ret;

	chip->init_thresh = THRESH_MIN;
	if (of_property_read_u32(node, "qcom,temperature-threshold-set",
				 &chip->init_thresh) == 0) {
		if (chip->init_thresh > THRESH_MAX) {
			dev_err(&pdev->dev, "Invalid qcom,temperature-threshold-set=%u\n",
				chip->init_thresh);
			return -EINVAL;
		}
	}

	chip->irq = platform_get_irq(pdev, 0);
	if (chip->irq < 0)
		return chip->irq;

	/* ADC based measurements are optional */
	chip->adc = iio_channel_get(&pdev->dev, "thermal");
	if (PTR_ERR(chip->adc) == -EPROBE_DEFER)
		return PTR_ERR(chip->adc);

	chip->base = res;

	ret = qpnp_tm_read(chip, QPNP_TM_REG_TYPE, &type);
	if (ret < 0) {
		dev_err(&pdev->dev, "could not read type\n");
		goto fail;
	}

	ret = qpnp_tm_read(chip, QPNP_TM_REG_SUBTYPE, &subtype);
	if (ret < 0) {
		dev_err(&pdev->dev, "could not read subtype\n");
		goto fail;
	}

	ret = qpnp_tm_read(chip, QPNP_TM_REG_DIG_MAJOR, &dig_major);
	if (ret < 0) {
		dev_err(&pdev->dev, "could not read dig_major\n");
		goto fail;
	}

	if (type != QPNP_TM_TYPE || (subtype != QPNP_TM_SUBTYPE_GEN1
				     && subtype != QPNP_TM_SUBTYPE_GEN2)) {
		dev_err(&pdev->dev, "invalid type 0x%02x or subtype 0x%02x\n",
			type, subtype);
		ret = -ENODEV;
		goto fail;
	}

	chip->subtype = subtype;

	if (subtype == QPNP_TM_SUBTYPE_GEN2 && dig_major >= 1)
		chip->temp_map = &temp_map_gen2_v1;
	else
		chip->temp_map = &temp_map_gen1;

	ret = qpnp_tm_init(chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "init failed\n");
		goto fail;
	}

	if (subtype == QPNP_TM_SUBTYPE_GEN2) {
		/*
		 * The interrupt signal on TEMP_GEN2 modules is low when the
		 * over-temperature stage is 0 and high when the stage is
		 * greater than 0.  Therefore, triggering on both edges is
		 * required in order to detect both stage 0 -> 1 and 1 -> 0
		 * transitions.
		 *
		 * There is no mechanism to receive interrupts on other stage
		 * transitions (e.g. 1 -> 2 or 2 -> 1).
		 */
		flags = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;
	} else {
		/*
		 * The interrupt signal on older modules provides a short pulse
		 * on every over-temperature stage transition (e.g. 0 -> 1,
		 * 1 -> 0, 1 -> 2, 2 -> 1, etc).  Therefore, triggering should
		 * only be performed on the rising edge.
		 */
		flags = IRQF_TRIGGER_RISING;
	}

	ret = devm_request_threaded_irq(&pdev->dev, chip->irq, NULL,
			qpnp_tm_isr, flags | IRQF_ONESHOT, node->name, chip);
	if (ret < 0)
		goto fail;

	chip->tz_dev = devm_thermal_zone_of_sensor_register(&pdev->dev, 0, chip,
							&qpnp_tm_sensor_ops);
	if (IS_ERR(chip->tz_dev)) {
		dev_err(&pdev->dev, "failed to register sensor\n");
		ret = PTR_ERR(chip->tz_dev);
		goto fail;
	}

	return 0;

fail:
	if (!IS_ERR(chip->adc))
		iio_channel_release(chip->adc);

	return ret;
}

static int qpnp_tm_remove(struct platform_device *pdev)
{
	struct qpnp_tm_chip *chip = dev_get_drvdata(&pdev->dev);

	if (!IS_ERR(chip->adc))
		iio_channel_release(chip->adc);

	return 0;
}

static int qpnp_tm_restore(struct device *dev)
{
	int ret = 0;
	struct qpnp_tm_chip *chip = dev_get_drvdata(dev);
	struct device_node *node = dev->of_node;
	unsigned long int flags;

	if (chip->subtype == QPNP_TM_SUBTYPE_GEN2)
		flags = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;
	else
		flags = IRQF_TRIGGER_RISING;

	if (chip->irq > 0) {
		ret = devm_request_threaded_irq(dev, chip->irq, NULL,
			qpnp_tm_isr, flags | IRQF_ONESHOT, node->name, chip);
		if (ret < 0)
			return ret;
	}

	ret = qpnp_tm_init(chip);
	if (ret < 0)
		dev_err(dev, "init failed\n");

	return ret;
}

static int qpnp_tm_freeze(struct device *dev)
{
	struct qpnp_tm_chip *chip = dev_get_drvdata(dev);

	if (chip->irq > 0)
		devm_free_irq(dev, chip->irq, chip);

	return 0;
}

static const struct dev_pm_ops qpnp_tm_pm_ops = {
	.freeze = qpnp_tm_freeze,
	.restore = qpnp_tm_restore,
};

static const struct of_device_id qpnp_tm_match_table[] = {
	{ .compatible = "qcom,spmi-temp-alarm" },
	{ }
};
MODULE_DEVICE_TABLE(of, qpnp_tm_match_table);

static struct platform_driver qpnp_tm_driver = {
	.driver = {
		.name = "spmi-temp-alarm",
		.of_match_table = qpnp_tm_match_table,
		.pm = &qpnp_tm_pm_ops,
	},
	.probe  = qpnp_tm_probe,
	.remove = qpnp_tm_remove,
};
module_platform_driver(qpnp_tm_driver);

MODULE_ALIAS("platform:spmi-temp-alarm");
MODULE_DESCRIPTION("QPNP PMIC Temperature Alarm driver");
MODULE_LICENSE("GPL v2");
