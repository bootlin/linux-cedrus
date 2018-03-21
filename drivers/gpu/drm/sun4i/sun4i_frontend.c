// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017 Free Electrons
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 */
#include <drm/drmP.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include "sun4i_drv.h"
#include "sun4i_format.h"
#include "sun4i_frontend.h"

static const u32 sun4i_frontend_vert_coef[32] = {
	0x00004000, 0x000140ff, 0x00033ffe, 0x00043ffd,
	0x00063efc, 0xff083dfc, 0x000a3bfb, 0xff0d39fb,
	0xff0f37fb, 0xff1136fa, 0xfe1433fb, 0xfe1631fb,
	0xfd192ffb, 0xfd1c2cfb, 0xfd1f29fb, 0xfc2127fc,
	0xfc2424fc, 0xfc2721fc, 0xfb291ffd, 0xfb2c1cfd,
	0xfb2f19fd, 0xfb3116fe, 0xfb3314fe, 0xfa3611ff,
	0xfb370fff, 0xfb390dff, 0xfb3b0a00, 0xfc3d08ff,
	0xfc3e0600, 0xfd3f0400, 0xfe3f0300, 0xff400100,
};

static const u32 sun4i_frontend_horz_coef[64] = {
	0x40000000, 0x00000000, 0x40fe0000, 0x0000ff03,
	0x3ffd0000, 0x0000ff05, 0x3ffc0000, 0x0000ff06,
	0x3efb0000, 0x0000ff08, 0x3dfb0000, 0x0000ff09,
	0x3bfa0000, 0x0000fe0d, 0x39fa0000, 0x0000fe0f,
	0x38fa0000, 0x0000fe10, 0x36fa0000, 0x0000fe12,
	0x33fa0000, 0x0000fd16, 0x31fa0000, 0x0000fd18,
	0x2ffa0000, 0x0000fd1a, 0x2cfa0000, 0x0000fc1e,
	0x29fa0000, 0x0000fc21, 0x27fb0000, 0x0000fb23,
	0x24fb0000, 0x0000fb26, 0x21fb0000, 0x0000fb29,
	0x1ffc0000, 0x0000fa2b, 0x1cfc0000, 0x0000fa2e,
	0x19fd0000, 0x0000fa30, 0x16fd0000, 0x0000fa33,
	0x14fd0000, 0x0000fa35, 0x11fe0000, 0x0000fa37,
	0x0ffe0000, 0x0000fa39, 0x0dfe0000, 0x0000fa3b,
	0x0afe0000, 0x0000fa3e, 0x08ff0000, 0x0000fb3e,
	0x06ff0000, 0x0000fb40, 0x05ff0000, 0x0000fc40,
	0x03ff0000, 0x0000fd41, 0x01ff0000, 0x0000fe42,
};

static void sun4i_frontend_scaler_init(struct sun4i_frontend *frontend)
{
	int i;

	for (i = 0; i < 32; i++) {
		regmap_write(frontend->regs, SUN4I_FRONTEND_CH0_HORZCOEF0_REG(i),
			     sun4i_frontend_horz_coef[2 * i]);
		regmap_write(frontend->regs, SUN4I_FRONTEND_CH1_HORZCOEF0_REG(i),
			     sun4i_frontend_horz_coef[2 * i]);
		regmap_write(frontend->regs, SUN4I_FRONTEND_CH0_HORZCOEF1_REG(i),
			     sun4i_frontend_horz_coef[2 * i + 1]);
		regmap_write(frontend->regs, SUN4I_FRONTEND_CH1_HORZCOEF1_REG(i),
			     sun4i_frontend_horz_coef[2 * i + 1]);
		regmap_write(frontend->regs, SUN4I_FRONTEND_CH0_VERTCOEF_REG(i),
			     sun4i_frontend_vert_coef[i]);
		regmap_write(frontend->regs, SUN4I_FRONTEND_CH1_VERTCOEF_REG(i),
			     sun4i_frontend_vert_coef[i]);
	}

	regmap_update_bits(frontend->regs, SUN4I_FRONTEND_FRM_CTRL_REG,
			   SUN4I_FRONTEND_FRM_CTRL_COEF_ACCESS_CTRL,
			   SUN4I_FRONTEND_FRM_CTRL_COEF_ACCESS_CTRL);
}

