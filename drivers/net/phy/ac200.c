// SPDX-License-Identifier: GPL-2.0+
/**
 * Driver for AC200 Ethernet PHY
 *
 * Copyright (c) 2019 Jernej Skrabec <jernej.skrabec@siol.net>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/ac200.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/platform_device.h>

#define AC200_EPHY_ID			0x00441400
#define AC200_EPHY_ID_MASK		0x0ffffff0

/* macros for system ephy control 0 register */
#define AC200_EPHY_RESET_INVALID	BIT(0)
#define AC200_EPHY_SYSCLK_GATING	BIT(1)

/* macros for system ephy control 1 register */
#define AC200_EPHY_E_EPHY_MII_IO_EN	BIT(0)
#define AC200_EPHY_E_LNK_LED_IO_EN	BIT(1)
#define AC200_EPHY_E_SPD_LED_IO_EN	BIT(2)
#define AC200_EPHY_E_DPX_LED_IO_EN	BIT(3)

/* macros for ephy control register */
#define AC200_EPHY_SHUTDOWN		BIT(0)
#define AC200_EPHY_LED_POL		BIT(1)
#define AC200_EPHY_CLK_SEL		BIT(2)
#define AC200_EPHY_ADDR(x)		(((x) & 0x1F) << 4)
#define AC200_EPHY_XMII_SEL		BIT(11)
#define AC200_EPHY_CALIB(x)		(((x) & 0xF) << 12)

struct ac200_ephy_dev {
	struct phy_driver	*ephy;
	struct ac200_dev	*ac200;
};

static char *ac200_phy_name = "AC200 EPHY";

static void disable_intelligent_ieee(struct phy_device *phydev)
{
	unsigned int value;

	phy_write(phydev, 0x1f, 0x0100);	/* switch to page 1 */
	value = phy_read(phydev, 0x17);
	value &= ~BIT(3);			/* disable IEEE */
	phy_write(phydev, 0x17, value);
	phy_write(phydev, 0x1f, 0x0000);	/* switch to page 0 */
}

static void disable_802_3az_ieee(struct phy_device *phydev)
{
	unsigned int value;

	phy_write(phydev, 0xd, 0x7);
	phy_write(phydev, 0xe, 0x3c);
	phy_write(phydev, 0xd, BIT(14) | 0x7);
	value = phy_read(phydev, 0xe);
	value &= ~BIT(1);
	phy_write(phydev, 0xd, 0x7);
	phy_write(phydev, 0xe, 0x3c);
	phy_write(phydev, 0xd, BIT(14) | 0x7);
	phy_write(phydev, 0xe, value);

	phy_write(phydev, 0x1f, 0x0200);	/* switch to page 2 */
	phy_write(phydev, 0x18, 0x0000);
}

static int ac200_ephy_config_init(struct phy_device *phydev)
{
	const struct ac200_ephy_dev *priv = phydev->drv->driver_data;
	u16 value;

	phy_write(phydev, 0x1f, 0x0100);	/* Switch to Page 1 */
	phy_write(phydev, 0x12, 0x4824);	/* Disable APS */

	phy_write(phydev, 0x1f, 0x0200);	/* Switch to Page 2 */
	phy_write(phydev, 0x18, 0x0000);	/* PHYAFE TRX optimization */

	phy_write(phydev, 0x1f, 0x0600);	/* Switch to Page 6 */
	phy_write(phydev, 0x14, 0x708f);	/* PHYAFE TX optimization */
	phy_write(phydev, 0x13, 0xF000);	/* PHYAFE RX optimization */
	phy_write(phydev, 0x15, 0x1530);

	phy_write(phydev, 0x1f, 0x0800);	/* Switch to Page 6 */
	phy_write(phydev, 0x18, 0x00bc);	/* PHYAFE TRX optimization */

	disable_intelligent_ieee(phydev);	/* Disable Intelligent IEEE */
	disable_802_3az_ieee(phydev);		/* Disable 802.3az IEEE */
	phy_write(phydev, 0x1f, 0x0000);	/* Switch to Page 0 */

	value = (phydev->interface == PHY_INTERFACE_MODE_RMII) ?
		AC200_EPHY_XMII_SEL : 0;
	ac200_reg_mod(priv->ac200, AC200_EPHY_CTL, AC200_EPHY_XMII_SEL, value);

	/* FIXME: This is probably H6 specific */
	value = phy_read(phydev, 0x13);
	value |= BIT(12);
	phy_write(phydev, 0x13, value);

	return 0;

}

