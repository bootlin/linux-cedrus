/*
 * Media requests UAPI
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

#ifndef __LINUX_MEDIA_REQUEST_H
#define __LINUX_MEDIA_REQUEST_H

#ifndef __KERNEL__
#include <stdint.h>
#endif
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/version.h>

/* Only check that requests can be used, do not allocate */
#define MEDIA_REQUEST_FLAG_TEST			0x00000001

struct media_request_new {
	__u32 flags;
	__s32 fd;
} __attribute__ ((packed));

#define MEDIA_REQUEST_IOC_SUBMIT	  _IO('|',  128)
#define MEDIA_REQUEST_IOC_REINIT	  _IO('|',  129)

#endif
