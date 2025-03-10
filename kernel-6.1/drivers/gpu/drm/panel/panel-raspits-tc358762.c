/*
 * Copyright (C) 2013, NVIDIA Corporation.  All rights reserved.
 * Copyright (c) 2022 Radxa Computer Co., Ltd.
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
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <linux/errno.h>
#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>
#include <linux/delay.h>
int trigger_bridge_raspits_tc358762 = 1;
static int timeout = 5;

struct panel_desc {
	const struct drm_display_mode *modes;
	unsigned int num_modes;
	const struct display_timing *timings;
	unsigned int num_timings;

	unsigned int bpc;

	struct {
		unsigned int width;
		unsigned int height;
	} size;

	/**
	 * @prepare: the time (in milliseconds) that it takes for the panel to
	 *		become ready and start receiving video data
	 * @enable: the time (in milliseconds) that it takes for the panel to
	 *		display the first valid frame after starting to receive
	 *		video data
	 * @disable: the time (in milliseconds) that it takes for the panel to
	 *		turn the display off (no content is visible)
	 * @unprepare: the time (in milliseconds) that it takes for the panel
	 *		to power itself down completely
	 */
	struct {
		unsigned int prepare;
		unsigned int enable;
		unsigned int disable;
		unsigned int unprepare;
	} delay;

	u32 bus_format;
};

struct raspits_tc358762 {
	struct drm_panel base;
	bool prepared;
	bool enabled;

	struct device *dev;
	struct mipi_dsi_device *dsi;
	const struct panel_desc *desc;

	struct backlight_device *backlight;
	struct regulator *supply;
	struct i2c_adapter *ddc;

	struct gpio_desc *enable_gpio;
	struct drm_connector *connector;
	struct drm_device *drm;
};

static inline struct raspits_tc358762 *to_raspits_tc358762(struct drm_panel *panel)
{
	return container_of(panel, struct raspits_tc358762, base);
}

static int raspits_tc358762_get_fixed_modes(struct raspits_tc358762 *panel)
{
	struct drm_connector *connector = panel->connector;
	struct drm_device *drm = panel->drm;
	struct drm_display_mode *mode;
	unsigned int i, num = 0;

	if (!panel->desc)
		return 0;

	for (i = 0; i < panel->desc->num_timings; i++) {
		const struct display_timing *dt = &panel->desc->timings[i];
		struct videomode vm;

		videomode_from_timing(dt, &vm);
		mode = drm_mode_create(drm);
		if (!mode) {
			dev_err(drm->dev, "failed to add mode %ux%u\n",
				dt->hactive.typ, dt->vactive.typ);
			continue;
		}

		drm_display_mode_from_videomode(&vm, mode);
		drm_mode_set_name(mode);

		drm_mode_probed_add(connector, mode);
		num++;
	}

	for (i = 0; i < panel->desc->num_modes; i++) {
		const struct drm_display_mode *m = &panel->desc->modes[i];

		mode = drm_mode_duplicate(drm, m);
		if (!mode) {
			dev_err(drm->dev, "failed to add mode %ux%u\n",
				m->hdisplay, m->vdisplay);
			continue;
		}

		drm_mode_set_name(mode);

		drm_mode_probed_add(connector, mode);
		num++;
	}

	connector->display_info.bpc = panel->desc->bpc;
	connector->display_info.width_mm = panel->desc->size.width;
	connector->display_info.height_mm = panel->desc->size.height;
	if (panel->desc->bus_format)
		drm_display_info_set_bus_formats(&connector->display_info,
						 &panel->desc->bus_format, 1);

	return num;
}

