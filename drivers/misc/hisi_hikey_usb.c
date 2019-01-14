// SPDX-License-Identifier: GPL-2.0+
/*
 * hisi_hikey_usb.c
 *
 * Copyright (c) Hisilicon Tech. Co., Ltd. All rights reserved.
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
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/extcon-provider.h>
#include <linux/usb/role.h>

#define DEVICE_DRIVER_NAME "hisi_hikey_usb"

#define HUB_VBUS_POWER_ON 1
#define HUB_VBUS_POWER_OFF 0
#define USB_SWITCH_TO_HUB 1
#define USB_SWITCH_TO_TYPEC 0

#define INVALID_GPIO_VALUE (-1)

struct hisi_hikey_usb {
	int otg_switch_gpio;
	int typec_vbus_gpio;
	int typec_vbus_enable_val;
	int hub_vbus_gpio;

	struct extcon_dev *edev;
	struct usb_role_switch *role_sw;
};

static const unsigned int usb_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

static void hub_power_ctrl(struct hisi_hikey_usb *hisi_hikey_usb, int value)
{
	int gpio = hisi_hikey_usb->hub_vbus_gpio;

	if (gpio_is_valid(gpio))
		gpio_set_value(gpio, value);
}

static void usb_switch_ctrl(struct hisi_hikey_usb *hisi_hikey_usb,
		int switch_to)
{
	int gpio = hisi_hikey_usb->otg_switch_gpio;
	const char *switch_to_str = (switch_to == USB_SWITCH_TO_HUB) ?
		"hub" : "typec";

	if (!gpio_is_valid(gpio)) {
		pr_err("%s: otg_switch_gpio is err\n", __func__);
		return;
	}

	if (gpio_get_value(gpio) == switch_to) {
		pr_info("%s: already switch to %s\n", __func__, switch_to_str);
		return;
	}

	gpio_direction_output(gpio, switch_to);
	pr_info("%s: switch to %s\n", __func__, switch_to_str);
}

static void usb_typec_power_ctrl(struct hisi_hikey_usb *hisi_hikey_usb,
		int value)
{
	int gpio = hisi_hikey_usb->typec_vbus_gpio;

	if (!gpio_is_valid(gpio)) {
		pr_err("%s: typec power gpio is err\n", __func__);
		return;
	}

	if (gpio_get_value(gpio) == value) {
		pr_info("%s: typec power no change\n", __func__);
		return;
	}

	gpio_direction_output(gpio, value);
	pr_info("%s: set typec vbus gpio to %d\n", __func__, value);
}

static int extcon_hisi_pd_set_role(struct device *dev, enum usb_role role)
{
	struct hisi_hikey_usb *hisi_hikey_usb = dev_get_drvdata(dev);

	dev_info(dev, "%s:set usb role to %d\n", __func__, role);
	switch (role) {
	case USB_ROLE_NONE:
		usb_switch_ctrl(hisi_hikey_usb, USB_SWITCH_TO_HUB);
		usb_typec_power_ctrl(hisi_hikey_usb,
				!hisi_hikey_usb->typec_vbus_enable_val);
		hub_power_ctrl(hisi_hikey_usb, HUB_VBUS_POWER_ON);
		extcon_set_state_sync(hisi_hikey_usb->edev, EXTCON_USB, false);
		extcon_set_state_sync(hisi_hikey_usb->edev, EXTCON_USB_HOST,
				true);
		break;
	case USB_ROLE_HOST:
		usb_switch_ctrl(hisi_hikey_usb, USB_SWITCH_TO_TYPEC);
		usb_typec_power_ctrl(hisi_hikey_usb,
				hisi_hikey_usb->typec_vbus_enable_val);
		extcon_set_state_sync(hisi_hikey_usb->edev, EXTCON_USB, false);
		extcon_set_state_sync(hisi_hikey_usb->edev, EXTCON_USB_HOST,
				true);
		break;
	case USB_ROLE_DEVICE:
		hub_power_ctrl(hisi_hikey_usb, HUB_VBUS_POWER_OFF);
		usb_typec_power_ctrl(hisi_hikey_usb,
				hisi_hikey_usb->typec_vbus_enable_val);
		usb_switch_ctrl(hisi_hikey_usb, USB_SWITCH_TO_TYPEC);
		extcon_set_state_sync(hisi_hikey_usb->edev, EXTCON_USB_HOST,
				false);
		extcon_set_state_sync(hisi_hikey_usb->edev, EXTCON_USB, true);
		break;
	}

	return 0;
}

static enum usb_role extcon_hisi_pd_get_role(struct device *dev)
{
	struct hisi_hikey_usb *hisi_hikey_usb = dev_get_drvdata(dev);

	return usb_role_switch_get_role(hisi_hikey_usb->role_sw);
}

static const struct usb_role_switch_desc sw_desc = {
	.set = extcon_hisi_pd_set_role,
	.get = extcon_hisi_pd_get_role,
	.allow_userspace_control = true,
};

static int hisi_hikey_usb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *root = dev->of_node;
	struct hisi_hikey_usb *hisi_hikey_usb;
	int ret;

	hisi_hikey_usb = devm_kzalloc(dev, sizeof(*hisi_hikey_usb), GFP_KERNEL);
	if (!hisi_hikey_usb)
		return -ENOMEM;

	dev_set_name(dev, "hisi_hikey_usb");

	hisi_hikey_usb->hub_vbus_gpio = INVALID_GPIO_VALUE;
	hisi_hikey_usb->otg_switch_gpio = INVALID_GPIO_VALUE;
	hisi_hikey_usb->typec_vbus_gpio = INVALID_GPIO_VALUE;

	hisi_hikey_usb->hub_vbus_gpio = of_get_named_gpio(root,
			"hub_vdd33_en_gpio", 0);
	if (!gpio_is_valid(hisi_hikey_usb->hub_vbus_gpio)) {
		pr_err("%s: hub_vbus_gpio is err\n", __func__);
		return hisi_hikey_usb->hub_vbus_gpio;
	}

	ret = gpio_request(hisi_hikey_usb->hub_vbus_gpio, "hub_vbus_int_gpio");
	if (ret) {
		pr_err("%s: request hub_vbus_gpio err\n", __func__);
		hisi_hikey_usb->hub_vbus_gpio = INVALID_GPIO_VALUE;
		return ret;
	}

	ret = gpio_direction_output(hisi_hikey_usb->hub_vbus_gpio,
			HUB_VBUS_POWER_ON);
	if (ret) {
		pr_err("%s: power on hub vbus err\n", __func__);
		goto free_gpio1;
	}

	hisi_hikey_usb->typec_vbus_gpio = of_get_named_gpio(root,
		"typc_vbus_int_gpio,typec-gpios", 0);
	if (!gpio_is_valid(hisi_hikey_usb->typec_vbus_gpio)) {
		pr_err("%s: typec_vbus_gpio is err\n", __func__);
		ret = hisi_hikey_usb->typec_vbus_gpio;
		goto free_gpio1;
	}

	ret = gpio_request(hisi_hikey_usb->typec_vbus_gpio,
			"typc_vbus_int_gpio");
	if (ret) {
		pr_err("%s: request typec_vbus_gpio err\n", __func__);
		hisi_hikey_usb->typec_vbus_gpio = INVALID_GPIO_VALUE;
		goto free_gpio1;
	}

	ret = of_property_read_u32(root, "typc_vbus_enable_val",
				   &hisi_hikey_usb->typec_vbus_enable_val);
	if (ret) {
		pr_err("%s: typc_vbus_enable_val can't get\n", __func__);
		goto free_gpio2;
	}

	hisi_hikey_usb->typec_vbus_enable_val =
		!!hisi_hikey_usb->typec_vbus_enable_val;

	ret = gpio_direction_output(hisi_hikey_usb->typec_vbus_gpio,
				    hisi_hikey_usb->typec_vbus_enable_val);
	if (ret) {
		pr_err("%s: power on typec vbus err", __func__);
		goto free_gpio2;
	}

	if (of_device_is_compatible(root, "hisilicon,hikey960_usb")) {
		hisi_hikey_usb->otg_switch_gpio = of_get_named_gpio(root,
				"otg_gpio", 0);
		if (!gpio_is_valid(hisi_hikey_usb->otg_switch_gpio)) {
			pr_info("%s: otg_switch_gpio is err\n", __func__);
			goto free_gpio2;
		}

		ret = gpio_request(hisi_hikey_usb->otg_switch_gpio,
				"otg_switch_gpio");
		if (ret) {
			hisi_hikey_usb->otg_switch_gpio = INVALID_GPIO_VALUE;
			pr_err("%s: request typec_vbus_gpio err\n", __func__);
			goto free_gpio2;
		}
	}

	hisi_hikey_usb->edev = devm_extcon_dev_allocate(dev, usb_extcon_cable);
	if (IS_ERR(hisi_hikey_usb->edev)) {
		dev_err(dev, "failed to allocate extcon device\n");
		goto free_gpio2;
	}

	ret = devm_extcon_dev_register(dev, hisi_hikey_usb->edev);
	if (ret < 0) {
		dev_err(dev, "failed to register extcon device\n");
		goto free_gpio2;
	}
	extcon_set_state(hisi_hikey_usb->edev, EXTCON_USB_HOST, true);

	hisi_hikey_usb->role_sw = usb_role_switch_register(dev, &sw_desc);
	if (IS_ERR(hisi_hikey_usb->role_sw))
		goto free_gpio2;

	platform_set_drvdata(pdev, hisi_hikey_usb);

	return 0;

free_gpio2:
	if (gpio_is_valid(hisi_hikey_usb->typec_vbus_gpio)) {
		gpio_free(hisi_hikey_usb->typec_vbus_gpio);
		hisi_hikey_usb->typec_vbus_gpio = INVALID_GPIO_VALUE;
	}

free_gpio1:
	if (gpio_is_valid(hisi_hikey_usb->hub_vbus_gpio)) {
		gpio_free(hisi_hikey_usb->hub_vbus_gpio);
		hisi_hikey_usb->hub_vbus_gpio = INVALID_GPIO_VALUE;
	}

	return ret;
}

static int  hisi_hikey_usb_remove(struct platform_device *pdev)
{
	struct hisi_hikey_usb *hisi_hikey_usb = platform_get_drvdata(pdev);

	if (gpio_is_valid(hisi_hikey_usb->otg_switch_gpio)) {
		gpio_free(hisi_hikey_usb->otg_switch_gpio);
		hisi_hikey_usb->otg_switch_gpio = INVALID_GPIO_VALUE;
	}

	if (gpio_is_valid(hisi_hikey_usb->typec_vbus_gpio)) {
		gpio_free(hisi_hikey_usb->typec_vbus_gpio);
		hisi_hikey_usb->typec_vbus_gpio = INVALID_GPIO_VALUE;
	}

	if (gpio_is_valid(hisi_hikey_usb->hub_vbus_gpio)) {
		gpio_free(hisi_hikey_usb->hub_vbus_gpio);
		hisi_hikey_usb->hub_vbus_gpio = INVALID_GPIO_VALUE;
	}

	usb_role_switch_unregister(hisi_hikey_usb->role_sw);

	return 0;
}

static const struct of_device_id id_table_hisi_hikey_usb[] = {
	{.compatible = "hisilicon,gpio_hubv1"},
	{.compatible = "hisilicon,hikey960_usb"},
	{}
};

static struct platform_driver  hisi_hikey_usb_driver = {
	.probe = hisi_hikey_usb_probe,
	.remove = hisi_hikey_usb_remove,
	.driver = {
		.name = DEVICE_DRIVER_NAME,
		.of_match_table = of_match_ptr(id_table_hisi_hikey_usb),

	},
};

module_platform_driver(hisi_hikey_usb_driver);

MODULE_AUTHOR("Yu Chen <chenyu56@huawei.com>");
MODULE_DESCRIPTION("Driver Support for USB functionality of Hikey");
MODULE_LICENSE("GPL v2");