int sun4i_frontend_init(struct sun4i_frontend *frontend)
{
	return pm_runtime_get_sync(frontend->dev);
}
EXPORT_SYMBOL(sun4i_frontend_init);

void sun4i_frontend_exit(struct sun4i_frontend *frontend)
{
	pm_runtime_put(frontend->dev);
}
EXPORT_SYMBOL(sun4i_frontend_exit);

void sun4i_frontend_update_buffer(struct sun4i_frontend *frontend,
				  struct drm_plane *plane)
{
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	uint32_t format = fb->format->format;
	uint32_t width, height;
	uint32_t stride, offset;
	bool swap;
	dma_addr_t paddr;

	width = state->src_w >> 16;
	height = state->src_h >> 16;

	if (fb->modifier == DRM_FORMAT_MOD_ALLWINNER_MB32_TILED) {
		/*
		 * In MB32 tiled mode, the stride is defined as the distance
		 * between the start of the end line of the current tile and
		 * the start of the first line in the next vertical tile.
		 *
		 * Tiles are represented linearly in memory, thus the end line
		 * of current tile starts at: 31 * 32 (31 lines of 32 cols),
		 * the next vertical tile starts at: 32-bit-aligned-width * 32
		 * and the distance is: 32 * (32-bit-aligned-width - 31).
		 */

		stride = (fb->pitches[0] - 31) * 32;
		regmap_write(frontend->regs, SUN4I_FRONTEND_LINESTRD0_REG,
			     stride);

		/* Offset of the bottom-right point in the end tile. */
		offset = (width + (32 - 1)) & (32 - 1);
		regmap_write(frontend->regs, SUN4I_FRONTEND_TB_OFF0_REG,
			     SUN4I_FRONTEND_TB_OFF_X1(offset));

		if (drm_format_num_planes(format) > 1) {
			stride = (fb->pitches[1] - 31) * 32;
			regmap_write(frontend->regs, SUN4I_FRONTEND_LINESTRD1_REG,
				     stride);

			regmap_write(frontend->regs, SUN4I_FRONTEND_TB_OFF1_REG,
				     SUN4I_FRONTEND_TB_OFF_X1(offset));
		}

		if (drm_format_num_planes(format) > 2) {
			stride = (fb->pitches[2] - 31) * 32;
			regmap_write(frontend->regs, SUN4I_FRONTEND_LINESTRD2_REG,
				     stride);

			regmap_write(frontend->regs, SUN4I_FRONTEND_TB_OFF2_REG,
				     SUN4I_FRONTEND_TB_OFF_X1(offset));
		}
	} else {
		regmap_write(frontend->regs, SUN4I_FRONTEND_LINESTRD0_REG,
			     fb->pitches[0]);

		if (drm_format_num_planes(format) > 1)
			regmap_write(frontend->regs, SUN4I_FRONTEND_LINESTRD1_REG,
				     fb->pitches[1]);

		if (drm_format_num_planes(format) > 2)
			regmap_write(frontend->regs, SUN4I_FRONTEND_LINESTRD2_REG,
				     fb->pitches[2]);
	}

	/* Set the physical address of the buffer in memory */
	paddr = drm_fb_cma_get_gem_addr(fb, state, 0);
	paddr -= PHYS_OFFSET;
	DRM_DEBUG_DRIVER("Setting buffer #0 address to %pad\n", &paddr);
	regmap_write(frontend->regs, SUN4I_FRONTEND_BUF_ADDR0_REG, paddr);

	/* Some planar formats require chroma channel swapping by hand. */
	swap = sun4i_frontend_format_chroma_requires_swap(format);

	if (drm_format_num_planes(format) > 1) {
		paddr = drm_fb_cma_get_gem_addr(fb, state, swap ? 2 : 1);
		paddr -= PHYS_OFFSET;
		DRM_DEBUG_DRIVER("Setting buffer #1 address to %pad\n", &paddr);
		regmap_write(frontend->regs, SUN4I_FRONTEND_BUF_ADDR1_REG,
			     paddr);
	}

	if (drm_format_num_planes(format) > 2) {
		paddr = drm_fb_cma_get_gem_addr(fb, state, swap ? 1 : 2);
		paddr -= PHYS_OFFSET;
		DRM_DEBUG_DRIVER("Setting buffer #2 address to %pad\n", &paddr);
		regmap_write(frontend->regs, SUN4I_FRONTEND_BUF_ADDR2_REG,
			     paddr);
	}
}
EXPORT_SYMBOL(sun4i_frontend_update_buffer);

