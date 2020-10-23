/*
 * Copyright 2018 Google, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define BMS_DEV_NAME	"sm8150_bms"
#define pr_fmt(fmt) BMS_DEV_NAME": " fmt

#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_batterydata.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/bitops.h>
#include "google_bms.h"
/* hackaroo... */
#include <linux/qpnp/qpnp-revid.h>
#include "../qcom/smb5-reg.h"
#include "../qcom/smb5-lib.h"

#define BIAS_STS_READY	BIT(0)

struct bms_dev {
	struct	device			*dev;
	struct	power_supply		*psy;
	struct	regmap			*pmic_regmap;
	struct	notifier_block		nb;
	int				batt_id_ohms;
	int				taper_control;
	bool				fcc_stepper_enable;
	u32				rradc_base;
	int				chg_term_voltage;

};

struct bias_config {
	u16	status_reg;
	u16	lsb_reg;
	int	bias_kohms;
};

#define CHGR_BATTERY_CHARGER_STATUS_1_REG	0x1006

#define CHGR_BATTERY_CHARGER_STATUS_2_REG	0x1007
#define CHG_ERR_STATUS_SFT_EXPIRE		BIT(2)

#define CHGR_BATTERY_CHARGER_STATUS_5_REG	0x100B
#define ENABLE_TRICKLE_BIT			BIT(2)
#define ENABLE_PRE_CHARGING_BIT			BIT(1)
#define ENABLE_FULLON_MODE_BIT			BIT(0)

#define CHGR_FLOAT_VOLTAGE_NOW			0x1009
#define CHGR_CHARGING_ENABLE_CMD		0x1042
#define CHARGING_ENABLE_CMD_BIT			BIT(0)

#define CHGR_CHARGING_PAUSE_CMD			0x1043
#define CHARGING_PAUSE_CMD_BIT			BIT(0)

#define CHGR_FAST_CHARGE_CURRENT_SETTING	0x1061
#define CHGR_FLOAT_VOLTAGE_SETTING		0x1070

#define DCDC_ICL_STATUS_REG			0x1107
#define DCDC_AICL_ICL_STATUS_REG		0x1108
#define DCDC_AICL_STATUS_REG			0x110A
#define DCDC_SOFT_ILIMIT_BIT			BIT(6)

#define DCDC_POWER_PATH_STATUS_REG		0x110B
#define USE_USBIN_BIT				BIT(4)
#define USE_DCIN_BIT				BIT(3)
#define VALID_INPUT_POWER_SOURCE_STS_BIT	BIT(0)

#define CHG_P_DCIN_CMD_IL_REG			0x1440
#define CHG_P_DCIN_EN_OVERRIDE_BIT		BIT(1)

#define CHG_P_DCIN_INT_RT_STS			0x1410
#define CHG_P_DCIN_PLUGIN_BIT			BIT(4)
#define CHG_P_DCIN_EN_BIT			BIT(7)


#define CHGR_BATTERY_CHARGER_STATUS_MASK	GENMASK(2, 0)

#define CHGR_FLOAT_VOLTAGE_BASE		3600000
#define CHGR_CHARGE_CURRENT_STEP	50000

#define CHG_TERM_VOLTAGE	4350

enum sm8150_chg_status {
	SM8150_INHIBIT_CHARGE	= 0,
	SM8150_TRICKLE_CHARGE	= 1,
	SM8150_PRE_CHARGE	= 2,
	SM8150_FULLON_CHARGE	= 3,
	SM8150_TAPER_CHARGE	= 4,
	SM8150_TERMINATE_CHARGE	= 5,
	SM8150_PAUSE_CHARGE	= 6,
	SM8150_DISABLE_CHARGE	= 7,
};

static int sm8150_read(struct regmap *pmic_regmap, int addr, u8 *val, int len)
{
	int rc;

	if (!pmic_regmap)
		return -ENXIO;

	rc = regmap_bulk_read(pmic_regmap, addr, val, len);
	if (rc < 0) {
		pr_err("regmap_read failed for address %04x rc=%d\n",
			addr, rc);
		return rc;
	}

	return 0;
}

static int sm8150_write(struct regmap *pmic_regmap, int addr, u8 *val, int len)
{
	int rc;

	if (!pmic_regmap)
		return -ENXIO;

	rc = regmap_bulk_write(pmic_regmap, addr, val, len);
	if (rc < 0) {
		pr_err("regmap_write failed for address %04x rc=%d\n",
			addr, rc);
		return rc;
	}

	return 0;
}

static int sm8150_masked_write(struct regmap *pmic_regmap,
			       u16 addr, u8 mask, u8 val)
{
	return regmap_update_bits(pmic_regmap, addr, mask, val);
}

static int sm8150_rd8(struct regmap *pmic_regmap, int addr, u8 *val)
{
	return sm8150_read(pmic_regmap, addr, val, 1);
}

/* ------------------------------------------------------------------------- */

static irqreturn_t sm8150_chg_state_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct bms_dev *chg = irq_data->parent_data;
	u8 stat;
	int rc;

	dev_dbg(chg->dev, "IRQ: %s\n", irq_data->name);

	rc = sm8150_read(chg->pmic_regmap, CHGR_BATTERY_CHARGER_STATUS_1_REG,
				&stat, 1);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
				rc);
		return IRQ_HANDLED;
	}

	stat = stat & BATTERY_CHARGER_STATUS_MASK;
	power_supply_changed(chg->psy);
	return IRQ_HANDLED;
}

