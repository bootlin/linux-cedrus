/*
 * Copyright (C) 2015-2018 Free Electrons/Bootlin
 *
 * Maxime Ripard <maxime.ripard@bootlin.com>
 * Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <linux/component.h>
#include <linux/kfifo.h>
#include <linux/of_graph.h>
#include <linux/of_reserved_mem.h>

#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_of.h>

#include "sun4i_drv.h"
#include "sun4i_format.h"

const u32 sunxi_rgb2yuv_coef[12] = {
	0x00000107, 0x00000204, 0x00000064, 0x00000108,
	0x00003f69, 0x00003ed6, 0x000001c1, 0x00000808,
	0x000001c1, 0x00003e88, 0x00003fb8, 0x00000808
};

/*
 * These coefficients are taken from the A33 BSP from Allwinner.
 *
 * The formula is for each component, each coefficient being multiplied by
 * 1024 and each constant being multiplied by 16:
 * G = 1.164 * Y - 0.391 * U - 0.813 * V + 135
 * R = 1.164 * Y + 1.596 * V - 222
 * B = 1.164 * Y + 2.018 * U + 276
 *
 * This seems to be a conversion from Y[16:235] UV[16:240] to RGB[0:255],
 * following the BT601 spec.
 */
const u32 sunxi_bt601_yuv2rgb_coef[12] = {
	0x000004a7, 0x00001e6f, 0x00001cbf, 0x00000877,
	0x000004a7, 0x00000000, 0x00000662, 0x00003211,
	0x000004a7, 0x00000812, 0x00000000, 0x00002eb1,
};

bool sun4i_format_is_rgb(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_ARGB4444:
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_RGBA5551:
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return true;

	default:
		return false;
	}
}

bool sun4i_format_is_yuv(uint32_t format)
{
	return sun4i_format_is_yuv411(format) ||
	       sun4i_format_is_yuv420(format) ||
	       sun4i_format_is_yuv422(format) ||
	       sun4i_format_is_yuv444(format);
}

bool sun4i_format_is_yuv411(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_YUV411:
	case DRM_FORMAT_YVU411:
		return true;

	default:
		return false;
	}
}

bool sun4i_format_is_yuv420(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_YVU420:
		return true;

	default:
		return false;
	}
}

bool sun4i_format_is_yuv422(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_YVYU:
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_VYUY:
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV61:
	case DRM_FORMAT_YUV422:
	case DRM_FORMAT_YVU422:
		return true;

	default:
		return false;
	}
}

bool sun4i_format_is_yuv444(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_YUV444:
	case DRM_FORMAT_YVU444:
		return true;

	default:
		return false;
	}
}

bool sun4i_format_is_packed(uint32_t format)
{
	if (sun4i_format_is_rgb(format))
		return true;

	switch (format) {
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_YVYU:
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_VYUY:
		return true;

	default:
		return false;
	}
}

bool sun4i_format_is_semiplanar(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV61:
	case DRM_FORMAT_NV24:
	case DRM_FORMAT_NV42:
		return true;
	default:
		return false;
	}
}

bool sun4i_format_is_planar(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_YUV410:
	case DRM_FORMAT_YVU410:
	case DRM_FORMAT_YUV411:
	case DRM_FORMAT_YVU411:
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YUV422:
	case DRM_FORMAT_YVU422:
	case DRM_FORMAT_YUV444:
	case DRM_FORMAT_YVU444:
		return true;
	default:
		return false;
	}
}

bool sun4i_format_supports_tiling(uint32_t format)
{
	switch (format) {
	/* Semiplanar */
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV61:
	/* Planar */
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YUV422:
	case DRM_FORMAT_YVU422:
	case DRM_FORMAT_YUV411:
	case DRM_FORMAT_YVU411:
		return true;

	default:
		return false;
	}
}