static int sun4i_frontend_drm_format_to_input_fmt(uint32_t fmt, u32 *val)
{
	if (sun4i_format_is_rgb(fmt))
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_FMT_RGB;
	else if (sun4i_format_is_yuv411(fmt))
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_FMT_YUV411;
	else if (sun4i_format_is_yuv420(fmt))
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_FMT_YUV420;
	else if (sun4i_format_is_yuv422(fmt))
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_FMT_YUV422;
	else if (sun4i_format_is_yuv444(fmt))
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_FMT_YUV444;
	else
		return -EINVAL;

	return 0;
}

static int sun4i_frontend_drm_format_to_input_mode(uint32_t fmt, u32 *val,
						   uint64_t modifier)
{
	bool tiled = modifier == DRM_FORMAT_MOD_ALLWINNER_MB32_TILED;

	if (tiled && !sun4i_format_supports_tiling(fmt))
		return -EINVAL;

	if (sun4i_format_is_packed(fmt))
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_MOD_PACKED;
	else if (sun4i_format_is_semiplanar(fmt))
		*val = tiled ? SUN4I_FRONTEND_INPUT_FMT_DATA_MOD_MB32_SEMIPLANAR
			     : SUN4I_FRONTEND_INPUT_FMT_DATA_MOD_SEMIPLANAR;
	else if (sun4i_format_is_planar(fmt))
		*val = tiled ? SUN4I_FRONTEND_INPUT_FMT_DATA_MOD_MB32_PLANAR
			     : SUN4I_FRONTEND_INPUT_FMT_DATA_MOD_PLANAR;
	else
		return -EINVAL;

	return 0;
}

static int sun4i_frontend_drm_format_to_input_sequence(uint32_t fmt, u32 *val)
{
	/* Planar formats have an explicit input sequence. */
	if (sun4i_format_is_planar(fmt)) {
		*val = 0;
		return 0;
	}

	switch (fmt) {
	/* RGB */
	case DRM_FORMAT_XRGB8888:
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_PS_XRGB;
		return 0;

	case DRM_FORMAT_BGRX8888:
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_PS_BGRX;
		return 0;

	/* YUV420 */
	case DRM_FORMAT_NV12:
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_PS_UV;
		return 0;

	case DRM_FORMAT_NV21:
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_PS_VU;
		return 0;

	/* YUV422 */
	case DRM_FORMAT_YUYV:
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_PS_YUYV;
		return 0;

	case DRM_FORMAT_VYUY:
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_PS_VYUY;
		return 0;

	case DRM_FORMAT_YVYU:
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_PS_YVYU;
		return 0;

	case DRM_FORMAT_UYVY:
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_PS_UYVY;
		return 0;

	case DRM_FORMAT_NV16:
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_PS_UV;
		return 0;

	case DRM_FORMAT_NV61:
		*val = SUN4I_FRONTEND_INPUT_FMT_DATA_PS_VU;
		return 0;

	default:
		return -EINVAL;
	}
}

static int sun4i_frontend_drm_format_to_output_fmt(uint32_t fmt, u32 *val)
{
	switch (fmt) {
	case DRM_FORMAT_XRGB8888:
		*val = SUN4I_FRONTEND_OUTPUT_FMT_DATA_FMT_XRGB8888;
		return 0;

	case DRM_FORMAT_BGRX8888:
		*val = SUN4I_FRONTEND_OUTPUT_FMT_DATA_FMT_BGRX8888;
		return 0;

	default:
		return -EINVAL;
	}
}

static const uint32_t sun4i_frontend_formats[] = {
	/* RGB */
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_BGRX8888,
	/* YUV444 */
	DRM_FORMAT_YUV444,
	DRM_FORMAT_YVU444,
	/* YUV422 */
	DRM_FORMAT_YUYV,
	DRM_FORMAT_YVYU,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_VYUY,
	DRM_FORMAT_NV16,
	DRM_FORMAT_NV61,
	DRM_FORMAT_YUV422,
	DRM_FORMAT_YVU422,
	/* YUV420 */
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV21,
	DRM_FORMAT_YUV420,
	DRM_FORMAT_YVU420,
	/* YUV411 */
	DRM_FORMAT_YUV411,
	DRM_FORMAT_YVU411,
};