static irqreturn_t sm8150_batt_temp_changed_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct bms_dev *chg = irq_data->parent_data;

	/* TODO: handle software jeita ? */
	dev_dbg(chg->dev, "IRQ: %s\n", irq_data->name);
	power_supply_changed(chg->psy);
	return IRQ_HANDLED;
}

static irqreturn_t sm8150_batt_psy_changed_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct bms_dev *chg = irq_data->parent_data;

	dev_dbg(chg->dev, "IRQ: %s\n", irq_data->name);
	power_supply_changed(chg->psy);
	return IRQ_HANDLED;
}

static irqreturn_t sm8150_default_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	dev_dbg(chg->dev, "IRQ: %s\n", irq_data->name);
	return IRQ_HANDLED;
}

/* TODO: sparse, consider adding .irqno */
static struct smb_irq_info sm8150_bms_irqs[] = {
	/* CHARGER IRQs */
	[CHGR_ERROR_IRQ] = {
		.name		= "chgr-error",
		.handler	= sm8150_default_irq_handler,
	},
	[CHG_STATE_CHANGE_IRQ] = {
		.name		= "chg-state-change",
		.handler	= sm8150_chg_state_change_irq_handler,
		.wake		= true,
	},
	[STEP_CHG_STATE_CHANGE_IRQ] = {
		.name		= "step-chg-state-change",
	},
	[STEP_CHG_SOC_UPDATE_FAIL_IRQ] = {
		.name		= "step-chg-soc-update-fail",
	},
	[STEP_CHG_SOC_UPDATE_REQ_IRQ] = {
		.name		= "step-chg-soc-update-req",
	},
	[FG_FVCAL_QUALIFIED_IRQ] = {
		.name		= "fg-fvcal-qualified",
	},
	[VPH_ALARM_IRQ] = {
		.name		= "vph-alarm",
	},
	[VPH_DROP_PRECHG_IRQ] = {
		.name		= "vph-drop-prechg",
	},
	/* BATTERY IRQs */
	[BAT_TEMP_IRQ] = {
		.name		= "bat-temp",
		.handler	= sm8150_batt_temp_changed_irq_handler,
		.wake		= true,
	},
	[ALL_CHNL_CONV_DONE_IRQ] = {
		.name		= "all-chnl-conv-done",
	},
	[BAT_OV_IRQ] = {
		.name		= "bat-ov",
		.handler	= sm8150_batt_psy_changed_irq_handler,
	},
	[BAT_LOW_IRQ] = {
		.name		= "bat-low",
		.handler	= sm8150_batt_psy_changed_irq_handler,
	},
	[BAT_THERM_OR_ID_MISSING_IRQ] = {
		.name		= "bat-therm-or-id-missing",
		.handler	= sm8150_batt_psy_changed_irq_handler,
	},
	[BAT_TERMINAL_MISSING_IRQ] = {
		.name		= "bat-terminal-missing",
		.handler	= sm8150_batt_psy_changed_irq_handler,
	},
	[BUCK_OC_IRQ] = {
		.name		= "buck-oc",
	},
	[VPH_OV_IRQ] = {
		.name		= "vph-ov",
	},
};

static int sm8150_get_irq_index_byname(const char *irq_name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sm8150_bms_irqs); i++) {
		if (!sm8150_bms_irqs[i].name)
			continue;

		if (strcmp(sm8150_bms_irqs[i].name, irq_name) == 0)
			return i;
	}

	return -ENOENT;
}

static int sm8150_request_interrupt(struct bms_dev *bms,
				    struct device_node *node,
				    const char *irq_name)
{
	int rc, irq, irq_index;
	struct smb_irq_data *irq_data;

	irq = of_irq_get_byname(node, irq_name);
	if (irq < 0) {
		pr_err("Couldn't get irq %s byname\n", irq_name);
		return irq;
	}

	irq_index = sm8150_get_irq_index_byname(irq_name);
	if (irq_index < 0) {
		pr_err("%s is not a defined irq\n", irq_name);
		return irq_index;
	}

	if (!sm8150_bms_irqs[irq_index].handler)
		return 0;

	irq_data = devm_kzalloc(bms->dev, sizeof(*irq_data), GFP_KERNEL);
	if (!irq_data)
		return -ENOMEM;

	irq_data->parent_data = bms;
	irq_data->name = irq_name;
	irq_data->storm_data = sm8150_bms_irqs[irq_index].storm_data;
	mutex_init(&irq_data->storm_data.storm_lock);

	rc = devm_request_threaded_irq(bms->dev, irq, NULL,
					sm8150_bms_irqs[irq_index].handler,
					IRQF_ONESHOT, irq_name, irq_data);
	if (rc < 0) {
		pr_err("Couldn't request irq %d\n", irq);
		return rc;
	}

	sm8150_bms_irqs[irq_index].irq = irq;
	sm8150_bms_irqs[irq_index].irq_data = irq_data;
	if (sm8150_bms_irqs[irq_index].wake)
		enable_irq_wake(irq);

	return rc;
}

static void sm8150_free_interrupts(struct bms_dev *bms)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sm8150_bms_irqs); i++) {
		if (sm8150_bms_irqs[i].irq > 0) {
			if (sm8150_bms_irqs[i].wake)
				disable_irq_wake(sm8150_bms_irqs[i].irq);

			devm_free_irq(bms->dev, sm8150_bms_irqs[i].irq,
						sm8150_bms_irqs[i].irq_data);
		}
	}
}

