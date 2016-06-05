/*
 * Copyright (C) Overkiz SAS 2012
 *
 * Author: Boris BREZILLON <b.brezillon@overkiz.com>
 * License terms: GNU General Public License (GPL) version 2
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/interrupt.h>
#include <linux/irq.h>

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <soc/at91/atmel_tcb.h>

#define NPWM	2

#define ATMEL_TC_ACMR_MASK	(ATMEL_TC_CMR_ACPA_MSK | \
				 ATMEL_TC_CMR_ACPC_MSK | \
				 ATMEL_TC_CMR_AEEVT_MSK | \
				 ATMEL_TC_CMR_ASWTRG_MSK)

#define ATMEL_TC_BCMR_MASK	(ATMEL_TC_CMR_BCPB_MSK | \
				 ATMEL_TC_CMR_BCPC_MSK | \
				 ATMEL_TC_CMR_BEEVT_MSK | \
				 ATMEL_TC_CMR_BSWTRG_MSK)

struct atmel_tcb_pwm_device {
	enum pwm_polarity polarity;	/* PWM polarity */
	unsigned div;			/* PWM clock divider */
	unsigned duty;			/* PWM duty expressed in clk cycles */
	unsigned period;		/* PWM period expressed in clk cycles */
};

struct atmel_tcb_pwm_chip {
	struct pwm_chip chip;
	spinlock_t lock;
	u8 channel;
	u8 width;
	struct regmap *regmap;
	struct clk *clk;
	struct clk *slow_clk;
	struct atmel_tcb_pwm_device *pwms[NPWM];
};

static inline struct atmel_tcb_pwm_chip *to_tcb_chip(struct pwm_chip *chip)
{
	return container_of(chip, struct atmel_tcb_pwm_chip, chip);
}

static int atmel_tcb_pwm_set_polarity(struct pwm_chip *chip,
				      struct pwm_device *pwm,
				      enum pwm_polarity polarity)
{
	struct atmel_tcb_pwm_device *tcbpwm = pwm_get_chip_data(pwm);

	tcbpwm->polarity = polarity;

	return 0;
}

static int atmel_tcb_pwm_request(struct pwm_chip *chip,
				 struct pwm_device *pwm)
{
	struct atmel_tcb_pwm_chip *tcbpwmc = to_tcb_chip(chip);
	struct atmel_tcb_pwm_device *tcbpwm;
	unsigned cmr;
	int ret;

	tcbpwm = devm_kzalloc(chip->dev, sizeof(*tcbpwm), GFP_KERNEL);
	if (!tcbpwm)
		return -ENOMEM;

	ret = clk_prepare_enable(tcbpwmc->clk);
	if (ret) {
		devm_kfree(chip->dev, tcbpwm);
		return ret;
	}

	pwm_set_chip_data(pwm, tcbpwm);
	tcbpwm->polarity = PWM_POLARITY_NORMAL;
	tcbpwm->duty = 0;
	tcbpwm->period = 0;
	tcbpwm->div = 0;

	spin_lock(&tcbpwmc->lock);
	regmap_read(tcbpwmc->regmap, ATMEL_TC_CMR(tcbpwmc->channel), &cmr);
	/*
	 * Get init config from Timer Counter registers if
	 * Timer Counter is already configured as a PWM generator.
	 */
	if (cmr & ATMEL_TC_CMR_WAVE) {
		if (pwm->hwpwm == 0)
			regmap_read(tcbpwmc->regmap,
				    ATMEL_TC_RA(tcbpwmc->channel),
				    &tcbpwm->duty);
		else
			regmap_read(tcbpwmc->regmap,
				    ATMEL_TC_RB(tcbpwmc->channel),
				    &tcbpwm->duty);

		tcbpwm->div = cmr & ATMEL_TC_CMR_TCLKS_MSK;
		regmap_read(tcbpwmc->regmap, ATMEL_TC_RC(tcbpwmc->channel),
			    &tcbpwm->period);
		cmr &= (ATMEL_TC_CMR_TCLKS_MSK | ATMEL_TC_ACMR_MASK |
			ATMEL_TC_BCMR_MASK);
	} else
		cmr = 0;

	cmr |= ATMEL_TC_CMR_WAVE | ATMEL_TC_CMR_WAVESEL_UPRC |
	       ATMEL_TC_CMR_EEVT_XC(0);
	regmap_write(tcbpwmc->regmap, ATMEL_TC_CMR(tcbpwmc->channel), cmr);
	spin_unlock(&tcbpwmc->lock);

