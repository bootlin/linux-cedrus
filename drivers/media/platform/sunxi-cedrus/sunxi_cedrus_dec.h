/*
 * Sunxi Cedrus codec driver
 *
 * Copyright (C) 2016 Florent Revest
 * Florent Revest <florent.revest@free-electrons.com>
 *
 * Based on vim2m
 *
 * Copyright (c) 2009-2010 Samsung Electronics Co., Ltd.
 * Pawel Osciak, <pawel@osciak.com>
 * Marek Szyprowski, <m.szyprowski@samsung.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef SUNXI_CEDRUS_DEC_H_
#define SUNXI_CEDRUS_DEC_H_

int queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq);

void job_abort(void *priv);
void device_run(void *priv);

extern const struct v4l2_ioctl_ops sunxi_cedrus_ioctl_ops;

#endif /* SUNXI_CEDRUS_DEC_H_ */