static void sm8150_disable_interrupts(struct bms_dev *bms)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sm8150_bms_irqs); i++) {
		if (sm8150_bms_irqs[i].irq > 0)
			disable_irq(sm8150_bms_irqs[i].irq);
	}
}

static int sm8150_request_interrupts(struct bms_dev *bms)
{
	struct device_node *node = bms->dev->of_node;
	struct device_node *child;
	int rc = 0;
	const char *name;
	struct property *prop;

	for_each_available_child_of_node(node, child) {
		of_property_for_each_string(child, "interrupt-names",
					    prop, name) {
			rc = sm8150_request_interrupt(bms, child, name);
			if (rc < 0)
				return rc;
		}
	}
	return 0;
}

static struct bias_config batt_id_table[3] = {
	{0x4265, 0x4266, 400},
	{0x426D, 0x426E, 100},
	{0x4275, 0x4276, 30},
};

#define MAX_BIAS_CODE	0x70E4
static void sm8150_get_batt_id(const struct bms_dev *bms, int *batt_id_ohms)
{
	int i, rc, batt_id_kohms;
	u16 tmp = 0, bias_code = 0, delta = 0;
	u8 val, bias_id = 0;

	for (i = 0; i < ARRAY_SIZE(batt_id_table); i++)  {
		rc = sm8150_read(bms->pmic_regmap, batt_id_table[i].status_reg,
				&val, 1);
		if (rc < 0) {
			pr_err("Failed to read bias_sts, rc=%d\n", rc);
			return;
		}

		if (val & BIAS_STS_READY) {
			rc = sm8150_read(bms->pmic_regmap,
					batt_id_table[i].lsb_reg,
					(u8 *)&tmp, 2);
			if (rc < 0) {
				pr_err("Failed to read bias_lsb_reg, rc=%d\n",
					rc);
				return;
			}
		}

		pr_info("bias_code[%d]: 0x%04x\n", i, tmp);

		/*
		 * Bias code closer to MAX_BIAS_CODE/2 is the one which should
		 * be used for calculating battery id.
		 */
		if (!delta || abs(tmp - MAX_BIAS_CODE / 2) < delta) {
			bias_id = i;
			bias_code = tmp;
			delta = abs(tmp - MAX_BIAS_CODE / 2);
		}
	}

	pr_info("bias_id: %d bias_code: 0x%04x\n", bias_id, bias_code);

	/*
	 * Following equation is used for calculating battery id.
	 * batt_id(KOhms) = bias_id(KOhms) / ((MAX_BIAS_CODE / bias_code) - 1)
	 */
	batt_id_kohms = (batt_id_table[bias_id].bias_kohms * bias_code) * 10 /
			(MAX_BIAS_CODE - bias_code);
	*batt_id_ohms = (batt_id_kohms * 1000) / 10;
	pr_info("batt_id = %d\n", *batt_id_ohms);
}

#define MAX_READ_TRIES		 5
#define BATT_INFO_IBATT_LSB	0x41A2
#define BATT_INFO_IBATT_LSB_CP	0x41A8
#define BATT_CURRENT_NUMR	488281
#define BATT_CURRENT_DENR	1000
static int sm8150_get_battery_current(const struct bms_dev *bms, int *val)
{
	int rc = 0, tries = 0;
	int64_t temp = 0;
	u8 buf[2], buf_cp[2];

	while (tries++ < MAX_READ_TRIES) {
		rc = sm8150_read(bms->pmic_regmap, BATT_INFO_IBATT_LSB, buf, 2);
		if (rc < 0) {
			return rc;
		}

		rc = sm8150_read(bms->pmic_regmap, BATT_INFO_IBATT_LSB_CP,
				buf_cp, 2);
		if (rc < 0) {
			return rc;
		}

		if (buf[0] == buf_cp[0] && buf[1] == buf_cp[1])
			break;
	}

	if (tries == MAX_READ_TRIES) {
		pr_err("IBATT: shadow registers do not match\n");
		return -EINVAL;
	}

	temp = buf[1] << 8 | buf[0];
	/* Sign bit is bit 15 */
	temp = sign_extend32(temp, 15);
	*val = div_s64((s64)temp * BATT_CURRENT_NUMR, BATT_CURRENT_DENR);
	return 0;
}

#define BATT_INFO_VBATT_LSB	0x41A0
#define BATT_INFO_VBATT_LSB_CP	0x41A6
#define BATT_VOLTAGE_NUMR	122070
#define BATT_VOLTAGE_DENR	1000
static int sm8150_get_battery_voltage(const struct bms_dev *bms, int *val)
{
	int rc = 0, tries = 0;
	u16 temp = 0;
	u8 buf[2], buf_cp[2];

	while (tries++ < MAX_READ_TRIES) {
		rc = sm8150_read(bms->pmic_regmap, BATT_INFO_VBATT_LSB, buf, 2);
		if (rc < 0) {
			return rc;
		}

		rc = sm8150_read(bms->pmic_regmap, BATT_INFO_VBATT_LSB_CP,
				buf_cp, 2);
		if (rc < 0) {
			return rc;
		}

		if (buf[0] == buf_cp[0] && buf[1] == buf_cp[1])
			break;
	}

	if (tries == MAX_READ_TRIES) {
		pr_err("VBATT: shadow registers do not match\n");
		return -EINVAL;
	}

	temp = buf[1] << 8 | buf[0];
	*val = div_u64((u64)temp * BATT_VOLTAGE_NUMR, BATT_VOLTAGE_DENR);
	return 0;
}