static int raspits_tc358762_of_get_native_mode(struct raspits_tc358762 *panel)
{
	struct drm_connector *connector = panel->connector;
	struct drm_device *drm = panel->drm;
	struct drm_display_mode *mode;
	struct device_node *timings_np;
	int ret;
	u32 bus_flags;

	timings_np = of_get_child_by_name(panel->dev->of_node,
					  "display-timings");
	if (!timings_np) {
		dev_dbg(panel->dev, "failed to find display-timings node\n");
		return 0;
	}

	of_node_put(timings_np);
	mode = drm_mode_create(drm);
	if (!mode)
		return 0;

	ret = of_get_drm_display_mode(panel->dev->of_node, mode, &bus_flags, OF_USE_NATIVE_MODE);
	if (ret) {
		dev_dbg(panel->dev, "failed to find dts display timings\n");
		drm_mode_destroy(drm, mode);
		return 0;
	}

	drm_mode_set_name(mode);
	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

extern int rockpi_mcu_set_bright(int bright);
static int raspits_tc358762_disable(struct drm_panel *panel)
{
	struct raspits_tc358762 *p = to_raspits_tc358762(panel);

	if (!p->enabled)
		return 0;

	printk("panel disable\n");

	rockpi_mcu_set_bright(0x00);

	if (p->backlight) {
		p->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(p->backlight);
	}

	if (p->desc && p->desc->delay.disable)
		msleep(p->desc->delay.disable);

	p->enabled = false;

	return 0;
}

static int raspits_tc358762_unprepare(struct drm_panel *panel)
{
	struct raspits_tc358762 *p = to_raspits_tc358762(panel);

	if (!p->prepared)
		return 0;

	if (p->enable_gpio)
		gpiod_direction_output(p->enable_gpio, 0);

	regulator_disable(p->supply);

	if (p->desc && p->desc->delay.unprepare)
		msleep(p->desc->delay.unprepare);

	p->prepared = false;

	return 0;
}

static void raspits_tc358762_gen_write(struct mipi_dsi_device *dsi, const void *data, size_t len)
{
	int ret;

	ret = mipi_dsi_generic_write(dsi, data, len);
	if (ret < 0) {
		dev_err(&dsi->dev, "failed to writing gen seq\n");
	}
}

#define raspits_tc358762_gen_write_seq(dsi, seq...) \
({\
	static const u8 d[] = { seq };\
	raspits_tc358762_gen_write(dsi, d, ARRAY_SIZE(d));\
})

static int raspits_tc358762_dsi_init(struct raspits_tc358762 *p)
{
	struct mipi_dsi_device *dsi = p->dsi;

	raspits_tc358762_gen_write_seq(dsi, 0x10, 0x02, 0x03, 0x00, 0x00, 0x00);//LANE
	raspits_tc358762_gen_write_seq(dsi, 0x64, 0x01, 0x0c, 0x00, 0x00, 0x00);//D0S_CLRSIPOCOUNT
	raspits_tc358762_gen_write_seq(dsi, 0x68, 0x01, 0x0c, 0x00, 0x00, 0x00);//D1S_CLRSIPOCOUNT
	raspits_tc358762_gen_write_seq(dsi, 0x44, 0x01, 0x00, 0x00, 0x00, 0x00);//D0S_ATMR
	raspits_tc358762_gen_write_seq(dsi, 0x48, 0x01, 0x00, 0x00, 0x00, 0x00);//D1S_ATMR
	raspits_tc358762_gen_write_seq(dsi, 0x14, 0x01, 0x15, 0x00, 0x00, 0x00);//LPTXTIMCNT
	raspits_tc358762_gen_write_seq(dsi, 0x50, 0x04, 0x60, 0x00, 0x00, 0x00);//SPICMR/SPICTRL
	raspits_tc358762_gen_write_seq(dsi, 0x20, 0x04, 0x52, 0x01, 0x10, 0x00);//PORT/LCDCTRL
	raspits_tc358762_gen_write_seq(dsi, 0x24, 0x04, 0x14, 0x00, 0x1a, 0x00);//HBPR/HSR
	raspits_tc358762_gen_write_seq(dsi, 0x28, 0x04, 0x20, 0x03, 0x69, 0x00);//HFPR/HDISP(*)
	raspits_tc358762_gen_write_seq(dsi, 0x2c, 0x04, 0x02, 0x00, 0x15, 0x00);//VBFR/VSR
	raspits_tc358762_gen_write_seq(dsi, 0x30, 0x04, 0xe0, 0x01, 0x07, 0x00);//VFPR/VDISP(*)
	raspits_tc358762_gen_write_seq(dsi, 0x34, 0x04, 0x01, 0x00, 0x00, 0x00);//VFUEN
	raspits_tc358762_gen_write_seq(dsi, 0x64, 0x04, 0x0f, 0x04, 0x00, 0x00);//SYSCTRL
	raspits_tc358762_gen_write_seq(dsi, 0x04, 0x01, 0x01, 0x00, 0x00, 0x00);//STARTPPI
	raspits_tc358762_gen_write_seq(dsi, 0x04, 0x02, 0x01, 0x00, 0x00, 0x00);//STARTDSI
	raspits_tc358762_gen_write_seq(dsi, 0x10, 0x04, 0x03, 0x00, 0x00, 0x00);//Read Size Register: RDPKTLN[2;0]

	usleep_range(10, 20);
	return 0;
}

static int raspits_tc358762_prepare(struct drm_panel *panel)
{
	struct raspits_tc358762 *p = to_raspits_tc358762(panel);
	int err;

	if (p->prepared)
		return 0;

	err = regulator_enable(p->supply);
	if (err < 0) {
		dev_err(panel->dev, "failed to enable supply: %d\n", err);
		return err;
	}

	if (p->enable_gpio)
		gpiod_direction_output(p->enable_gpio, 1);

	if (p->desc && p->desc->delay.prepare)
		msleep(p->desc->delay.prepare);

	p->prepared = true;

	return 0;
}

extern void rockpi_mcu_screen_power_up(void);
static int raspits_tc358762_enable(struct drm_panel *panel)
{
	struct raspits_tc358762 *p = to_raspits_tc358762(panel);

	if (p->enabled)
		return 0;

	printk("panel enable\n");

	if(trigger_bridge_raspits_tc358762) {
		pr_info("rockpi_mcu_screen_power_up");
		rockpi_mcu_screen_power_up();
	}

	raspits_tc358762_dsi_init(p);

	if (p->desc && p->desc->delay.enable)
		msleep(p->desc->delay.enable);

	if (p->backlight) {
		p->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(p->backlight);
	}

	rockpi_mcu_set_bright(0xFF);

	p->enabled = true;

	return 0;
}

static int raspits_tc358762_get_modes(struct drm_panel *panel,struct drm_connector *connector)
{
	struct raspits_tc358762 *p = to_raspits_tc358762(panel);
	int num = 0;

	p->connector = connector;

	/* add hard-coded panel modes */
	num += raspits_tc358762_get_fixed_modes(p);

	/* add device node plane modes */
	num += raspits_tc358762_of_get_native_mode(p);

	return num;
}

static int raspits_tc358762_get_timings(struct drm_panel *panel,
					unsigned int num_timings,
					struct display_timing *timings)
{
	struct raspits_tc358762 *p = to_raspits_tc358762(panel);
	unsigned int i;

	if (!p->desc)
		return 0;

	if (p->desc->num_timings < num_timings)
		num_timings = p->desc->num_timings;

	if (timings)
		for (i = 0; i < num_timings; i++)
			timings[i] = p->desc->timings[i];

	return p->desc->num_timings;
}

static const struct drm_panel_funcs raspits_tc358762_funcs = {
	.disable = raspits_tc358762_disable,
	.unprepare = raspits_tc358762_unprepare,
	.prepare = raspits_tc358762_prepare,
	.enable = raspits_tc358762_enable,
	.get_modes = raspits_tc358762_get_modes,
	.get_timings = raspits_tc358762_get_timings,
};

static int raspits_tc358762_mipi_probe(struct mipi_dsi_device *dsi, const struct panel_desc *desc)
{
	struct device_node *backlight, *ddc;
	struct raspits_tc358762 *panel;
	struct device *dev = &dsi->dev;
	int err;

	panel = devm_kzalloc(dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	panel->enabled = false;
	panel->prepared = false;
	panel->desc = desc;
	panel->dev = dev;
	panel->dsi = dsi;

	panel->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(panel->supply))
		return PTR_ERR(panel->supply);

	panel->enable_gpio = devm_gpiod_get_optional(dev, "enable",
							 GPIOD_OUT_LOW);
	if (IS_ERR(panel->enable_gpio)) {
		err = PTR_ERR(panel->enable_gpio);
		dev_err(dev, "failed to request GPIO: %d\n", err);
		return err;
	}

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		panel->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!panel->backlight)
			return -EPROBE_DEFER;
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

	drm_panel_init(&panel->base,dev, &raspits_tc358762_funcs,DRM_MODE_CONNECTOR_DSI);
	drm_panel_add(&panel->base);
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

	return err;
}