bool sun4i_frontend_format_is_supported(uint32_t fmt, uint64_t modifier)
{
	bool found = false;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sun4i_frontend_formats); i++) {
		if (sun4i_frontend_formats[i] == fmt) {
			found = true;
			break;
		}
	}

	if (!found)
		return false;

	if (modifier == DRM_FORMAT_MOD_ALLWINNER_MB32_TILED)
		return sun4i_format_supports_tiling(fmt);

	return true;
}

bool sun4i_frontend_plane_check(struct drm_plane_state *state)
{
	struct drm_framebuffer *fb = state->fb;
	uint32_t format = fb->format->format;
	uint32_t width = state->src_w >> 16;
	uint64_t modifier = fb->modifier;
	bool supported;

	supported = sun4i_frontend_format_is_supported(format, modifier);
	if (!supported)
		return false;

	/* Width is required to be even for MB32 tiled format. */
	if (modifier == DRM_FORMAT_MOD_ALLWINNER_MB32_TILED &&
	    (width % 2) != 0) {
		DRM_DEBUG_DRIVER("MB32 tiled format requires an even width\n");
		return false;
	}

	return true;
}

bool sun4i_frontend_format_chroma_requires_swap(uint32_t fmt)
{
	switch (fmt) {
	case DRM_FORMAT_YVU444:
	case DRM_FORMAT_YVU422:
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YVU411:
		return true;

	default:
		return false;
	}
}

int sun4i_frontend_update_formats(struct sun4i_frontend *frontend,
				  struct drm_plane *plane, uint32_t out_fmt)
{
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	uint32_t format = fb->format->format;
	u32 out_fmt_val;
	u32 in_fmt_val;
	u32 in_mod_val;
	u32 in_ps_val;
	u32 bypass;
	unsigned int i;
	int ret;

	ret = sun4i_frontend_drm_format_to_input_fmt(format, &in_fmt_val);
	if (ret) {
		DRM_DEBUG_DRIVER("Invalid input format\n");
		return ret;
	}

	ret = sun4i_frontend_drm_format_to_input_mode(format, &in_mod_val,
						      fb->modifier);
	if (ret) {
		DRM_DEBUG_DRIVER("Invalid input mode\n");
		return ret;
	}

	ret = sun4i_frontend_drm_format_to_input_sequence(format, &in_ps_val);
	if (ret) {
		DRM_DEBUG_DRIVER("Invalid pixel sequence\n");
		return ret;
	}

	ret = sun4i_frontend_drm_format_to_output_fmt(out_fmt, &out_fmt_val);
	if (ret) {
		DRM_DEBUG_DRIVER("Invalid output format\n");
		return ret;
	}

	/*
	 * I have no idea what this does exactly, but it seems to be
	 * related to the scaler FIR filter phase parameters.
	 */
	regmap_write(frontend->regs, SUN4I_FRONTEND_CH0_HORZPHASE_REG, 0x400);
	regmap_write(frontend->regs, SUN4I_FRONTEND_CH1_HORZPHASE_REG, 0x400);
	regmap_write(frontend->regs, SUN4I_FRONTEND_CH0_VERTPHASE0_REG, 0x400);
	regmap_write(frontend->regs, SUN4I_FRONTEND_CH1_VERTPHASE0_REG, 0x400);
	regmap_write(frontend->regs, SUN4I_FRONTEND_CH0_VERTPHASE1_REG, 0x400);
	regmap_write(frontend->regs, SUN4I_FRONTEND_CH1_VERTPHASE1_REG, 0x400);

	if (sun4i_format_is_yuv(format) &&
	    !sun4i_format_is_yuv(out_fmt)) {
		/* Setup the CSC engine for YUV to RGB conversion. */

		for (i = 0; i < ARRAY_SIZE(sunxi_bt601_yuv2rgb_coef); i++)
			regmap_write(frontend->regs,
				     SUN4I_FRONTEND_CSC_COEF_REG(i),
				     sunxi_bt601_yuv2rgb_coef[i]);

		regmap_update_bits(frontend->regs,
				   SUN4I_FRONTEND_FRM_CTRL_REG,
				   SUN4I_FRONTEND_FRM_CTRL_COEF_RDY,
				   SUN4I_FRONTEND_FRM_CTRL_COEF_RDY);

		bypass = 0;
	} else {
		bypass = SUN4I_FRONTEND_BYPASS_CSC_EN;
	}