#define ADC_RR_BATT_TEMP_LSB(chip)		(chip->rradc_base + 0x88)
#define ADC_RR_BATT_TEMP_MSB(chip)		(chip->rradc_base + 0x89)
#define GEN4_BATT_TEMP_MSB_MASK			GENMASK(1, 0)

static int sm8150_get_battery_temp(const struct bms_dev *bms, int *val)
{
	int rc = 0;
	u8 buf;

	if (!bms->rradc_base)
		return -EIO;

	rc = sm8150_read(bms->pmic_regmap, ADC_RR_BATT_TEMP_LSB(bms), &buf, 1);
	if (rc < 0) {
		pr_err("failed to read addr=0x%04x, rc=%d\n",
			ADC_RR_BATT_TEMP_LSB(bms), rc);
		return rc;
	}

	/* Only 8 bits are used. Bit 7 is sign bit */
	*val = sign_extend32(buf, 7);

	/* Value is in Celsius; Convert it to deciDegC */
	*val *= 10;

	return 0;
}


#define SM8150_IS_ONLINE(stat)	\
	(((stat) & (USE_DCIN_BIT | USE_USBIN_BIT)) && \
	((stat) & VALID_INPUT_POWER_SOURCE_STS_BIT))

/* charger online when connected */
static bool sm8150_is_online(const struct bms_dev *bms)
{
	u8 stat;
	const int rc = sm8150_read(bms->pmic_regmap,
				   DCDC_POWER_PATH_STATUS_REG,
				   &stat, 1);

	return (rc == 0) && SM8150_IS_ONLINE(stat);
}

static int sm8150_is_limited(const struct bms_dev *bms)
{
	int rc;
	u8 val;

	rc = sm8150_read(bms->pmic_regmap, DCDC_AICL_STATUS_REG, &val, 1);
	return (rc < 0) ? -EIO : ((val & DCDC_SOFT_ILIMIT_BIT) != 0);
}

int sm8150_rerun_aicl(const struct bms_dev *bms)
{
	int rc;
	u8 stat;

	rc = sm8150_read(bms->pmic_regmap, POWER_PATH_STATUS_REG, &stat, 1);
	if (rc < 0) {
		pr_err("Couldn't read POWER_PATH_STATUS rc=%d\n", rc);
		return rc;
	}

	pr_info("Re-running AICL (susp=%d)\n",
		(stat & USBIN_SUSPEND_STS_BIT) !=0 );

	/* USB is suspended so skip re-running AICL */
	if (stat & USBIN_SUSPEND_STS_BIT)
		return -EINVAL;

	rc = sm8150_masked_write(bms->pmic_regmap, AICL_CMD_REG,
					RERUN_AICL_BIT, RERUN_AICL_BIT);
	if (rc < 0)
		pr_err("Couldn't write to AICL_CMD_REG rc=%d\n", rc);

	return 0;
}

static int sm8150_get_chg_type(const struct bms_dev *bms)
{
	u8 val;
	int chg_type, rc;

	rc = sm8150_read(bms->pmic_regmap, CHGR_BATTERY_CHARGER_STATUS_1_REG,
				&val, 1);
	if (rc < 0)
		return POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;

	switch (val & CHGR_BATTERY_CHARGER_STATUS_MASK) {
	case SM8150_TRICKLE_CHARGE:
	case SM8150_PRE_CHARGE:
		chg_type = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	case SM8150_FULLON_CHARGE:
		chg_type = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	case SM8150_TAPER_CHARGE:
		chg_type = POWER_SUPPLY_CHARGE_TYPE_TAPER;
		break;
	default:
		chg_type = POWER_SUPPLY_CHARGE_TYPE_NONE;
		break;
	}

	return chg_type;
}

