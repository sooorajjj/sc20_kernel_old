/*
 * Copyright (C) 2013, NVIDIA Corporation.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/backlight.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

struct panel_desc {
	const struct drm_display_mode *modes;
	unsigned int num_modes;

	struct {
		unsigned int width;
		unsigned int height;
	} size;
};

/* TODO: convert to gpiod_*() API once it's been merged */
#define GPIO_ACTIVE_LOW	(1 << 0)

struct panel_simple {
	struct drm_panel base;
	bool enabled;

	const struct panel_desc *desc;

	struct backlight_device *backlight;
	struct regulator *supply;
	struct i2c_adapter *ddc;

	unsigned long enable_gpio_flags;
	int enable_gpio;
};

static inline struct panel_simple *to_panel_simple(struct drm_panel *panel)
{
	return container_of(panel, struct panel_simple, base);
}

static int panel_simple_get_fixed_modes(struct panel_simple *panel)
{
	struct drm_connector *connector = panel->base.connector;
	struct drm_device *drm = panel->base.drm;
	struct drm_display_mode *mode;
	unsigned int i, num = 0;

	if (!panel->desc)
		return 0;

	for (i = 0; i < panel->desc->num_modes; i++) {
		const struct drm_display_mode *m = &panel->desc->modes[i];

		mode = drm_mode_duplicate(drm, m);
		if (!mode) {
			dev_err(drm->dev, "failed to add mode %ux%u@%u\n",
				m->hdisplay, m->vdisplay, m->vrefresh);
			continue;
		}

		drm_mode_set_name(mode);

		drm_mode_probed_add(connector, mode);
		num++;
	}

	connector->display_info.width_mm = panel->desc->size.width;
	connector->display_info.height_mm = panel->desc->size.height;

	return num;
}

static int panel_simple_disable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	if (!p->enabled)
		return 0;

	if (p->backlight) {
		p->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(p->backlight);
	}

	if (gpio_is_valid(p->enable_gpio)) {
		if (p->enable_gpio_flags & GPIO_ACTIVE_LOW)
			gpio_set_value(p->enable_gpio, 1);
		else
			gpio_set_value(p->enable_gpio, 0);
	}

	regulator_disable(p->supply);
	p->enabled = false;

	return 0;
}

static int panel_simple_enable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	int err;

	if (p->enabled)
		return 0;

	err = regulator_enable(p->supply);
	if (err < 0) {
		dev_err(panel->dev, "failed to enable supply: %d\n", err);
		return err;
	}

	if (gpio_is_valid(p->enable_gpio)) {
		if (p->enable_gpio_flags & GPIO_ACTIVE_LOW)
			gpio_set_value(p->enable_gpio, 0);
		else
			gpio_set_value(p->enable_gpio, 1);
	}

	if (p->backlight) {
		p->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(p->backlight);
	}

	p->enabled = true;

	return 0;
}

static int panel_simple_get_modes(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	int num = 0;

	/* probe EDID if a DDC bus is available */
	if (p->ddc) {
		struct edid *edid = drm_get_edid(panel->connector, p->ddc);
		drm_mode_connector_update_edid_property(panel->connector, edid);
		if (edid) {
			num += drm_add_edid_modes(panel->connector, edid);
			kfree(edid);
		}
	}

	/* add hard-coded panel modes */
	num += panel_simple_get_fixed_modes(p);

	return num;
}

static const struct drm_panel_funcs panel_simple_funcs = {
	.disable = panel_simple_disable,
	.enable = panel_simple_enable,
	.get_modes = panel_simple_get_modes,
};