	tcbpwmc->pwms[pwm->hwpwm] = tcbpwm;

	return 0;
}

static void atmel_tcb_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct atmel_tcb_pwm_chip *tcbpwmc = to_tcb_chip(chip);
	struct atmel_tcb_pwm_device *tcbpwm = pwm_get_chip_data(pwm);

	clk_disable_unprepare(tcbpwmc->clk);
	tcbpwmc->pwms[pwm->hwpwm] = NULL;
	devm_kfree(chip->dev, tcbpwm);
}

static void atmel_tcb_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct atmel_tcb_pwm_chip *tcbpwmc = to_tcb_chip(chip);
	struct atmel_tcb_pwm_device *tcbpwm = pwm_get_chip_data(pwm);
	unsigned cmr;
	enum pwm_polarity polarity = tcbpwm->polarity;

	/*
	 * If duty is 0 the timer will be stopped and we have to
	 * configure the output correctly on software trigger:
	 *  - set output to high if PWM_POLARITY_INVERSED
	 *  - set output to low if PWM_POLARITY_NORMAL
	 *
	 * This is why we're reverting polarity in this case.
	 */
	if (tcbpwm->duty == 0)
		polarity = !polarity;

	spin_lock(&tcbpwmc->lock);
	regmap_read(tcbpwmc->regmap, ATMEL_TC_CMR(tcbpwmc->channel), &cmr);

	/* flush old setting and set the new one */
	if (pwm->hwpwm == 0) {
		cmr &= ~ATMEL_TC_ACMR_MASK;
		if (polarity == PWM_POLARITY_INVERSED)
			cmr |= ATMEL_TC_CMR_ASWTRG(CLEAR);
		else
			cmr |= ATMEL_TC_CMR_ASWTRG(SET);
	} else {
		cmr &= ~ATMEL_TC_BCMR_MASK;
		if (polarity == PWM_POLARITY_INVERSED)
			cmr |= ATMEL_TC_CMR_BSWTRG(CLEAR);
		else
			cmr |= ATMEL_TC_CMR_BSWTRG(SET);
	}

	regmap_write(tcbpwmc->regmap, ATMEL_TC_CMR(tcbpwmc->channel), cmr);

	/*
	 * Use software trigger to apply the new setting.
	 * If both PWM devices in this group are disabled we stop the clock.
	 */
	if (!(cmr & (ATMEL_TC_CMR_ACPC_MSK | ATMEL_TC_CMR_BCPC_MSK)))
		regmap_write(tcbpwmc->regmap, ATMEL_TC_CCR(tcbpwmc->channel),
			     ATMEL_TC_CCR_SWTRG | ATMEL_TC_CCR_CLKDIS);
	else
		regmap_write(tcbpwmc->regmap, ATMEL_TC_CCR(tcbpwmc->channel),
			     ATMEL_TC_CCR_SWTRG);

	spin_unlock(&tcbpwmc->lock);
}