static void raspits_tc358762_remove(struct device *dev)
{
	struct raspits_tc358762 *panel = dev_get_drvdata(dev);

	drm_panel_remove(&panel->base);
	raspits_tc358762_disable(&panel->base);

	if (panel->ddc)
		put_device(&panel->ddc->dev);

	if (panel->backlight)
		put_device(&panel->backlight->dev);
}

static void raspits_tc358762_shutdown(struct device *dev)
{
	struct raspits_tc358762 *panel = dev_get_drvdata(dev);

	raspits_tc358762_disable(&panel->base);
}

struct bridge_desc {
	struct panel_desc desc;

	unsigned long flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;
};

static const struct drm_display_mode raspits_tc358762_mode = {
	.clock = 26000000 / 1000,
	.hdisplay = 800,
	.hsync_start = 800 + 48,
	.hsync_end = 800 + 48 + 88,
	.htotal = 800 + 48 + 88 + 40,
	.vdisplay = 480,
	.vsync_start = 480 + 5,
	.vsync_end = 480 + 5 + 1,
	.vtotal = 480 + 5 + 1 + 21,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct bridge_desc raspits_tc358762_bridge = {
	.desc = {
		.modes = &raspits_tc358762_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 217,
			.height = 136,
		},
	},
	.flags = MIPI_DSI_MODE_VIDEO |
		 MIPI_DSI_MODE_VIDEO_BURST | MIPI_DSI_MODE_LPM,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 1,
};

