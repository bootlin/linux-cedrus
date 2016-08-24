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

#ifndef SUNXI_CEDRUS_HW_H_
#define SUNXI_CEDRUS_HW_H_

struct sunxi_cedrus_dev;
struct sunxi_cedrus_ctx;

int sunxi_cedrus_hw_probe(struct sunxi_cedrus_dev *vpu);
void sunxi_cedrus_hw_remove(struct sunxi_cedrus_dev *vpu);

void process_mpeg2(struct sunxi_cedrus_ctx *ctx, dma_addr_t in_buf,
		   dma_addr_t out_luma, dma_addr_t out_chroma,
		   struct v4l2_ctrl_mpeg2_frame_hdr *frame_hdr);

#endif /* SUNXI_CEDRUS_HW_H_ */
