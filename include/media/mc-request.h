/*
 * Media requests support for media controller
 *
 * Copyright (C) 2018, The Chromium OS Authors.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _MEDIA_MC_REQUEST_H
#define _MEDIA_MC_REQUEST_H

#include <linux/kconfig.h>
#include <media/media-request.h>

#if IS_ENABLED(CONFIG_MEDIA_REQUEST_API)

struct mc_request_entity {
	struct media_request_entity base;
	struct media_entity *entity;
};

#else  /* CONFIG_MEDIA_REQUEST_API */

#endif  /* CONFIG_MEDIA_REQUEST_API */

#endif
