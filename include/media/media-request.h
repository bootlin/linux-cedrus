// SPDX-License-Identifier: GPL-2.0
/*
 * Media device request objects
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Author: Sakari Ailus <sakari.ailus@linux.intel.com>
 */

#ifndef MEDIA_REQUEST_H
#define MEDIA_REQUEST_H

#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <media/media-device.h>

int media_request_alloc(struct media_device *mdev,
			struct media_request_alloc *alloc);

#endif