static int panel_simple_probe(struct device *dev, const struct panel_desc *desc)
{
	struct device_node *backlight, *ddc;
	struct panel_simple *panel;
	enum of_gpio_flags flags;
	int err;

	panel = devm_kzalloc(dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	panel->enabled = false;
	panel->desc = desc;

	panel->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(panel->supply))
		return PTR_ERR(panel->supply);

	panel->enable_gpio = of_get_named_gpio_flags(dev->of_node,
						     "enable-gpios", 0,
						     &flags);
	if (gpio_is_valid(panel->enable_gpio)) {
		unsigned int value;

		if (flags & OF_GPIO_ACTIVE_LOW)
			panel->enable_gpio_flags |= GPIO_ACTIVE_LOW;

		err = gpio_request(panel->enable_gpio, "enable");
		if (err < 0) {
			dev_err(dev, "failed to request GPIO#%u: %d\n",
				panel->enable_gpio, err);
			return err;
		}

		value = (panel->enable_gpio_flags & GPIO_ACTIVE_LOW) != 0;

		err = gpio_direction_output(panel->enable_gpio, value);
		if (err < 0) {
			dev_err(dev, "failed to setup GPIO%u: %d\n",
				panel->enable_gpio, err);
			goto free_gpio;
		}
	}

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		panel->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!panel->backlight) {
			err = -EPROBE_DEFER;
			goto free_gpio;
		}
	}

	ddc = of_parse_phandle(dev->of_node, "ddc-i2c-bus", 0);
	if (ddc) {
		panel->ddc = of_find_i2c_adapter_by_node(ddc);
		of_node_put(ddc);

		if (!panel->ddc) {
			err = -EPROBE_DEFER;
			goto free_backlight;
		}
	}

	drm_panel_init(&panel->base);
	panel->base.dev = dev;
	panel->base.funcs = &panel_simple_funcs;

	err = drm_panel_add(&panel->base);
	if (err < 0)
		goto free_ddc;

	dev_set_drvdata(dev, panel);

	return 0;

free_ddc:
	if (panel->ddc)
		put_device(&panel->ddc->dev);
free_backlight:
	if (panel->backlight)
		put_device(&panel->backlight->dev);
free_gpio:
	if (gpio_is_valid(panel->enable_gpio))
		gpio_free(panel->enable_gpio);

	return err;
}

static int panel_simple_remove(struct device *dev)
{
	struct panel_simple *panel = dev_get_drvdata(dev);

	drm_panel_detach(&panel->base);
	drm_panel_remove(&panel->base);

	panel_simple_disable(&panel->base);

	if (panel->ddc)
		put_device(&panel->ddc->dev);

	if (panel->backlight)
		put_device(&panel->backlight->dev);

	if (gpio_is_valid(panel->enable_gpio))
		gpio_free(panel->enable_gpio);

	return 0;
}

static void panel_simple_shutdown(struct device *dev)
{
	struct panel_simple *panel = dev_get_drvdata(dev);

	panel_simple_disable(&panel->base);
}

// static const struct drm_display_mode auo_b101aw03_mode = {
// 	.clock = 51450,
// 	.hdisplay = 1024,
// 	.hsync_start = 1024 + 156,
// 	.hsync_end = 1024 + 156 + 8,
// 	.htotal = 1024 + 156 + 8 + 156,
// 	.vdisplay = 600,
// 	.vsync_start = 600 + 16,
// 	.vsync_end = 600 + 16 + 6,
// 	.vtotal = 600 + 16 + 6 + 16,
// 	.vrefresh = 60,
// };

// static const struct panel_desc auo_b101aw03 = {
// 	.modes = &auo_b101aw03_mode,
// 	.num_modes = 1,
// 	.size = {
// 		.width = 223,
// 		.height = 125,
// 	},
// };

// static const struct drm_display_mode chunghwa_claa101wb01_mode = {
// 	.clock = 69300,
// 	.hdisplay = 1366,
// 	.hsync_start = 1366 + 48,
// 	.hsync_end = 1366 + 48 + 32,
// 	.htotal = 1366 + 48 + 32 + 20,
// 	.vdisplay = 768,
// 	.vsync_start = 768 + 16,
// 	.vsync_end = 768 + 16 + 8,
// 	.vtotal = 768 + 16 + 8 + 16,
// 	.vrefresh = 60,
// };

// static const struct panel_desc chunghwa_claa101wb01 = {
// 	.modes = &chunghwa_claa101wb01_mode,
// 	.num_modes = 1,
// 	.size = {
// 		.width = 223,
// 		.height = 125,
// 	},
// };

