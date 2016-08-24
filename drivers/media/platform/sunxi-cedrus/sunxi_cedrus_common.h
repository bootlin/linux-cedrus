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

#ifndef SUNXI_CEDRUS_COMMON_H_
#define SUNXI_CEDRUS_COMMON_H_

#include "sunxi_cedrus_regs.h"

#include <linux/mfd/syscon.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-request.h>

#define SUNXI_CEDRUS_NAME		"sunxi-cedrus"

struct sunxi_cedrus_dev {
	struct v4l2_device	v4l2_dev;
	struct video_device	vfd;
	struct platform_device	*pdev;
	struct device		*dev;
	struct v4l2_m2m_dev	*m2m_dev;
	struct v4l2_request_mgr req_mgr;

	/* Mutex for device file */
	struct mutex		dev_mutex;
	/* Spinlock for interrupt */
	spinlock_t		irqlock;

	struct clk *mod_clk;
	struct clk *ahb_clk;
	struct clk *ram_clk;

	struct reset_control *rstc;

	struct regmap *syscon;

	char *base;

	dma_addr_t mbh_buf;
	dma_addr_t dcac_buf;
	dma_addr_t ncf_buf;

	void *mbh_buf_virt;
	void *dcac_buf_virt;
	void *ncf_buf_virt;

	unsigned int mbh_buf_size;
	unsigned int dcac_buf_size;
	unsigned int ncf_buf_size;
};

struct sunxi_cedrus_fmt {
	u32	fourcc;
	int	depth;
	u32	types;
	unsigned int num_planes;
};

struct sunxi_cedrus_ctx {
	struct v4l2_fh		fh;
	struct sunxi_cedrus_dev	*dev;

	struct sunxi_cedrus_fmt *vpu_src_fmt;
	struct v4l2_pix_format_mplane src_fmt;
	struct sunxi_cedrus_fmt *vpu_dst_fmt;
	struct v4l2_pix_format_mplane dst_fmt;

	struct v4l2_ctrl_handler hdl;
	struct v4l2_request_entity req_entity;

	struct vb2_buffer *dst_bufs[VIDEO_MAX_FRAME];

	struct v4l2_ctrl *mpeg2_frame_hdr_ctrl;
};

static inline void sunxi_cedrus_write(struct sunxi_cedrus_dev *vpu,
				      u32 val, u32 reg)
{
	writel(val, vpu->base + reg);
}

static inline u32 sunxi_cedrus_read(struct sunxi_cedrus_dev *vpu, u32 reg)
{
	return readl(vpu->base + reg);
}

#endif /* SUNXI_CEDRUS_COMMON_H_ */