static int sm8150_get_chg_status(const struct bms_dev *bms,
				 bool *dc_valid, bool *usb_valid)
{
	bool plugged, valid;
	int rc, ret;
	int vchrg = 0;
	u8 pstat, stat1, stat2;

	rc = sm8150_rd8(bms->pmic_regmap, DCDC_POWER_PATH_STATUS_REG, &pstat);
	if (rc < 0)
		return POWER_SUPPLY_STATUS_UNKNOWN;

	valid = (pstat & VALID_INPUT_POWER_SOURCE_STS_BIT);
	plugged = (pstat & USE_DCIN_BIT) || (pstat & USE_USBIN_BIT);

	*dc_valid = valid && (pstat & USE_DCIN_BIT);
	*usb_valid = valid && (pstat & USE_USBIN_BIT);

	rc = sm8150_rd8(bms->pmic_regmap, CHGR_BATTERY_CHARGER_STATUS_1_REG,
			&stat1);
	if (rc < 0)
		return POWER_SUPPLY_STATUS_UNKNOWN;

	rc = sm8150_rd8(bms->pmic_regmap, CHGR_BATTERY_CHARGER_STATUS_2_REG,
			&stat2);
	if (rc < 0)
		return POWER_SUPPLY_STATUS_UNKNOWN;

	pr_debug("pmic: pstat=%x stat1=%x stat2=%x\n",
		pstat, stat1, stat2);

	stat1 = stat1 & CHGR_BATTERY_CHARGER_STATUS_MASK;

	if (!plugged)
		return POWER_SUPPLY_STATUS_DISCHARGING;

	switch (stat1) {
	case SM8150_TRICKLE_CHARGE:
	case SM8150_PRE_CHARGE:
	case SM8150_FULLON_CHARGE:
	case SM8150_TAPER_CHARGE:
		ret = POWER_SUPPLY_STATUS_CHARGING;
		break;
	/* pause on FCC=0, JEITA, USB/DC suspend or on INPUT UV/OV */
	case SM8150_PAUSE_CHARGE:
	case SM8150_INHIBIT_CHARGE:
	case SM8150_TERMINATE_CHARGE:
		/* flag full only at the correct voltage */
		rc = sm8150_get_battery_voltage(bms, &vchrg);
		if (rc == 0)
			vchrg = (vchrg / 1000);
		if (vchrg < bms->chg_term_voltage)
			ret = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else
			ret = POWER_SUPPLY_STATUS_FULL;
		break;
	/* disabled disconnect */
	case SM8150_DISABLE_CHARGE:
		ret = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	default:
		ret = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	if (ret != POWER_SUPPLY_STATUS_CHARGING)
		return ret;

	if (valid) {
		u8 stat;

		rc = sm8150_rd8(bms->pmic_regmap,
				CHGR_BATTERY_CHARGER_STATUS_5_REG,
				&stat);
		if (rc < 0)
			return POWER_SUPPLY_STATUS_UNKNOWN;

		stat &= ENABLE_TRICKLE_BIT | ENABLE_PRE_CHARGING_BIT |
					ENABLE_FULLON_MODE_BIT;
		if (stat)
			return POWER_SUPPLY_STATUS_CHARGING;
	}

	return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static int sm8150_get_chg_chgr_state(const struct bms_dev *bms,
			  union gbms_charger_state *chg_state)
{
	int vchrg, rc;
	bool usb_valid, dc_valid;
	u8 icl = 0;
	u8 reg = 0, val;

	chg_state->v = 0;
	chg_state->f.chg_status = sm8150_get_chg_status(bms, &dc_valid,
							&usb_valid);
	chg_state->f.chg_type = sm8150_get_chg_type(bms);
	chg_state->f.flags = gbms_gen_chg_flags(chg_state->f.chg_status,
						chg_state->f.chg_type);

	rc = sm8150_is_limited(bms);
	if (rc > 0)
		chg_state->f.flags |= GBMS_CS_FLAG_ILIM;

	rc = sm8150_get_battery_voltage(bms, &vchrg);
	if (rc == 0)
		chg_state->f.vchrg = (vchrg / 1000);

	if (usb_valid) {
		(void)sm8150_rd8(bms->pmic_regmap, DCDC_ICL_STATUS_REG,
				 &icl);
	} else if (dc_valid) {
		(void)sm8150_rd8(bms->pmic_regmap, DCDC_CFG_REF_MAX_PSNS_REG,
				 &icl);
	}
	chg_state->f.icl = (icl * 50);

	pr_info("MSC_PCS chg_state=%lx [0x%x:%d:%d:%d:%d] chg=%c\n",
		(unsigned long)chg_state->v,
		chg_state->f.flags,
		chg_state->f.chg_type,
		chg_state->f.chg_status,
		chg_state->f.vchrg,
		chg_state->f.icl,
		usb_valid ? 'u' : dc_valid ? 'w' : ' ');

	rc = sm8150_rd8(bms->pmic_regmap, CHG_P_DCIN_INT_RT_STS, &reg);

	if ((!rc) && (reg & CHG_P_DCIN_PLUGIN_BIT) &&
	    (!(reg & CHG_P_DCIN_EN_BIT))) {
		val = CHG_P_DCIN_EN_OVERRIDE_BIT;
		pr_info("MSC_PCS: reset DCIN enable pin\n");
		sm8150_write(bms->pmic_regmap, CHG_P_DCIN_CMD_IL_REG, &val, 1);
	}

	return 0;
}

static int sm8150_psy_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *pval)
{
	struct bms_dev *bms = (struct bms_dev *)power_supply_get_drvdata(psy);
	union gbms_charger_state chg_state;
	u8 val;
	int ivalue = 0;
	int rc = 0;

	if (!bms->psy) {
		pr_err("failed to register power supply\n");
		return -ENODATA;
	}

	switch (psp) {
	/* called from power_supply_update_leds(), not using it on this
	 * platform. Could return the state of the charge buck (BUCKEN)
	 */
	case POWER_SUPPLY_PROP_ONLINE:
		pval->intval = sm8150_is_online(bms);
		break;
	case POWER_SUPPLY_PROP_RESISTANCE_ID:
		pval->intval = bms->batt_id_ohms;
		break;
	/* pixel battery management subsystem */
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		/*CHGR_FAST_CHARGE_CURRENT_SETTING, 0x1061
		 * 7 : 0 => FAST_CHARGE_CURRENT_SETTING:
		 * Fast Charge Current = DATA x 50mA
		 */
		rc = sm8150_read(bms->pmic_regmap,
				CHGR_FAST_CHARGE_CURRENT_SETTING, &val, 1);
		if (!rc)
			pval->intval = val * CHGR_CHARGE_CURRENT_STEP;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		/*CHGR_FLOAT_VOLTAGE_SETTING  0x1070
		 * 7 : 0 => FLOAT_VOLTAGE_SETTING:
		 * Float voltage setting = 3.6V + (DATA x 10mV)
		 */
		rc = sm8150_read(bms->pmic_regmap, CHGR_FLOAT_VOLTAGE_SETTING,
				&val, 1);
		if (!rc)
			pval->intval = val * 10000 + CHGR_FLOAT_VOLTAGE_BASE;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CHARGER_STATE:
		rc = sm8150_get_chg_chgr_state(bms, &chg_state);
		if (!rc)
			pval->int64val = chg_state.v;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		pval->intval = sm8150_get_chg_type(bms);
		break;
	case POWER_SUPPLY_PROP_CHARGE_DONE:
		rc = sm8150_read(bms->pmic_regmap,
				CHGR_BATTERY_CHARGER_STATUS_1_REG, &val, 1);
		if (!rc) {
			val = val & CHGR_BATTERY_CHARGER_STATUS_MASK;
			pval->intval = (val == SM8150_TERMINATE_CHARGE);
		}
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		rc = sm8150_get_battery_current(bms, &ivalue);
		if (!rc)
			pval->intval = ivalue;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMITED:
		rc = sm8150_is_limited(bms);
		if (rc < 0)
			break;
		pval->intval = (rc > 0);
		break;

	case POWER_SUPPLY_PROP_TEMP:
		rc = sm8150_get_battery_temp(bms, &ivalue);
		if (rc < 0)
			break;
		pval->intval = ivalue;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		/*CHGR_FLOAT_VOLTAGE_NOW 0x1009
		 * 7 : 0 => FLOAT_VOLTAGE:
		 * Float voltage after JEITA compensation
		 */
		rc = sm8150_read(bms->pmic_regmap, CHGR_FLOAT_VOLTAGE_NOW,
				&val, 1);
		if (!rc)
			pval->intval = val * 10000 + CHGR_FLOAT_VOLTAGE_BASE;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		rc = sm8150_get_battery_voltage(bms, &ivalue);
		if (!rc)
			pval->intval = ivalue;
		break;
	case POWER_SUPPLY_PROP_SAFETY_TIMER_EXPIRED:
		rc = sm8150_read(bms->pmic_regmap,
				CHGR_BATTERY_CHARGER_STATUS_2_REG, &val, 1);
		if (!rc)
			pval->intval = (val & CHG_ERR_STATUS_SFT_EXPIRE) ?
					1 : 0;
		break;
	case POWER_SUPPLY_PROP_FCC_STEPPER_ENABLE:
		pval->intval = bms->fcc_stepper_enable;
		break;
	case POWER_SUPPLY_PROP_TAPER_CONTROL:
		pval->intval = bms->taper_control;
		break;
	case POWER_SUPPLY_PROP_CHARGE_DISABLE:
		rc = sm8150_read(bms->pmic_regmap, CHGR_CHARGING_ENABLE_CMD,
				&val, 1);
		if (!rc)
			pval->intval = (val & CHARGING_ENABLE_CMD_BIT) ? 0 : 1;
		break;
	case POWER_SUPPLY_PROP_RERUN_AICL:
		pval->intval = 0;
		break;
	default:
		pr_err("getting unsupported property: %d\n", psp);
		return -EINVAL;
	}

	if (rc < 0)
		return -ENODATA;

	return 0;
}

static int sm8150_charge_disable(struct bms_dev *bms, bool disable)
{
	const u8 val = disable ? 0 : CHARGING_ENABLE_CMD_BIT;
	int rc;

	rc = sm8150_masked_write(bms->pmic_regmap,
				 CHGR_CHARGING_ENABLE_CMD,
				 CHARGING_ENABLE_CMD_BIT, val);

	pr_info("CHARGE_DISABLE : disable=%d -> val=%d (%d)\n",
		disable, val, rc);

	return rc;
}

static int sm8150_charge_pause(struct bms_dev *bms, bool pause)
{
	const u8 val = pause ? CHARGING_PAUSE_CMD_BIT : 0;
	int rc;

	rc = sm8150_masked_write(bms->pmic_regmap,
				 CHGR_CHARGING_PAUSE_CMD,
				 CHARGING_PAUSE_CMD_BIT, val);

	pr_info("CHARGE_PAUSE : pause=%d -> val=%d (%d)\n",
		pause, val, rc);

	return rc;
}

static int sm8150_psy_set_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  const union power_supply_propval *pval)
{
	struct bms_dev *bms = (struct bms_dev *)power_supply_get_drvdata(psy);
	u8 val;
	int ivalue = 0;
	int rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		/*CHGR_FAST_CHARGE_CURRENT_SETTING, 0x1061
		 * 7 : 0 => FAST_CHARGE_CURRENT_SETTING:
		 * Fast Charge Current = DATA x 50mA
		 */
		ivalue = pval->intval;
		if (ivalue < CHGR_CHARGE_CURRENT_STEP) {
			val = 0;
		} else {
			val = ivalue / CHGR_CHARGE_CURRENT_STEP;
		}

		if (ivalue == 0)
			rc = sm8150_charge_pause(bms, true);

		rc = sm8150_write(bms->pmic_regmap,
					CHGR_FAST_CHARGE_CURRENT_SETTING,
					&val, 1);

		/* NOTE FCC==0 will cause the device to not draw any current
		 * from USB (QC#04128172). Need to take care of this detail
		 * in the platform driver to keep the charger code sane.
		 */
		if (ivalue != 0) {
			u8 paused;

			rc = sm8150_read(bms->pmic_regmap,
					 CHGR_CHARGING_PAUSE_CMD,
					 &paused, 1);
			if (rc == 0 && (paused & CHARGING_PAUSE_CMD_BIT)) {
				rc = sm8150_charge_pause(bms, false);

				/* make sure charging restart */
				if (rc == 0)
					rc = sm8150_charge_disable(bms, true);
				if (rc == 0)
					rc = sm8150_charge_disable(bms, false);
			}
		}

		pr_info("CONSTANT_CHARGE_CURRENT_MAX : ivalue=%d, val=%d pause=%d (%d)\n",
			ivalue, val, ivalue == 0, rc);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		/*CHGR_FLOAT_VOLTAGE_SETTING  0x1070
		 * 7 : 0 => FLOAT_VOLTAGE_SETTING:
		 * Float voltage setting = 3.6V + (DATA x 10mV)
		 */
		ivalue = pval->intval;
		if (ivalue < CHGR_FLOAT_VOLTAGE_BASE) {
			val = 0;
		} else {
			val = (ivalue - CHGR_FLOAT_VOLTAGE_BASE) / 10000;
		}

		rc = sm8150_write(bms->pmic_regmap,
					CHGR_FLOAT_VOLTAGE_SETTING,
					&val, 1);
		pr_info("CONSTANT_CHARGE_VOLTAGE_MAX : ivalue=%d, val=%d (%d)\n",
							ivalue, val, rc);
		break;
	case POWER_SUPPLY_PROP_TAPER_CONTROL:
		bms->taper_control = pval->intval;
		break;
	case POWER_SUPPLY_PROP_CHARGE_DISABLE:
		rc = sm8150_charge_disable(bms, pval->intval != 0);
		break;
	case POWER_SUPPLY_PROP_RERUN_AICL:
		(void)sm8150_rerun_aicl(bms);
		break;
	default:
		pr_err("setting unsupported property: %d\n", psp);
		break;
	}

	return rc;
}