static const struct mdio_device_id __maybe_unused ac200_ephy_phy_tbl[] = {
	{ AC200_EPHY_ID, AC200_EPHY_ID_MASK },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(mdio, ac200_ephy_phy_tbl);

static int ac200_ephy_probe(struct platform_device *pdev)
{
	struct ac200_dev *ac200 = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct ac200_ephy_dev *priv;
	struct nvmem_cell *calcell;
	struct phy_driver *ephy;
	u16 *caldata, calib;
	size_t callen;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ephy = devm_kzalloc(dev, sizeof(*ephy), GFP_KERNEL);
	if (!ephy)
		return -ENOMEM;

	calcell = devm_nvmem_cell_get(dev, "ephy_calib");
	if (IS_ERR(calcell)) {
		dev_err(dev, "Unable to find calibration data!\n");
		return PTR_ERR(calcell);
	}

	caldata = nvmem_cell_read(calcell, &callen);
	if (IS_ERR(caldata)) {
		dev_err(dev, "Unable to read calibration data!\n");
		return PTR_ERR(caldata);
	}

	if (callen != 2) {
		dev_err(dev, "Calibration data has wrong length: 2 != %lu\n",
			callen);
		return -EINVAL;
	}

	calib = *caldata + 3;
	kfree(caldata);

	ephy->phy_id = AC200_EPHY_ID;
	ephy->phy_id_mask = AC200_EPHY_ID_MASK;
	ephy->name = ac200_phy_name;
	ephy->driver_data = priv;
	ephy->soft_reset = genphy_soft_reset;
	ephy->config_init = ac200_ephy_config_init;
	ephy->suspend = genphy_suspend;
	ephy->resume = genphy_resume;

	priv->ac200 = ac200;
	priv->ephy = ephy;
	platform_set_drvdata(pdev, priv);

	ret = ac200_reg_write(ac200, AC200_SYS_EPHY_CTL0,
			      AC200_EPHY_RESET_INVALID |
			      AC200_EPHY_SYSCLK_GATING);
	if (ret)
		return ret;

	ret = ac200_reg_write(ac200, AC200_SYS_EPHY_CTL1,
			      AC200_EPHY_E_EPHY_MII_IO_EN |
			      AC200_EPHY_E_LNK_LED_IO_EN |
			      AC200_EPHY_E_SPD_LED_IO_EN |
			      AC200_EPHY_E_DPX_LED_IO_EN);
	if (ret)
		return ret;

	ret = ac200_reg_write(ac200, AC200_EPHY_CTL,
			      AC200_EPHY_LED_POL |
			      AC200_EPHY_CLK_SEL |
			      AC200_EPHY_ADDR(1) |
			      AC200_EPHY_CALIB(calib));
	if (ret)
		return ret;

	ret = phy_driver_register(priv->ephy, THIS_MODULE);
	if (ret) {
		dev_err(dev, "Unable to register phy\n");
		return ret;
	}

	return 0;
}

static int ac200_ephy_remove(struct platform_device *pdev)
{
	struct ac200_ephy_dev *priv = platform_get_drvdata(pdev);

	phy_driver_unregister(priv->ephy);

	ac200_reg_write(priv->ac200, AC200_EPHY_CTL,
			AC200_EPHY_SHUTDOWN);
	ac200_reg_write(priv->ac200, AC200_SYS_EPHY_CTL1, 0);
	ac200_reg_write(priv->ac200, AC200_SYS_EPHY_CTL0, 0);

	return 0;
}

static const struct of_device_id ac200_ephy_match[] = {
	{ .compatible = "x-powers,ac200-ephy" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ac200_ephy_match);

static struct platform_driver ac200_ephy_driver = {
	.probe		= ac200_ephy_probe,
	.remove		= ac200_ephy_remove,
	.driver		= {
		.name		= "ac200-ephy",
		.of_match_table	= ac200_ephy_match,
	},
};
module_platform_driver(ac200_ephy_driver);

MODULE_AUTHOR("Jernej Skrabec <jernej.skrabec@siol.net>>");
MODULE_DESCRIPTION("AC200 Ethernet PHY driver");
MODULE_LICENSE("GPL");