// static const struct of_device_id platform_of_match[] = {
// 	{
// 		.compatible = "auo,b101aw03",
// 		.data = &auo_b101aw03,
// 	}, {
// 		.compatible = "chunghwa,claa101wb01",
// 		.data = &chunghwa_claa101wb01
// 	}, {
// 		.compatible = "simple-panel",
// 	}, {
// 		/* sentinel */
// 	}
// };
// MODULE_DEVICE_TABLE(of, platform_of_match);

// static int panel_simple_platform_probe(struct platform_device *pdev)
// {
// 	const struct of_device_id *id;

// 	id = of_match_node(platform_of_match, pdev->dev.of_node);
// 	if (!id)
// 		return -ENODEV;

// 	return panel_simple_probe(&pdev->dev, id->data);
// }

// static int panel_simple_platform_remove(struct platform_device *pdev)
// {
// 	return panel_simple_remove(&pdev->dev);
// }

// static void panel_simple_platform_shutdown(struct platform_device *pdev)
// {
// 	panel_simple_shutdown(&pdev->dev);
// }

// static struct platform_driver panel_simple_platform_driver = {
// 	.driver = {
// 		.name = "panel-simple",
// 		.owner = THIS_MODULE,
// 		.of_match_table = platform_of_match,
// 	},
// 	.probe = panel_simple_platform_probe,
// 	.remove = panel_simple_platform_remove,
// 	.shutdown = panel_simple_platform_shutdown,
// };

struct panel_desc_dsi {
	struct panel_desc desc;

	unsigned long flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;
};

clock = 908 * 1312 * 60 / 100

static const struct drm_display_mode sc20_ili9881c_mode = {
	.clock = 714778,
	.hdisplay = 720,
	.hsync_start = 720 + 52,
	.hsync_end = 720 + 52 + 36,
	.htotal = 720 + 52 + 36 + 100,
	.vdisplay = 1280,
	.vsync_start = 1280 + 8,
	.vsync_end = 1280 + 8 + 4,
	.vtotal = 1280 + 8 + 4 + 20,
	.vrefresh = 60,
};

