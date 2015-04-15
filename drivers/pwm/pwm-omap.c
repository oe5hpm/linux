/*
 *    Copyright (c) 2014 Joachim Eastwood <manabian@gmail.com>
 *    Copyright (c) 2012 NeilBrown <neilb@suse.de>
 *    Heavily based on earlier code which is:
 *    Copyright (c) 2010 Grant Erickson <marathon96@gmail.com>
 *
 *    Also based on pwm-samsung.c
 *
 *    This program is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU General Public License
 *    version 2 as published by the Free Software Foundation.
 *
 *    Description:
 *      This file is the core OMAP support for the generic, Linux
 *      PWM driver / controller, using the OMAP's dual-mode timers.
 *
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/time.h>

#include <../arch/arm/plat-omap/include/plat/dmtimer.h>

#define DM_TIMER_LOAD_MIN		0xFFFFFFFE

struct omap_chip {
	struct omap_dm_timer	*dm_timer;
	unsigned int		duty_ns, period_ns;
	struct mutex		mutex;
	struct pwm_chip		chip;
};

#define to_omap_chip(chip)	container_of(chip, struct omap_chip, chip)

static int omap_pwm_calc_value(unsigned long clk_rate, int ns)
{
	u64 c;

	c = (u64)clk_rate * ns;
	do_div(c, NSEC_PER_SEC);

	return DM_TIMER_LOAD_MIN - c;
}

static void omap_pwm_start(struct omap_chip *omap)
{
	/*
	 * According to OMAP 4 TRM section 22.2.4.10 the counter should be
	 * started at 0xFFFFFFFE when overflow and match is used to ensure
	 * that the PWM line is toggled on the first event.
	 *
	 * Note that omap_dm_timer_enable/disable is for register access and
	 * not the timer counter itself.
	 */
	omap_dm_timer_enable(omap->dm_timer);
	omap_dm_timer_write_counter(omap->dm_timer, DM_TIMER_LOAD_MIN);
	omap_dm_timer_disable(omap->dm_timer);

	omap_dm_timer_start(omap->dm_timer);
}

static int omap_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct omap_chip *omap = to_omap_chip(chip);

	mutex_lock(&omap->mutex);
	omap_pwm_start(omap);
	mutex_unlock(&omap->mutex);

	return 0;
}

static void omap_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct omap_chip *omap = to_omap_chip(chip);

	mutex_lock(&omap->mutex);
	omap_dm_timer_stop(omap->dm_timer);
	mutex_unlock(&omap->mutex);
}

static int omap_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			   int duty_ns, int period_ns)
{
	struct omap_chip *omap = to_omap_chip(chip);
	int load_value, match_value;
	unsigned long clk_rate;
	bool timer_active;

	dev_dbg(chip->dev, "duty cycle: %d, period %d\n", duty_ns, period_ns);

	mutex_lock(&omap->mutex);
	if (omap->duty_ns == duty_ns &&
	    omap->period_ns == period_ns) {
		/* No change - don't cause any transients. */
		mutex_unlock(&omap->mutex);
		return 0;
	}

	clk_rate = clk_get_rate(omap_dm_timer_get_fclk(omap->dm_timer));

	/*
	 * Calculate the appropriate load and match values based on the
	 * specified period and duty cycle. The load value determines the
	 * cycle time and the match value determines the duty cycle.
	 */
	load_value = omap_pwm_calc_value(clk_rate, period_ns);
	match_value = omap_pwm_calc_value(clk_rate, period_ns - duty_ns);

	/*
	 * We MUST stop the associated dual-mode timer before attempting to
	 * write its registers, but calls to omap_dm_timer_start/stop must
	 * be balanced so check if timer is active before calling timer_stop.
	 */
	timer_active = pm_runtime_active(&omap->dm_timer->pdev->dev);
	if (timer_active)
		omap_dm_timer_stop(omap->dm_timer);

	omap_dm_timer_set_load(omap->dm_timer, true, load_value);
	omap_dm_timer_set_match(omap->dm_timer, true, match_value);

	dev_dbg(chip->dev, "load value: %#08x (%d), match value: %#08x (%d)\n",
		load_value, load_value,	match_value, match_value);

	omap_dm_timer_set_pwm(omap->dm_timer,
			      pwm->polarity == PWM_POLARITY_INVERSED,
			      true,
			      OMAP_TIMER_TRIGGER_OVERFLOW_AND_COMPARE);

	/* If config was called while timer was running it must be reenabled. */
	if (timer_active)
		omap_pwm_start(omap);

	omap->duty_ns = duty_ns;
	omap->period_ns = period_ns;
	mutex_unlock(&omap->mutex);

	return 0;
}