	regmap_update_bits(frontend->regs, SUN4I_FRONTEND_BYPASS_REG,
			   SUN4I_FRONTEND_BYPASS_CSC_EN, bypass);

	regmap_write(frontend->regs, SUN4I_FRONTEND_INPUT_FMT_REG,
		     in_mod_val | in_fmt_val | in_ps_val);

	/*
	 * TODO: It look like the A31 and A80 at least will need the
	 * bit 7 (ALPHA_EN) enabled when using a format with alpha (so
	 * ARGB8888).
	 */
	regmap_write(frontend->regs, SUN4I_FRONTEND_OUTPUT_FMT_REG,
		     out_fmt_val);

	return 0;
}
EXPORT_SYMBOL(sun4i_frontend_update_formats);

void sun4i_frontend_update_coord(struct sun4i_frontend *frontend,
				 struct drm_plane *plane)
{
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	uint32_t format = fb->format->format;
	uint32_t luma_width, luma_height;
	uint32_t chroma_width, chroma_height;

	/* Set height and width */
	DRM_DEBUG_DRIVER("Frontend crtc size W: %u H: %u\n",
			 state->crtc_w, state->crtc_h);

	luma_width = chroma_width = state->src_w >> 16;
	luma_height = chroma_height = state->src_h >> 16;

	if (sun4i_format_is_yuv411(format)) {
		chroma_width = DIV_ROUND_UP(luma_width, 4);
	} else if (sun4i_format_is_yuv420(format)) {
		chroma_width = DIV_ROUND_UP(luma_width, 2);
		chroma_height = DIV_ROUND_UP(luma_height, 2);
	} else if (sun4i_format_is_yuv422(format)) {
		chroma_width = DIV_ROUND_UP(luma_width, 2);
	}

	regmap_write(frontend->regs, SUN4I_FRONTEND_CH0_INSIZE_REG,
		     SUN4I_FRONTEND_INSIZE(luma_height, luma_width));
	regmap_write(frontend->regs, SUN4I_FRONTEND_CH0_OUTSIZE_REG,
		     SUN4I_FRONTEND_OUTSIZE(state->crtc_h, state->crtc_w));
	regmap_write(frontend->regs, SUN4I_FRONTEND_CH0_HORZFACT_REG,
		     (luma_width << 16) / state->crtc_w);
	regmap_write(frontend->regs, SUN4I_FRONTEND_CH0_VERTFACT_REG,
		     (luma_height << 16) / state->crtc_h);

	/* These also have to be specified, even for interleaved formats. */
	regmap_write(frontend->regs, SUN4I_FRONTEND_CH1_INSIZE_REG,
		     SUN4I_FRONTEND_INSIZE(chroma_height, chroma_width));
	regmap_write(frontend->regs, SUN4I_FRONTEND_CH1_OUTSIZE_REG,
		     SUN4I_FRONTEND_OUTSIZE(state->crtc_h, state->crtc_w));
	regmap_write(frontend->regs, SUN4I_FRONTEND_CH1_HORZFACT_REG,
		     (chroma_width << 16) / state->crtc_w);
	regmap_write(frontend->regs, SUN4I_FRONTEND_CH1_VERTFACT_REG,
		     (chroma_height << 16) / state->crtc_h);

	regmap_write_bits(frontend->regs, SUN4I_FRONTEND_FRM_CTRL_REG,
			  SUN4I_FRONTEND_FRM_CTRL_REG_RDY,
			  SUN4I_FRONTEND_FRM_CTRL_REG_RDY);
}
EXPORT_SYMBOL(sun4i_frontend_update_coord);

int sun4i_frontend_enable(struct sun4i_frontend *frontend)
{
	regmap_write_bits(frontend->regs, SUN4I_FRONTEND_FRM_CTRL_REG,
			  SUN4I_FRONTEND_FRM_CTRL_FRM_START,
			  SUN4I_FRONTEND_FRM_CTRL_FRM_START);

	return 0;
}
EXPORT_SYMBOL(sun4i_frontend_enable);

static struct regmap_config sun4i_frontend_regmap_config = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= 0x0a14,
};

