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

#ifndef _SUN4I_FORMAT_H_
#define _SUN4I_FORMAT_H_

extern const u32 sunxi_rgb2yuv_coef[12];
extern const u32 sunxi_bt601_yuv2rgb_coef[12];

bool sun4i_format_is_rgb(uint32_t format);
bool sun4i_format_is_yuv(uint32_t format);
bool sun4i_format_is_yuv411(uint32_t format);
bool sun4i_format_is_yuv420(uint32_t format);
bool sun4i_format_is_yuv422(uint32_t format);
bool sun4i_format_is_yuv444(uint32_t format);
bool sun4i_format_is_packed(uint32_t format);
bool sun4i_format_is_semiplanar(uint32_t format);
bool sun4i_format_is_planar(uint32_t format);
bool sun4i_format_supports_tiling(uint32_t format);

static inline bool sun4i_format_is_packed_yuv422(uint32_t format)
{
	return sun4i_format_is_packed(format) && sun4i_format_is_yuv422(format);
}

#endif /* _SUN4I_FORMAT_H_ */
