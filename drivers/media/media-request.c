// SPDX-License-Identifier: GPL-2.0
/*
 * Media device request objects
 *
 * Copyright (C) 2018 Intel Corporation
 * Copyright (C) 2018, The Chromium OS Authors.  All rights reserved.
 *
 * Author: Sakari Ailus <sakari.ailus@linux.intel.com>
 */

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/string.h>

#include <media/media-device.h>
#include <media/media-request.h>

int media_request_alloc(struct media_device *mdev,
			struct media_request_alloc *alloc)
{
	return -ENOMEM;
}