static int sun4i_frontend_bind(struct device *dev, struct device *master,
			 void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct sun4i_frontend *frontend;
	struct drm_device *drm = data;
	struct sun4i_drv *drv = drm->dev_private;
	struct resource *res;
	void __iomem *regs;

	frontend = devm_kzalloc(dev, sizeof(*frontend), GFP_KERNEL);
	if (!frontend)
		return -ENOMEM;

	dev_set_drvdata(dev, frontend);
	frontend->dev = dev;
	frontend->node = dev->of_node;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	frontend->regs = devm_regmap_init_mmio(dev, regs,
					       &sun4i_frontend_regmap_config);
	if (IS_ERR(frontend->regs)) {
		dev_err(dev, "Couldn't create the frontend regmap\n");
		return PTR_ERR(frontend->regs);
	}

	frontend->reset = devm_reset_control_get(dev, NULL);
	if (IS_ERR(frontend->reset)) {
		dev_err(dev, "Couldn't get our reset line\n");
		return PTR_ERR(frontend->reset);
	}

	frontend->bus_clk = devm_clk_get(dev, "ahb");
	if (IS_ERR(frontend->bus_clk)) {
		dev_err(dev, "Couldn't get our bus clock\n");
		return PTR_ERR(frontend->bus_clk);
	}

	frontend->mod_clk = devm_clk_get(dev, "mod");
	if (IS_ERR(frontend->mod_clk)) {
		dev_err(dev, "Couldn't get our mod clock\n");
		return PTR_ERR(frontend->mod_clk);
	}

	frontend->ram_clk = devm_clk_get(dev, "ram");
	if (IS_ERR(frontend->ram_clk)) {
		dev_err(dev, "Couldn't get our ram clock\n");
		return PTR_ERR(frontend->ram_clk);
	}

	list_add_tail(&frontend->list, &drv->frontend_list);
	pm_runtime_enable(dev);

	return 0;
}

static void sun4i_frontend_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct sun4i_frontend *frontend = dev_get_drvdata(dev);

	list_del(&frontend->list);
	pm_runtime_force_suspend(dev);
}

static const struct component_ops sun4i_frontend_ops = {
	.bind	= sun4i_frontend_bind,
	.unbind	= sun4i_frontend_unbind,
};

static int sun4i_frontend_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &sun4i_frontend_ops);
}

static int sun4i_frontend_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &sun4i_frontend_ops);

	return 0;
}

static int sun4i_frontend_runtime_resume(struct device *dev)
{
	struct sun4i_frontend *frontend = dev_get_drvdata(dev);
	int ret;

	clk_set_rate(frontend->mod_clk, 300000000);

	clk_prepare_enable(frontend->bus_clk);
	clk_prepare_enable(frontend->mod_clk);
	clk_prepare_enable(frontend->ram_clk);

	ret = reset_control_reset(frontend->reset);
	if (ret) {
		dev_err(dev, "Couldn't reset our device\n");
		return ret;
	}

	regmap_update_bits(frontend->regs, SUN4I_FRONTEND_EN_REG,
			   SUN4I_FRONTEND_EN_EN,
			   SUN4I_FRONTEND_EN_EN);

	sun4i_frontend_scaler_init(frontend);

	return 0;
}

static int sun4i_frontend_runtime_suspend(struct device *dev)
{
	struct sun4i_frontend *frontend = dev_get_drvdata(dev);

	clk_disable_unprepare(frontend->ram_clk);
	clk_disable_unprepare(frontend->mod_clk);
	clk_disable_unprepare(frontend->bus_clk);

	reset_control_assert(frontend->reset);

	return 0;
}

static const struct dev_pm_ops sun4i_frontend_pm_ops = {
	.runtime_resume		= sun4i_frontend_runtime_resume,
	.runtime_suspend	= sun4i_frontend_runtime_suspend,
};

const struct of_device_id sun4i_frontend_of_table[] = {
	{ .compatible = "allwinner,sun8i-a33-display-frontend" },
	{ }
};
EXPORT_SYMBOL(sun4i_frontend_of_table);
MODULE_DEVICE_TABLE(of, sun4i_frontend_of_table);

static struct platform_driver sun4i_frontend_driver = {
	.probe		= sun4i_frontend_probe,
	.remove		= sun4i_frontend_remove,
	.driver		= {
		.name		= "sun4i-frontend",
		.of_match_table	= sun4i_frontend_of_table,
		.pm		= &sun4i_frontend_pm_ops,
	},
};
module_platform_driver(sun4i_frontend_driver);

MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com>");
MODULE_DESCRIPTION("Allwinner A10 Display Engine Frontend Driver");
MODULE_LICENSE("GPL");