static const struct panel_desc_dsi sc20_ili9881c = {
	.desc = {
		.modes = &sc20_ili9881c_mode,
		.num_modes = 1,
		.size = {
			.width = 59,
			.height = 104,
		},
	},
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct drm_display_mode lg_lh500wx1_sd03_mode = {
	.clock = 67000,
	.hdisplay = 720,
	.hsync_start = 720 + 12,
	.hsync_end = 720 + 12 + 4,
	.htotal = 720 + 12 + 4 + 112,
	.vdisplay = 1280,
	.vsync_start = 1280 + 8,
	.vsync_end = 1280 + 8 + 4,
	.vtotal = 1280 + 8 + 4 + 12,
	.vrefresh = 60,
};

static const struct panel_desc_dsi lg_lh500wx1_sd03 = {
	.desc = {
		.modes = &lg_lh500wx1_sd03_mode,
		.num_modes = 1,
		.size = {
			.width = 62,
			.height = 110,
		},
	},
	.flags = MIPI_DSI_MODE_VIDEO,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct drm_display_mode panasonic_vvx10f004b00_mode = {
	.clock = 157200,
	.hdisplay = 1920,
	.hsync_start = 1920 + 154,
	.hsync_end = 1920 + 154 + 16,
	.htotal = 1920 + 154 + 16 + 32,
	.vdisplay = 1200,
	.vsync_start = 1200 + 17,
	.vsync_end = 1200 + 17 + 2,
	.vtotal = 1200 + 17 + 2 + 16,
	.vrefresh = 60,
};

static const struct panel_desc_dsi panasonic_vvx10f004b00 = {
	.desc = {
		.modes = &panasonic_vvx10f004b00_mode,
		.num_modes = 1,
		.size = {
			.width = 217,
			.height = 136,
		},
	},
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct of_device_id dsi_of_match[] = {
	{
		.compatible = "sc20,ili9881c",
		.data = &sc20_ili9881c
	}, {
		.compatible = "lg,lh500wx1-sd03",
		.data = &lg_lh500wx1_sd03
	}, {
		.compatible = "panasonic,vvx10f004b00",
		.data = &panasonic_vvx10f004b00
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, dsi_of_match);

static int panel_simple_dsi_probe(struct mipi_dsi_device *dsi)
{
	const struct panel_desc_dsi *desc;
	const struct of_device_id *id;
	int err,ret;

	id = of_match_node(dsi_of_match, dsi->dev.of_node);
	if (!id)
		return -ENODEV;

	desc = id->data;

	err = panel_simple_probe(&dsi->dev, &desc->desc);
	if (err < 0)
		return err;

	dsi->mode_flags = desc->flags;

	/*Display on-command sc20,ili9881c */
	ret = mipi_dsi_dcs_write(dsi, 0xff, (u8[]){ 0x98, 0x81, 0x03 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x01, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x02, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x03, (u8[]){ 0x73 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x04, (u8[]){ 0x03 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x05, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x06, (u8[]){ 0x06 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x07, (u8[]){ 0x06 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x08, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x09, (u8[]){ 0x18 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x0a, (u8[]){ 0x04 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x0b, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x0c, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x0d, (u8[]){ 0x03 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x0e, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x0f, (u8[]){ 0x25 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x10, (u8[]){ 0x25 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x11, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x12, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x13, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x14, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x15, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x16, (u8[]){ 0x0c }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x17, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x18, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x19, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x1a, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x1b, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x1c, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x1d, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x1e, (u8[]){ 0xc0 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x1f, (u8[]){ 0x80 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x20, (u8[]){ 0x04 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x21, (u8[]){ 0x01 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x22, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x23, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x24, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x25, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x26, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x27, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x28, (u8[]){ 0x33 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x29, (u8[]){ 0x03 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x2a, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x2b, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x2c, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x2d, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x2e, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x2f, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x30, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x31, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x32, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x33, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x34, (u8[]){ 0x04 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x35, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x36, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x37, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x38, (u8[]){ 0x3c }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x39, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x3a, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x3b, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x3c, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x3d, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x3e, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x3f, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x40, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x41, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x42, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x43, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x44, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x50, (u8[]){ 0x01 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x51, (u8[]){ 0x23 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x52, (u8[]){ 0x45 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x53, (u8[]){ 0x67 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x54, (u8[]){ 0x89 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x55, (u8[]){ 0xab }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x56, (u8[]){ 0x01 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x57, (u8[]){ 0x23 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x58, (u8[]){ 0x45 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x59, (u8[]){ 0x67 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x5a, (u8[]){ 0x89 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x5b, (u8[]){ 0xab }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x5c, (u8[]){ 0xcd }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x5d, (u8[]){ 0xef }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x5e, (u8[]){ 0x11 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x5f, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x60, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x61, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x62, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x63, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x64, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x65, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x66, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x67, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x68, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x69, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x6a, (u8[]){ 0x0c }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x6b, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x6c, (u8[]){ 0x0f }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x6d, (u8[]){ 0x0e }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x6e, (u8[]){ 0x0d }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x6f, (u8[]){ 0x06 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x70, (u8[]){ 0x07 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x71, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x72, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x73, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x74, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x75, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x76, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x77, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x78, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x79, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x7a, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x7b, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x7c, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x7d, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x7e, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x7f, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x80, (u8[]){ 0x0c }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x81, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x82, (u8[]){ 0x0f }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x83, (u8[]){ 0x0e }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x84, (u8[]){ 0x0d }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x85, (u8[]){ 0x06 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x86, (u8[]){ 0x07 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x87, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x88, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x89, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x8a, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xff, (u8[]){ 0x98, 0x81, 0x04 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x6c, (u8[]){ 0x15 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x6e, (u8[]){ 0x22 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x6f, (u8[]){ 0x33 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x3a, (u8[]){ 0xa4 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x8d, (u8[]){ 0x0d }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x87, (u8[]){ 0xba }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x26, (u8[]){ 0x76 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xb2, (u8[]){ 0xd1 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xff, (u8[]){ 0x98, 0x81, 0x01 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x22, (u8[]){ 0x0a }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x53, (u8[]){ 0xbe }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x55, (u8[]){ 0xa7 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x50, (u8[]){ 0x74 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x51, (u8[]){ 0x74 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x31, (u8[]){ 0x02 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x60, (u8[]){ 0x14 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xa0, (u8[]){ 0x15 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xa1, (u8[]){ 0x26 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xa2, (u8[]){ 0x2b }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xa3, (u8[]){ 0x14 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xa4, (u8[]){ 0x17 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xa5, (u8[]){ 0x2c }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xa6, (u8[]){ 0x20 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xa7, (u8[]){ 0x21 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xa8, (u8[]){ 0x95 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xa9, (u8[]){ 0x1d }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xaa, (u8[]){ 0x27 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xab, (u8[]){ 0x89 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xac, (u8[]){ 0x1a }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xad, (u8[]){ 0x18 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xae, (u8[]){ 0x4b }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xaf, (u8[]){ 0x21 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xb0, (u8[]){ 0x26 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xb1, (u8[]){ 0x60 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xb2, (u8[]){ 0x71 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xb3, (u8[]){ 0x3f }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xc0, (u8[]){ 0x05 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xc1, (u8[]){ 0x26 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xc2, (u8[]){ 0x3f }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xc3, (u8[]){ 0x0f }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xc4, (u8[]){ 0x14 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xc5, (u8[]){ 0x27 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xc6, (u8[]){ 0x1a }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xc7, (u8[]){ 0x1e }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xc8, (u8[]){ 0x9e }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xc9, (u8[]){ 0x1a }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xca, (u8[]){ 0x29 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xcb, (u8[]){ 0x82 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xcc, (u8[]){ 0x18 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xcd, (u8[]){ 0x16 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xce, (u8[]){ 0x4c }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xcf, (u8[]){ 0x1f }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xd0, (u8[]){ 0x28 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xd1, (u8[]){ 0x53 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xd2, (u8[]){ 0x62 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xd3, (u8[]){ 0x3f }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0xff, (u8[]){ 0x98, 0x81, 0x00 }, 1);
	msleep(1);
	ret = mipi_dsi_dcs_write(dsi, 0x11, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x29, (u8[]){ 0x00 }, 1);
	ret = mipi_dsi_dcs_write(dsi, 0x35, (u8[]){ 0x00 }, 1);

	dsi->format = desc->format;
	dsi->lanes = desc->lanes;

	return mipi_dsi_attach(dsi);
}

static int panel_simple_dsi_remove(struct mipi_dsi_device *dsi)
{
	int err;

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	return panel_simple_remove(&dsi->dev);
}

static void panel_simple_dsi_shutdown(struct mipi_dsi_device *dsi)
{
	panel_simple_shutdown(&dsi->dev);
}

static struct mipi_dsi_driver panel_simple_dsi_driver = {
	.driver = {
		.name = "panel-simple-dsi",
		.owner = THIS_MODULE,
		.of_match_table = dsi_of_match,
	},
	.probe = panel_simple_dsi_probe,
	.remove = panel_simple_dsi_remove,
	.shutdown = panel_simple_dsi_shutdown,
};

static int __init panel_simple_init(void)
{
    printk("panel-simple: init....\n");
/*	int err;

	err = platform_driver_register(&panel_simple_platform_driver);
	if (err < 0)
		return err;

	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI)) {
		err = mipi_dsi_driver_register(&panel_simple_dsi_driver);
		if (err < 0)
			return err;
	}*/

	return mipi_dsi_driver_register(&panel_simple_dsi_driver);
}
module_init(panel_simple_init);

static void __exit panel_simple_exit(void)
{
	printk("panel-simple: init....\n");
	/*if (IS_ENABLED(CONFIG_DRM_MIPI_DSI))*/
		mipi_dsi_driver_unregister(&panel_simple_dsi_driver);
	
	/*platform_driver_unregister(&panel_simple_platform_driver);*/
}
module_exit(panel_simple_exit);

MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_DESCRIPTION("DRM Driver for Simple Panels");
MODULE_LICENSE("GPL and additional rights");