static int atmel_tcb_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct atmel_tcb_pwm_chip *tcbpwmc = to_tcb_chip(chip);
	struct atmel_tcb_pwm_device *tcbpwm = pwm_get_chip_data(pwm);
	u32 cmr;
	enum pwm_polarity polarity = tcbpwm->polarity;

	/*
	 * If duty is 0 the timer will be stopped and we have to
	 * configure the output correctly on software trigger:
	 *  - set output to high if PWM_POLARITY_INVERSED
	 *  - set output to low if PWM_POLARITY_NORMAL
	 *
	 * This is why we're reverting polarity in this case.
	 */
	if (tcbpwm->duty == 0)
		polarity = !polarity;

	spin_lock(&tcbpwmc->lock);
	regmap_read(tcbpwmc->regmap, ATMEL_TC_CMR(tcbpwmc->channel), &cmr);

	/* flush old setting and set the new one */
	cmr &= ~ATMEL_TC_CMR_TCLKS_MSK;

	if (pwm->hwpwm == 0) {
		cmr &= ~ATMEL_TC_ACMR_MASK;

		/* Set CMR flags according to given polarity */
		if (polarity == PWM_POLARITY_INVERSED)
			cmr |= ATMEL_TC_CMR_ASWTRG(CLEAR);
		else
			cmr |= ATMEL_TC_CMR_ASWTRG(SET);
	} else {
		cmr &= ~ATMEL_TC_BCMR_MASK;
		if (polarity == PWM_POLARITY_INVERSED)
			cmr |= ATMEL_TC_CMR_BSWTRG(CLEAR);
		else
			cmr |= ATMEL_TC_CMR_BSWTRG(SET);
	}

	/*
	 * If duty is 0 or equal to period there's no need to register
	 * a specific action on RA/RB and RC compare.
	 * The output will be configured on software trigger and keep
	 * this config till next config call.
	 */
	if (tcbpwm->duty != tcbpwm->period && tcbpwm->duty > 0) {
		if (pwm->hwpwm == 0) {
			if (polarity == PWM_POLARITY_INVERSED)
				cmr |= ATMEL_TC_CMR_ACPA(SET) |
				       ATMEL_TC_CMR_ACPC(CLEAR);
			else
				cmr |= ATMEL_TC_CMR_ACPA(CLEAR) |
				       ATMEL_TC_CMR_ACPC(SET);
		} else {
			if (polarity == PWM_POLARITY_INVERSED)
				cmr |= ATMEL_TC_CMR_BCPB(SET) |
				       ATMEL_TC_CMR_BCPC(CLEAR);
			else
				cmr |= ATMEL_TC_CMR_BCPB(CLEAR) |
				       ATMEL_TC_CMR_BCPC(SET);
		}
	}

	cmr |= (tcbpwm->div & ATMEL_TC_CMR_TCLKS_MSK);

	regmap_write(tcbpwmc->regmap, ATMEL_TC_CMR(tcbpwmc->channel), cmr);

	if (pwm->hwpwm == 0)
		regmap_write(tcbpwmc->regmap, ATMEL_TC_RA(tcbpwmc->channel),
			     tcbpwm->duty);
	else
		regmap_write(tcbpwmc->regmap, ATMEL_TC_RB(tcbpwmc->channel),
			     tcbpwm->duty);

	regmap_write(tcbpwmc->regmap, ATMEL_TC_RC(tcbpwmc->channel),
		     tcbpwm->period);

	/* Use software trigger to apply the new setting */
	regmap_write(tcbpwmc->regmap, ATMEL_TC_CCR(tcbpwmc->channel),
		     ATMEL_TC_CCR_SWTRG | ATMEL_TC_CCR_CLKEN);
	spin_unlock(&tcbpwmc->lock);
	return 0;
}

static int atmel_tcb_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
				int duty_ns, int period_ns)
{
	struct atmel_tcb_pwm_chip *tcbpwmc = to_tcb_chip(chip);
	struct atmel_tcb_pwm_device *tcbpwm = pwm_get_chip_data(pwm);
	struct atmel_tcb_pwm_device *atcbpwm = NULL;
	int i;
	int slowclk = 0;
	unsigned period;
	unsigned duty;
	unsigned rate = clk_get_rate(tcbpwmc->clk);
	unsigned long long min;
	unsigned long long max;

	/*
	 * Find best clk divisor:
	 * the smallest divisor which can fulfill the period_ns requirements.
	 */
	for (i = 0; i < 5; ++i) {
		if (atmel_tc_divisors[i] == 0) {
			slowclk = i;
			continue;
		}
		min = div_u64((u64)NSEC_PER_SEC * atmel_tc_divisors[i], rate);
		max = min << tcbpwmc->width;
		if (max >= period_ns)
			break;
	}

	/*
	 * If none of the divisor are small enough to represent period_ns
	 * take slow clock (32KHz).
	 */
	if (i == 5) {
		i = slowclk;
		rate = clk_get_rate(tcbpwmc->slow_clk);
		min = div_u64(NSEC_PER_SEC, rate);
		max = min << tcbpwmc->width;

		/* If period is too big return ERANGE error */
		if (max < period_ns)
			return -ERANGE;
	}

	duty = div_u64(duty_ns, min);
	period = div_u64(period_ns, min);

	if (pwm->hwpwm == 0)
		atcbpwm = tcbpwmc->pwms[1];
	else
		atcbpwm = tcbpwmc->pwms[0];

	/*
	 * PWM devices provided by the TCB driver are grouped by 2.
	 * PWM devices in a given group must be configured with the
	 * same period_ns.
	 *
	 * We're checking the period value of the second PWM device
	 * in this group before applying the new config.
	 */
	if ((atcbpwm && atcbpwm->duty > 0 &&
			atcbpwm->duty != atcbpwm->period) &&
		(atcbpwm->div != i || atcbpwm->period != period)) {
		dev_err(chip->dev,
			"failed to configure period_ns: PWM group already configured with a different value\n");
		return -EINVAL;
	}

	tcbpwm->period = period;
	tcbpwm->div = i;
	tcbpwm->duty = duty;

	/* If the PWM is enabled, call enable to apply the new conf */
	if (pwm_is_enabled(pwm))
		atmel_tcb_pwm_enable(chip, pwm);

	return 0;
}

