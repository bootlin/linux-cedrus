/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/* sun4i_drm.h
 *
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#ifndef _UAPI_SUN4I_DRM_H_
#define _UAPI_SUN4I_DRM_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct drm_sun4i_gem_create_tiled {
	__u32 height;
	__u32 width;
	__u32 format;
	/* handle, offsets, pitches, size will be returned */
	__u32 handle;
	__u32 pitches[4];
	__u32 offsets[4];
	__u64 size;
};

#define DRM_SUN4I_GEM_CREATE_TILED	0x00

#define DRM_IOCTL_SUN4I_GEM_CREATE_TILED \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_SUN4I_GEM_CREATE_TILED, \
		 struct drm_sun4i_gem_create_tiled)

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI_SUN4I_DRM_H_ */
