/*
 * Sunxi-Cedrus VPU driver
 *
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 *
 * Based on the vim2m driver, that is:
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

#ifndef _SUNXI_CEDRUS_DEC_H_
#define _SUNXI_CEDRUS_DEC_H_

extern const struct v4l2_ioctl_ops sunxi_cedrus_ioctl_ops;

void sunxi_cedrus_device_work(struct work_struct *work);
void sunxi_cedrus_device_run(void *priv);
void sunxi_cedrus_job_abort(void *priv);
void sunxi_cedrus_request_complete(struct media_request *req);

int sunxi_cedrus_queue_init(void *priv, struct vb2_queue *src_vq,
			    struct vb2_queue *dst_vq);

#endif