static const struct drm_display_mode raspits_tc358762_mode_5inch = {
	.clock = 26000000 / 1000,
	.hdisplay = 800,
	.hsync_start = 800 + 48,
	.hsync_end = 800 + 48 + 18,
	.htotal = 800 + 48 + 18 + 20,
	.vdisplay = 480,
	.vsync_start = 480 + 7,
	.vsync_end = 480 + 7 + 1,
	.vtotal = 480 + 7 + 1 + 21,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct bridge_desc raspits_tc358762_5inch_bridge = {
	.desc = {
		.modes = &raspits_tc358762_mode_5inch,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 217,
			.height = 136,
		},
	},
	.flags = MIPI_DSI_MODE_VIDEO |
		 MIPI_DSI_MODE_VIDEO_BURST | MIPI_DSI_MODE_LPM,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 1,
};

static const struct of_device_id dsi_of_match[] = {
	{
		.compatible = "raspits,tc358762",
		.data = &raspits_tc358762_bridge
	}, {
		.compatible = "raspits,tc358762-5inch",
		.data = &raspits_tc358762_5inch_bridge
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, dsi_of_match);

static bool of_child_node_is_present(const struct device_node *node,
				     const char *name)
{
	struct device_node *child;

	child = of_get_child_by_name(node, name);
	of_node_put(child);

	return !!child;
}

extern int rockpi_mcu_is_connected(void);
static int raspits_tc358762_dsi_probe(struct mipi_dsi_device *dsi)
{
	const struct bridge_desc *desc;
	struct bridge_desc *desc_tmp;
	const struct of_device_id *id;
	const struct panel_desc *pdesc;
	u32 bus_flags;
	u32 val;
	int err;

	id = of_match_node(dsi_of_match,dsi->dev.of_node);
	if (!id)
		return -ENODEV;

	if (!rockpi_mcu_is_connected() && timeout > 0){
		msleep(100);
		timeout--;
		return -EPROBE_DEFER;
	}

	desc_tmp = devm_kzalloc(&dsi->dev, sizeof(*desc_tmp), GFP_KERNEL);
	if (!desc_tmp) {
		return -ENOMEM;
	}

	desc_tmp->desc.modes = ((const struct bridge_desc *)id->data)->desc.modes;
	desc_tmp->flags = ((const struct bridge_desc *)id->data)->flags;
	desc_tmp->lanes = ((const struct bridge_desc *)id->data)->lanes;
	desc_tmp->format = ((const struct bridge_desc *)id->data)->format;
	desc_tmp->desc.num_modes = ((const struct bridge_desc *)id->data)->desc.num_modes;
	desc_tmp->desc.bpc = ((const struct bridge_desc *)id->data)->desc.bpc;
	desc_tmp->desc.size = ((const struct bridge_desc *)id->data)->desc.size;

	printk("find panel: %s\n", id->compatible);

	if (of_child_node_is_present(dsi->dev.of_node, "display-timings")) {
		struct drm_display_mode *mode;
		mode = devm_kzalloc(&dsi->dev, sizeof(*mode), GFP_KERNEL);
		if (!mode)
			return -ENOMEM;

		if (!of_get_drm_display_mode(dsi->dev.of_node, mode, &bus_flags,
					     OF_USE_NATIVE_MODE)) {
			desc_tmp->desc.modes = mode;
		}
	}
	desc = desc_tmp;

	if (desc) {
		dsi->mode_flags = desc->flags;
		dsi->format = desc->format;
		dsi->lanes = desc->lanes;
		pdesc = &desc->desc;
	} else {
		pdesc = NULL;
	}

	err = raspits_tc358762_mipi_probe(dsi, pdesc);

	if (err < 0)
		return err;

	if (!of_property_read_u32(dsi->dev.of_node, "dsi,flags", &val))
		dsi->mode_flags = val;

	if (!of_property_read_u32(dsi->dev.of_node, "dsi,format", &val))
		dsi->format = val;

	if (!of_property_read_u32(dsi->dev.of_node, "dsi,lanes", &val))
		dsi->lanes = val;

	return mipi_dsi_attach(dsi);
}

static void raspits_tc358762_dsi_remove(struct mipi_dsi_device *dsi)
{
	int err;

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	return raspits_tc358762_remove(&dsi->dev);
}

static void raspits_tc358762_dsi_shutdown(struct mipi_dsi_device *dsi)
{
	raspits_tc358762_shutdown(&dsi->dev);
}

static struct mipi_dsi_driver raspits_tc358762_dsi_driver = {
	.driver = {
		.name = "bridge-raspits-tc358762-dsi",
		.of_match_table = dsi_of_match,
	},
	.probe = raspits_tc358762_dsi_probe,
	.remove = raspits_tc358762_dsi_remove,
	.shutdown = raspits_tc358762_dsi_shutdown,
};

static int __init raspits_tc358762_init(void)
{
	int err;

	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI)) {
		err = mipi_dsi_driver_register(&raspits_tc358762_dsi_driver);
		if (err < 0)
			return err;
	}

	return 0;
}
module_init(raspits_tc358762_init);

static void __exit raspits_tc358762_exit(void)
{
	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI))
		mipi_dsi_driver_unregister(&raspits_tc358762_dsi_driver);
}
module_exit(raspits_tc358762_exit);

MODULE_AUTHOR("Radxa <dev@radxa.com>");
MODULE_AUTHOR("Jerry <xbl@rock-chips.com>");
MODULE_DESCRIPTION("DRM Driver for Raspberry Pi Touchscreen tc358762 Bridge");
MODULE_LICENSE("GPL and additional rights");