static int sm8150_property_is_writeable(struct power_supply *psy,
					enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_TAPER_CONTROL:
	case POWER_SUPPLY_PROP_CHARGE_DISABLE:
	case POWER_SUPPLY_PROP_RERUN_AICL:
		return 1;
	default:
		break;
	}

	return 0;
}

static enum power_supply_property sm8150_psy_props[] = {
	POWER_SUPPLY_PROP_RESISTANCE_ID,
	/* pixel battery management subsystem */
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CHARGE_CHARGER_STATE,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CHARGE_DONE,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMITED,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,		/* compat */
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_SAFETY_TIMER_EXPIRED,
	POWER_SUPPLY_PROP_FCC_STEPPER_ENABLE,	/* compat */
	POWER_SUPPLY_PROP_CHARGE_DISABLE,
	POWER_SUPPLY_PROP_TAPER_CONTROL,	/* compat */
	POWER_SUPPLY_PROP_RERUN_AICL
};

static struct power_supply_desc sm8150_psy_desc = {
	.name = "sm8150_bms",
	.type = POWER_SUPPLY_TYPE_BMS,
	.properties = sm8150_psy_props,
	.num_properties = ARRAY_SIZE(sm8150_psy_props),
	.get_property = sm8150_psy_get_property,
	.set_property = sm8150_psy_set_property,
	.property_is_writeable = sm8150_property_is_writeable,
};