static const struct pwm_ops atmel_tcb_pwm_ops = {
	.request = atmel_tcb_pwm_request,
	.free = atmel_tcb_pwm_free,
	.config = atmel_tcb_pwm_config,
	.set_polarity = atmel_tcb_pwm_set_polarity,
	.enable = atmel_tcb_pwm_enable,
	.disable = atmel_tcb_pwm_disable,
	.owner = THIS_MODULE,
};

static int atmel_tcb_pwm_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	const struct atmel_tcb_info *tcb_info;
	struct atmel_tcb_pwm_chip *tcbpwm;
	struct device_node *np = pdev->dev.of_node;
	struct regmap *regmap;
	struct clk *clk;
	struct clk *slow_clk;
	int err;
	int channel;

	err = of_property_read_u32(np, "reg", &channel);
	if (err < 0) {
		dev_err(&pdev->dev,
			"failed to get Timer Counter Block channel from device tree (error: %d)\n",
			err);
		return err;
	}

	regmap = syscon_node_to_regmap(np->parent);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	slow_clk = of_clk_get_by_name(np->parent, "slow_clk");
	if (IS_ERR(slow_clk))
		return PTR_ERR(slow_clk);

	clk = tcb_clk_get(np, channel);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	tcbpwm = devm_kzalloc(&pdev->dev, sizeof(*tcbpwm), GFP_KERNEL);
	if (tcbpwm == NULL) {
		err = -ENOMEM;
		dev_err(&pdev->dev, "failed to allocate memory\n");
		goto err_slow_clk;
	}

	tcbpwm->chip.dev = &pdev->dev;
	tcbpwm->chip.ops = &atmel_tcb_pwm_ops;
	tcbpwm->chip.of_xlate = of_pwm_xlate_with_flags;
	tcbpwm->chip.of_pwm_n_cells = 3;
	tcbpwm->chip.base = -1;
	tcbpwm->chip.npwm = NPWM;
	tcbpwm->channel = channel;
	tcbpwm->regmap = regmap;
	tcbpwm->clk = clk;
	tcbpwm->slow_clk = slow_clk;

	match = of_match_node(atmel_tcb_dt_ids, np->parent);
	tcb_info = match->data;
	tcbpwm->width = tcb_info->bits;

	err = clk_prepare_enable(slow_clk);
	if (err)
		goto err_slow_clk;

	spin_lock_init(&tcbpwm->lock);

	err = pwmchip_add(&tcbpwm->chip);
	if (err < 0)
		goto err_disable_clk;

	platform_set_drvdata(pdev, tcbpwm);

	return 0;

err_disable_clk:
	clk_disable_unprepare(tcbpwm->slow_clk);

err_slow_clk:
	clk_put(slow_clk);

	return err;
}

static int atmel_tcb_pwm_remove(struct platform_device *pdev)
{
	struct atmel_tcb_pwm_chip *tcbpwm = platform_get_drvdata(pdev);
	int err;

	clk_disable_unprepare(tcbpwm->slow_clk);
	clk_put(tcbpwm->slow_clk);
	clk_put(tcbpwm->clk);

	err = pwmchip_remove(&tcbpwm->chip);
	if (err < 0)
		return err;

	return 0;
}

static const struct of_device_id atmel_tcb_pwm_dt_ids[] = {
	{ .compatible = "atmel,tcb-pwm", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, atmel_tcb_pwm_dt_ids);

static struct platform_driver atmel_tcb_pwm_driver = {
	.driver = {
		.name = "atmel-tcb-pwm",
		.of_match_table = atmel_tcb_pwm_dt_ids,
	},
	.probe = atmel_tcb_pwm_probe,
	.remove = atmel_tcb_pwm_remove,
};
module_platform_driver(atmel_tcb_pwm_driver);

MODULE_AUTHOR("Boris BREZILLON <b.brezillon@overkiz.com>");
MODULE_DESCRIPTION("Atmel Timer Counter Pulse Width Modulation Driver");
MODULE_LICENSE("GPL v2");
