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

#ifndef _SUNXI_CEDRUS_MPEG2_H_
#define _SUNXI_CEDRUS_MPEG2_H_

void sunxi_cedrus_mpeg2_setup(struct sunxi_cedrus_ctx *ctx,
			      dma_addr_t src_buf_addr,
			      dma_addr_t dst_luma_addr,
			      dma_addr_t dst_chroma_addr,
			      struct v4l2_ctrl_mpeg2_frame_hdr *frame_hdr);
void sunxi_cedrus_mpeg2_trigger(struct sunxi_cedrus_ctx *ctx, bool mpeg1);

#endif