/* All callback functions below */

static int sm8150_notifier_cb(struct notifier_block *nb,
		unsigned long event, void *data)
{
	if (event != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_OK;
	/*TBD: notification?*/
	return NOTIFY_OK;
}

/* All init functions below this */
#define PERPH_SUBTYPE_REG		0x05
#define FG_BATT_SOC_PM8150B		0x10
#define FG_BATT_INFO_PM8150B		0x11
#define FG_MEM_IF_PM8150B		0x0D
#define FG_ADC_RR_PM8150B		0x13

static int sm8150_parse_dt_fg(struct bms_dev *bms, struct device_node *node)
{
	struct device_node *child, *revid_node;
	struct pmic_revid_data *pmic_rev_id;
	int ret = 0;
	u8 subtype;
	u32 base;

	revid_node = of_parse_phandle(node, "qcom,pmic-revid", 0);
	if (!revid_node) {
		pr_err("node: %s no rev_id\n", node->name);
		return -ENXIO;
	}

	pmic_rev_id = get_revid_data(revid_node);
	of_node_put(revid_node);
	if (IS_ERR_OR_NULL(pmic_rev_id)) {
		pr_err("node %s pmic_revid error, defer??? rc=%ld\n",
			node->name, PTR_ERR(pmic_rev_id));
		/*
		 * the revid peripheral must be registered, any failure
		 * here only indicates that the rev-id module has not
		 * probed yet.
		 */
		return -EPROBE_DEFER;
	}

	pr_info("node %s PMIC subtype %d Digital major %d\n",
		node->name, pmic_rev_id->pmic_subtype, pmic_rev_id->rev4);

	for_each_available_child_of_node(node, child) {

		ret = of_property_read_u32(child, "reg", &base);
		if (ret < 0) {
			dev_err(bms->dev, "reg not specified in node %s, rc=%d\n",
				child->full_name, ret);
			return ret;
		}

		ret = sm8150_read(bms->pmic_regmap,
					base + PERPH_SUBTYPE_REG, &subtype, 1);
		if (ret < 0) {
			dev_err(bms->dev, "Couldn't read subtype for base %d, rc=%d\n",
				base, ret);
			return ret;
		}

		switch (subtype) {
		case FG_BATT_SOC_PM8150B:
			break;
		case FG_BATT_INFO_PM8150B:
			break;
		case FG_MEM_IF_PM8150B:
			break;
		case FG_ADC_RR_PM8150B:
			bms->rradc_base = base;
			break;
		default:
			dev_err(bms->dev, "Invalid peripheral subtype 0x%x\n",
				subtype);
			break;
		}
	}

	return 0;
}

static int sm8150_parse_dt(struct bms_dev *bms)
{
	struct device_node *fg_node, *node = bms->dev->of_node;
	const char *psy_name = NULL;
	int ret;

	if (!node)  {
		pr_err("device tree node missing\n");
		return -ENXIO;
	}

	fg_node = of_get_parent(node);
	if (fg_node)
		fg_node = of_get_child_by_name(fg_node, "qpnp,fg");
	if (fg_node)
		sm8150_parse_dt_fg(bms, fg_node);
	else
		pr_err("cannot find qpnp,fg, rradc not available\n");

	ret = of_property_read_u32(node, "google,chg-term-voltage",
				   &bms->chg_term_voltage);
	if (ret < 0)
		bms->chg_term_voltage = CHG_TERM_VOLTAGE;

	ret = of_property_read_string(node, "google,psy-name", &psy_name);
	if (ret == 0)
		sm8150_psy_desc.name =
			devm_kstrdup(bms->dev, psy_name, GFP_KERNEL);

	/* compat/fake */
	bms->taper_control = POWER_SUPPLY_TAPER_CONTROL_MODE_STEPPER;
	bms->fcc_stepper_enable = of_property_read_bool(node,
						"qcom,fcc-stepping-enable");

	return 0;
}

static int sm8150_storage_iter(int index, gbms_tag_t *tag, void *ptr)
{
	if (index != 0)
		return -ENOENT;
	*tag = GBMS_TAG_BRID;
	return 0;
}

static int sm8150_storage_read(gbms_tag_t tag, void *buff, size_t size,
			       void *ptr)
{
	struct bms_dev *bms = (struct bms_dev *)ptr;

	if (tag != GBMS_TAG_BRID)
		return -ENOENT;
	if (size != sizeof(int))
		return -EINVAL;

	*((int *)buff) = bms->batt_id_ohms;
	return 0;
}

static struct gbms_storage_desc sm8150_storage_dsc = {
	.iter = sm8150_storage_iter,
	.read = sm8150_storage_read,
};

static int bms_probe(struct platform_device *pdev)
{
	struct bms_dev *bms;
	struct power_supply_config bms_psy_cfg = {};
	int rc = 0;

	bms = devm_kzalloc(&pdev->dev, sizeof(*bms), GFP_KERNEL);
	if (!bms) {
		pr_info("kalloc error\n");
		return -ENOMEM;
	}

	bms->dev = &pdev->dev;
	bms->batt_id_ohms = -EINVAL;
	bms->pmic_regmap = dev_get_regmap(bms->dev->parent, NULL);
	if (!bms->pmic_regmap) {
		pr_err("Parent regmap is unavailable\n");
	} else {
		sm8150_get_batt_id(bms, &bms->batt_id_ohms);

		rc = gbms_storage_register(&sm8150_storage_dsc, "pmic", bms);
		if (rc < 0)
			pr_err("Couldn't register the storage rc = %d\n", rc);
	}

	rc = sm8150_parse_dt(bms);
	if (rc < 0) {
		pr_err("Parse the device tree fail. rc = %d\n", rc);
		goto exit;
	}

	/* Register the power supply */
	bms_psy_cfg.drv_data = bms;
	bms_psy_cfg.of_node = bms->dev->of_node;
	bms_psy_cfg.supplied_to = NULL;
	bms_psy_cfg.num_supplicants = 0;
	bms->psy = devm_power_supply_register(bms->dev, &sm8150_psy_desc,
			&bms_psy_cfg);
	if (IS_ERR(bms->psy)) {
		pr_err("failed to register psy rc = %ld\n", PTR_ERR(bms->psy));
		goto exit;
	}

	bms->nb.notifier_call = sm8150_notifier_cb;
	rc = power_supply_reg_notifier(&bms->nb);
	if (rc < 0) {
		pr_err("Couldn't register psy notifier rc = %d\n", rc);
		goto exit;
	}

	rc = sm8150_request_interrupts(bms);
	if (rc < 0) {
		pr_err("Couldn't register the interrupts rc = %d\n", rc);
		goto exit;
	}

	pr_info("BMS driver probed successfully\n");

	return 0;
exit:
	return rc;
}

static int bms_remove(struct platform_device *pdev)
{
	struct bms_dev *bms = platform_get_drvdata(pdev);

	sm8150_free_interrupts(bms);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static void bms_shutdown(struct platform_device *pdev)
{
	struct bms_dev *bms = platform_get_drvdata(pdev);

	/* disable all interrupts */
	sm8150_disable_interrupts(bms);

	return;
}

static const struct of_device_id bms_of_match[] = {
	{.compatible = "google,sm8150_bms"},
	{},
};

static struct platform_driver sm8150_bms_driver = {
	.driver = {
		.name = BMS_DEV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = bms_of_match,
	},
	.probe		= bms_probe,
	.remove		= bms_remove,
	.shutdown	= bms_shutdown,
};

module_platform_driver(sm8150_bms_driver);
MODULE_DESCRIPTION("SM8150 BMS driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" BMS_DEV_NAME);
