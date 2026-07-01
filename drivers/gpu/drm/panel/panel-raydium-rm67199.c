// SPDX-License-Identifier: GPL-2.0
/*
 * Raydium RM67199 MIPI-DSI panel driver for Dongxingqiang DXQ5D4408
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

struct rm67199_cmd {
	u8 cmd;
	u8 data;
};

struct rm67199_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset;
	struct backlight_device *backlight;
	struct regulator_bulk_data supplies[3];
	enum drm_panel_orientation orientation;
	bool prepared;
	bool enabled;
};

static const struct rm67199_cmd dxq5d4408_init_cmds[] = {
	{ 0xFE, 0xA0 }, { 0x2B, 0x18 }, { 0xFE, 0x70 }, { 0x7D, 0x35 },
	{ 0x5D, 0x0A }, { 0x5A, 0xFF }, { 0x5C, 0xF6 }, { 0x52, 0x00 },
	{ 0xFE, 0xD0 }, { 0x40, 0x02 }, { 0x13, 0x40 }, { 0xFE, 0x40 },
	{ 0x05, 0x08 }, { 0x06, 0x08 }, { 0x08, 0x08 }, { 0x09, 0x08 },
	{ 0x0A, 0xCA }, { 0x0B, 0x88 }, { 0x0E, 0x06 }, { 0x20, 0x93 },
	{ 0x21, 0x93 }, { 0x24, 0x02 }, { 0x26, 0x02 }, { 0x28, 0x05 },
	{ 0x2A, 0x05 }, { 0x74, 0x2F }, { 0x75, 0x1E }, { 0xAD, 0x00 },
	{ 0xFE, 0x60 }, { 0x00, 0xCC }, { 0x01, 0x00 }, { 0x02, 0x04 },
	{ 0x03, 0x00 }, { 0x04, 0x00 }, { 0x05, 0x07 }, { 0x06, 0x00 },
	{ 0x07, 0x88 }, { 0x08, 0x00 }, { 0x09, 0xCC }, { 0x0A, 0x00 },
	{ 0x0B, 0x04 }, { 0x0C, 0x00 }, { 0x0D, 0x00 }, { 0x0E, 0x05 },
	{ 0x0F, 0x00 }, { 0x10, 0x88 }, { 0x11, 0x00 }, { 0x12, 0xCC },
	{ 0x13, 0x0F }, { 0x14, 0xFF }, { 0x15, 0x04 }, { 0x16, 0x00 },
	{ 0x17, 0x06 }, { 0x18, 0x00 }, { 0x19, 0x88 }, { 0x1A, 0x00 },
	{ 0x24, 0xCC }, { 0x25, 0x00 }, { 0x26, 0x02 }, { 0x27, 0x00 },
	{ 0x28, 0x00 }, { 0x29, 0x06 }, { 0x2A, 0x06 }, { 0x2B, 0x82 },
	{ 0x2D, 0x00 }, { 0x2F, 0xCC }, { 0x30, 0x00 }, { 0x31, 0x02 },
	{ 0x32, 0x00 }, { 0x33, 0x00 }, { 0x34, 0x07 }, { 0x35, 0x06 },
	{ 0x36, 0x82 }, { 0x37, 0x00 }, { 0x38, 0xCC }, { 0x39, 0x00 },
	{ 0x3A, 0x02 }, { 0x3B, 0x00 }, { 0x3D, 0x00 }, { 0x3F, 0x07 },
	{ 0x40, 0x00 }, { 0x41, 0x88 }, { 0x42, 0x00 }, { 0x43, 0xCC },
	{ 0x44, 0x00 }, { 0x45, 0x02 }, { 0x46, 0x00 }, { 0x47, 0x00 },
	{ 0x48, 0x06 }, { 0x49, 0x02 }, { 0x4A, 0x8A }, { 0x4B, 0x00 },
	{ 0x5F, 0xCA }, { 0x60, 0x01 }, { 0x61, 0xE8 }, { 0x62, 0x09 },
	{ 0x63, 0x00 }, { 0x64, 0x07 }, { 0x65, 0x00 }, { 0x66, 0x30 },
	{ 0x67, 0x80 }, { 0x9B, 0x03 }, { 0xA9, 0x06 }, { 0xAA, 0x07 },
	{ 0xAB, 0x02 }, { 0xAC, 0x11 }, { 0xAD, 0x10 }, { 0xAE, 0x04 },
	{ 0xAF, 0x05 }, { 0xB0, 0x10 }, { 0xB1, 0x10 }, { 0xB2, 0x10 },
	{ 0xB3, 0x10 }, { 0xB4, 0x10 }, { 0xB5, 0x10 }, { 0xB6, 0x10 },
	{ 0xB7, 0x10 }, { 0xB8, 0x10 }, { 0xB9, 0x10 }, { 0xBA, 0x05 },
	{ 0xBB, 0x04 }, { 0xBC, 0x01 }, { 0xBD, 0x00 }, { 0xBE, 0x0A },
	{ 0xBF, 0x11 }, { 0xC0, 0x10 }, { 0xFE, 0x40 }, { 0x0E, 0x06 },
	{ 0xFE, 0x50 }, { 0xDA, 0x01 }, { 0x00, 0x00 }, { 0x01, 0x00 },
	{ 0x46, 0x00 }, { 0x47, 0x00 }, { 0x88, 0x00 }, { 0x89, 0x00 },
	{ 0xBE, 0x02 }, { 0xCF, 0x24 }, { 0xD2, 0x02 }, { 0xD3, 0x18 },
	{ 0xD6, 0x02 }, { 0xD7, 0x5E }, { 0xD0, 0x02 }, { 0xD1, 0x2D },
	{ 0xD4, 0x02 }, { 0xD5, 0x21 }, { 0xD8, 0x02 }, { 0xD9, 0x69 },
	{ 0x02, 0x02 }, { 0x03, 0xF7 }, { 0x48, 0x02 }, { 0x49, 0xE7 },
	{ 0x8A, 0x03 }, { 0x8B, 0x47 }, { 0x04, 0x02 }, { 0x05, 0x43 },
	{ 0x4A, 0x02 }, { 0x4B, 0x37 }, { 0x8C, 0x02 }, { 0x8D, 0x7F },
	{ 0x06, 0x02 }, { 0x07, 0x72 }, { 0x4C, 0x02 }, { 0x4D, 0x6F },
	{ 0x8E, 0x02 }, { 0x8F, 0xB2 }, { 0x08, 0x02 }, { 0x09, 0x9E },
	{ 0x4E, 0x02 }, { 0x4F, 0x9E }, { 0x90, 0x02 }, { 0x91, 0xE1 },
	{ 0x0A, 0x02 }, { 0x0B, 0xC5 }, { 0x50, 0x02 }, { 0x51, 0xC5 },
	{ 0x92, 0x03 }, { 0x93, 0x0B }, { 0x0C, 0x02 }, { 0x0D, 0xE8 },
	{ 0x52, 0x02 }, { 0x53, 0xE8 }, { 0x94, 0x03 }, { 0x95, 0x32 },
	{ 0x0E, 0x03 }, { 0x0F, 0x15 }, { 0x58, 0x03 }, { 0x59, 0x16 },
	{ 0x96, 0x03 }, { 0x97, 0x67 }, { 0x10, 0x03 }, { 0x11, 0x41 },
	{ 0x5A, 0x03 }, { 0x5B, 0x43 }, { 0x98, 0x03 }, { 0x99, 0x9C },
	{ 0x12, 0x03 }, { 0x13, 0x63 }, { 0x5C, 0x03 }, { 0x5D, 0x66 },
	{ 0x9A, 0x03 }, { 0x9B, 0xC7 }, { 0x14, 0x03 }, { 0x15, 0x85 },
	{ 0x5E, 0x03 }, { 0x5F, 0x89 }, { 0x9C, 0x03 }, { 0x9D, 0xF2 },
	{ 0x16, 0x03 }, { 0x17, 0xC9 }, { 0x60, 0x03 }, { 0x61, 0xCE },
	{ 0x9E, 0x04 }, { 0x9F, 0x47 }, { 0x18, 0x03 }, { 0x19, 0xFF },
	{ 0x62, 0x04 }, { 0x63, 0x05 }, { 0xA4, 0x04 }, { 0xA5, 0x89 },
	{ 0x1A, 0x04 }, { 0x1B, 0x35 }, { 0x64, 0x04 }, { 0x65, 0x3C },
	{ 0xA6, 0x04 }, { 0xA7, 0xCB }, { 0x1C, 0x04 }, { 0x1D, 0x6B },
	{ 0x66, 0x04 }, { 0x67, 0x73 }, { 0xAC, 0x05 }, { 0xAD, 0x10 },
	{ 0x1E, 0x04 }, { 0x1F, 0xA0 }, { 0x68, 0x04 }, { 0x69, 0xAA },
	{ 0xAE, 0x05 }, { 0xAF, 0x55 }, { 0x20, 0x04 }, { 0x21, 0xD7 },
	{ 0x6A, 0x04 }, { 0x6B, 0xE4 }, { 0xB0, 0x05 }, { 0xB1, 0x98 },
	{ 0x22, 0x05 }, { 0x23, 0x12 }, { 0x6C, 0x05 }, { 0x6D, 0x20 },
	{ 0xB2, 0x05 }, { 0xB3, 0xE6 }, { 0x24, 0x05 }, { 0x25, 0x4F },
	{ 0x6E, 0x05 }, { 0x6F, 0x5C }, { 0xB4, 0x06 }, { 0xB5, 0x36 },
	{ 0x26, 0x05 }, { 0x27, 0x6D }, { 0x70, 0x05 }, { 0x71, 0x7A },
	{ 0xB6, 0x06 }, { 0xB7, 0x59 }, { 0x28, 0x05 }, { 0x29, 0x8B },
	{ 0x72, 0x05 }, { 0x73, 0x98 }, { 0xB8, 0x06 }, { 0xB9, 0x79 },
	{ 0x2A, 0x05 }, { 0x2B, 0xAB }, { 0x74, 0x05 }, { 0x75, 0xB6 },
	{ 0xBA, 0x06 }, { 0xBB, 0x99 }, { 0x30, 0x05 }, { 0x31, 0xCB },
	{ 0x76, 0x05 }, { 0x77, 0xD6 }, { 0xBC, 0x06 }, { 0xBD, 0xB9 },
	{ 0x32, 0x05 }, { 0x33, 0xEB }, { 0x78, 0x05 }, { 0x79, 0xF6 },
	{ 0xBE, 0x06 }, { 0xBF, 0xD9 }, { 0x34, 0x06 }, { 0x35, 0x10 },
	{ 0x7A, 0x06 }, { 0x7B, 0x1C }, { 0xC0, 0x07 }, { 0xC1, 0x01 },
	{ 0x36, 0x06 }, { 0x37, 0x36 }, { 0x7C, 0x06 }, { 0x7D, 0x42 },
	{ 0xC2, 0x07 }, { 0xC3, 0x29 }, { 0x38, 0x06 }, { 0x39, 0x58 },
	{ 0x7E, 0x06 }, { 0x7F, 0x62 }, { 0xC4, 0x07 }, { 0xC5, 0x4D },
	{ 0x3A, 0x06 }, { 0x3B, 0x69 }, { 0x80, 0x06 }, { 0x81, 0x72 },
	{ 0xC6, 0x07 }, { 0xC7, 0x5F }, { 0x40, 0x06 }, { 0x41, 0x7C },
	{ 0x82, 0x06 }, { 0x83, 0x84 }, { 0xC8, 0x07 }, { 0xC9, 0x74 },
	{ 0x42, 0x06 }, { 0x43, 0x86 }, { 0x84, 0x06 }, { 0x85, 0x8E },
	{ 0xCA, 0x07 }, { 0xCB, 0x80 }, { 0x44, 0x06 }, { 0x45, 0x87 },
	{ 0x86, 0x06 }, { 0x87, 0x91 }, { 0xCC, 0x07 }, { 0xCD, 0x7F },
	{ 0xFE, 0xA0 }, { 0x22, 0x00 }, { 0xFE, 0x00 }, { 0xC2, 0x08 },
	{ 0x35, 0x00 }, { 0x51, 0xD0 }, { 0x36, 0xC0 },
};

static const struct drm_display_mode dxq5d4408_mode = {
	.clock = 151674,
	.hdisplay = 1080,
	.hsync_start = 1080 + 28,
	.hsync_end = 1080 + 28 + 4,
	.htotal = 1080 + 28 + 4 + 36,
	.vdisplay = 1920,
	.vsync_start = 1920 + 255,
	.vsync_end = 1920 + 255 + 4,
	.vtotal = 1920 + 255 + 4 + 23,
	.width_mm = 68,
	.height_mm = 121,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const u32 rm67199_bus_formats[] = {
	MEDIA_BUS_FMT_RGB888_1X24,
};

static inline struct rm67199_panel *to_rm67199_panel(struct drm_panel *panel)
{
	return container_of(panel, struct rm67199_panel, panel);
}

static int rm67199_push_cmds(struct rm67199_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(dxq5d4408_init_cmds); i++) {
		u8 buf[] = {
			dxq5d4408_init_cmds[i].cmd,
			dxq5d4408_init_cmds[i].data,
		};

		ret = mipi_dsi_dcs_write_buffer(dsi, buf, sizeof(buf));
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int rm67199_prepare(struct drm_panel *panel)
{
	struct rm67199_panel *ctx = to_rm67199_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (ctx->prepared)
		return 0;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret)
		return ret;

	msleep(200);

	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(300);
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(200);
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(300);

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = rm67199_push_cmds(ctx);
	if (ret < 0) {
		dev_err(dev, "failed to send init commands: %d\n", ret);
		goto power_off;
	}

	ret = mipi_dsi_dcs_set_tear_scanline(ctx->dsi, 0x0000);
	if (ret < 0) {
		dev_err(dev, "failed to set tear scanline: %d\n", ret);
		goto power_off;
	}

	ret = mipi_dsi_dcs_exit_sleep_mode(ctx->dsi);
	if (ret < 0) {
		dev_err(dev, "failed to exit sleep mode: %d\n", ret);
		goto power_off;
	}

	msleep(1200);

	ret = mipi_dsi_dcs_set_display_on(ctx->dsi);
	if (ret < 0) {
		dev_err(dev, "failed to set display on: %d\n", ret);
		goto power_off;
	}

	msleep(400);

	ctx->prepared = true;

	return 0;

power_off:
	gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	return ret;
}

static int rm67199_unprepare(struct drm_panel *panel)
{
	struct rm67199_panel *ctx = to_rm67199_panel(panel);
	int ret;

	if (!ctx->prepared)
		return 0;

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(ctx->dsi);
	if (ret < 0)
		dev_err(panel->dev, "failed to set display off: %d\n", ret);

	msleep(50);

	ret = mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
	if (ret < 0)
		dev_err(panel->dev, "failed to enter sleep mode: %d\n", ret);

	msleep(150);
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(50);

	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	ctx->prepared = false;

	return 0;
}

static int rm67199_enable(struct drm_panel *panel)
{
	struct rm67199_panel *ctx = to_rm67199_panel(panel);

	if (ctx->enabled)
		return 0;

	backlight_enable(ctx->backlight);
	ctx->enabled = true;

	return 0;
}

static int rm67199_disable(struct drm_panel *panel)
{
	struct rm67199_panel *ctx = to_rm67199_panel(panel);

	if (!ctx->enabled)
		return 0;

	backlight_disable(ctx->backlight);
	ctx->enabled = false;

	return 0;
}

static int rm67199_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct rm67199_panel *ctx = to_rm67199_panel(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &dxq5d4408_mode);
	if (!mode)
		return -ENOMEM;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = dxq5d4408_mode.width_mm;
	connector->display_info.height_mm = dxq5d4408_mode.height_mm;
	connector->display_info.bus_flags = DRM_BUS_FLAG_DE_LOW |
					    DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE;
	drm_display_info_set_bus_formats(&connector->display_info,
					 rm67199_bus_formats,
					 ARRAY_SIZE(rm67199_bus_formats));
	drm_connector_set_panel_orientation(connector, ctx->orientation);

	return 1;
}

static enum drm_panel_orientation rm67199_get_orientation(struct drm_panel *panel)
{
	struct rm67199_panel *ctx = to_rm67199_panel(panel);

	return ctx->orientation;
}

static int rm67199_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct rm67199_panel *ctx = mipi_dsi_get_drvdata(dsi);

	if (!ctx->prepared)
		return 0;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	return mipi_dsi_dcs_set_display_brightness(dsi, bl->props.brightness);
}

static const struct backlight_ops rm67199_bl_ops = {
	.update_status = rm67199_bl_update_status,
};

static const struct drm_panel_funcs rm67199_panel_funcs = {
	.prepare = rm67199_prepare,
	.unprepare = rm67199_unprepare,
	.enable = rm67199_enable,
	.disable = rm67199_disable,
	.get_modes = rm67199_get_modes,
	.get_orientation = rm67199_get_orientation,
};

static int rm67199_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct rm67199_panel *ctx;
	struct backlight_properties bl_props;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO |
			  MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_VIDEO_HSE |
			  MIPI_DSI_MODE_NO_EOT_PACKET;

	ctx->supplies[0].supply = "vbat";
	ctx->supplies[1].supply = "vddio";
	ctx->supplies[2].supply = "vci";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret)
		return ret;

	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset),
				     "failed to get reset GPIO\n");

	ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret < 0)
		return ret;

	memset(&bl_props, 0, sizeof(bl_props));
	bl_props.type = BACKLIGHT_RAW;
	bl_props.brightness = 255;
	bl_props.max_brightness = 255;

	ctx->backlight = devm_backlight_device_register(dev, dev_name(dev),
							dev, dsi,
							&rm67199_bl_ops,
							&bl_props);
	if (IS_ERR(ctx->backlight))
		return PTR_ERR(ctx->backlight);

	drm_panel_init(&ctx->panel, dev, &rm67199_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void rm67199_remove(struct mipi_dsi_device *dsi)
{
	struct rm67199_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static void rm67199_shutdown(struct mipi_dsi_device *dsi)
{
	struct rm67199_panel *ctx = mipi_dsi_get_drvdata(dsi);

	rm67199_disable(&ctx->panel);
	rm67199_unprepare(&ctx->panel);
}

static const struct of_device_id rm67199_of_match[] = {
	{ .compatible = "dongxingqiang,dxq5d4408" },
	{ .compatible = "raydium,rm67199-dxq5d4408" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rm67199_of_match);

static struct mipi_dsi_driver rm67199_driver = {
	.probe = rm67199_probe,
	.remove = rm67199_remove,
	.shutdown = rm67199_shutdown,
	.driver = {
		.name = "panel-raydium-rm67199",
		.of_match_table = rm67199_of_match,
	},
};
module_mipi_dsi_driver(rm67199_driver);

MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("DRM driver for Dongxingqiang DXQ5D4408 RM67199 MIPI DSI panel");
MODULE_LICENSE("GPL v2");