static int omap_pwm_set_polarity(struct pwm_chip *chip, struct pwm_device *pwm,
				 enum pwm_polarity polarity)
{
	struct omap_chip *omap = to_omap_chip(chip);

	/*
	 * PWM core will not call set_polarity while PWM is enabled so it's
	 * safe to reconfigure the timer here without stopping it first.
	 */
	mutex_lock(&omap->mutex);
	omap_dm_timer_set_pwm(omap->dm_timer,
			      polarity == PWM_POLARITY_INVERSED,
			      true,
			      OMAP_TIMER_TRIGGER_OVERFLOW_AND_COMPARE);
	mutex_unlock(&omap->mutex);

	return 0;
}

static struct pwm_ops omap_pwm_ops = {
	.enable		= omap_pwm_enable,
	.disable	= omap_pwm_disable,
	.config		= omap_pwm_config,
	.set_polarity	= omap_pwm_set_polarity,
	.owner		= THIS_MODULE,
};

static int omap_pwm_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct omap_dm_timer *dm_timer;
	struct device_node *timer;
	struct omap_chip *omap;
	int status;

	timer = of_parse_phandle(np, "ti,timers", 0);
	if (!timer)
		return -ENODEV;

	dm_timer = omap_dm_timer_request_by_node(timer);
	if (!dm_timer)
		return -EPROBE_DEFER;

	if (!(dm_timer->capability & OMAP_TIMER_HAS_PWM)) {
		dev_err(&pdev->dev, "timer has no PWM capability\n");
		return -ENODEV;
	}

	omap = devm_kzalloc(&pdev->dev, sizeof(*omap), GFP_KERNEL);
	if (omap == NULL)
		return -ENOMEM;

	omap->dm_timer = dm_timer;
	omap_dm_timer_set_source(omap->dm_timer, OMAP_TIMER_SRC_SYS_CLK);

	omap->chip.dev = &pdev->dev;
	omap->chip.ops = &omap_pwm_ops;
	omap->chip.base = -1;
	omap->chip.npwm = 1;
	omap->chip.of_xlate = of_pwm_xlate_with_flags;
	omap->chip.of_pwm_n_cells = 3;

	mutex_init(&omap->mutex);

	/*
	 * Ensure that the timer is stopped before we allow PWM core to call
	 * pwm_enable.
	 */
	if (pm_runtime_active(&omap->dm_timer->pdev->dev))
		omap_dm_timer_stop(omap->dm_timer);

	status = pwmchip_add(&omap->chip);
	if (status < 0) {
		dev_err(&pdev->dev, "failed to register PWM\n");
		omap_dm_timer_free(omap->dm_timer);
		return status;
	}

	platform_set_drvdata(pdev, omap);

	return 0;
}

static int omap_pwm_remove(struct platform_device *pdev)
{
	struct omap_chip *omap = platform_get_drvdata(pdev);
	int status;

	status = pwmchip_remove(&omap->chip);
	if (status < 0)
		return status;

	if (pm_runtime_active(&omap->dm_timer->pdev->dev))
		omap_dm_timer_stop(omap->dm_timer);

	omap_dm_timer_free(omap->dm_timer);

	mutex_destroy(&omap->mutex);

	return 0;
}

static const struct of_device_id omap_pwm_of_match[] = {
	{.compatible = "ti,omap-pwm"},
	{}
};
MODULE_DEVICE_TABLE(of, omap_pwm_of_match);

static struct platform_driver omap_pwm_driver = {
	.driver = {
		.name	= "omap-pwm",
		.owner	= THIS_MODULE,
		.of_match_table = omap_pwm_of_match,
	},
	.probe		= omap_pwm_probe,
	.remove		= omap_pwm_remove,
};
module_platform_driver(omap_pwm_driver);

MODULE_AUTHOR("Grant Erickson <marathon96@gmail.com>");
MODULE_AUTHOR("NeilBrown <neilb@suse.de>");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("2014-20-04");
MODULE_DESCRIPTION("OMAP PWM Driver using Dual-mode Timers");
