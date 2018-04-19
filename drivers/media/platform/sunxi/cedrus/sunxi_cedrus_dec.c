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

#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>

#include "sunxi_cedrus_common.h"
#include "sunxi_cedrus_mpeg2.h"
#include "sunxi_cedrus_dec.h"
#include "sunxi_cedrus_hw.h"

void sunxi_cedrus_device_work(struct work_struct *work)
{
	struct sunxi_cedrus_ctx *ctx = container_of(work,
			struct sunxi_cedrus_ctx, run_work);
	struct sunxi_cedrus_buffer *buffer_entry;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	unsigned long flags;

	spin_lock_irqsave(&ctx->dev->irq_lock, flags);

	if (list_empty(&ctx->src_list) ||
	    list_empty(&ctx->dst_list)) {
		pr_err("Empty source and/or destination buffers lists\n");
		spin_unlock_irqrestore(&ctx->dev->irq_lock, flags);
		return;
	}

	buffer_entry = list_last_entry(&ctx->src_list, struct sunxi_cedrus_buffer, list);
	list_del(ctx->src_list.prev);

	src_buf = &buffer_entry->vb;
	v4l2_m2m_buf_done(src_buf, buffer_entry->state);

	buffer_entry = list_last_entry(&ctx->dst_list, struct sunxi_cedrus_buffer, list);
	list_del(ctx->dst_list.prev);

	dst_buf = &buffer_entry->vb;
	v4l2_m2m_buf_done(dst_buf, buffer_entry->state);

	spin_unlock_irqrestore(&ctx->dev->irq_lock, flags);

	v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);
}

void sunxi_cedrus_device_run(void *priv)
{
	struct sunxi_cedrus_ctx *ctx = priv;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct media_request *src_req, *dst_req;
	dma_addr_t src_buf_addr, dst_luma_addr, dst_chroma_addr;
	unsigned long flags;
	struct v4l2_ctrl_mpeg2_frame_hdr *mpeg2_frame_hdr;
	bool mpeg1 = false;

	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	if (!src_buf) {
		v4l2_err(&ctx->dev->v4l2_dev,
			 "No source buffer to prepare\n");
		return;
	}

	dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!dst_buf) {
		v4l2_err(&ctx->dev->v4l2_dev,
			 "No destination buffer to prepare\n");
		return;
	}

	src_buf_addr = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	dst_luma_addr = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	dst_chroma_addr = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 1);
	if (!src_buf || !dst_luma_addr || !dst_chroma_addr) {
		v4l2_err(&ctx->dev->v4l2_dev,
			 "Acquiring kernel pointers to buffers failed\n");
		return;
	}

	/* Apply request(s) controls if needed. */

	src_req = src_buf->vb2_buf.req_obj.req;
	dst_req = dst_buf->vb2_buf.req_obj.req;

	if (src_req) {
		if (src_req->state != MEDIA_REQUEST_STATE_QUEUED) {
			v4l2_err(&ctx->dev->v4l2_dev,
				 "Unexpected state for request %s\n",
				 src_req->debug_str);
			return;
		}

		v4l2_ctrl_request_setup(src_req, &ctx->hdl);
	}

	if (dst_req && dst_req != src_req) {
		if (dst_req->state != MEDIA_REQUEST_STATE_QUEUED) {
			v4l2_err(&ctx->dev->v4l2_dev,
				 "Unexpected state for request %s\n",
				 dst_req->debug_str);
			return;
		}

		v4l2_ctrl_request_setup(dst_req, &ctx->hdl);
	}

	dst_buf->vb2_buf.timestamp = src_buf->vb2_buf.timestamp;
	if (src_buf->flags & V4L2_BUF_FLAG_TIMECODE)
		dst_buf->timecode = src_buf->timecode;
	dst_buf->field = src_buf->field;
	dst_buf->flags = src_buf->flags & (V4L2_BUF_FLAG_TIMECODE |
		 V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
		 V4L2_BUF_FLAG_BFRAME   | V4L2_BUF_FLAG_TSTAMP_SRC_MASK);

	ctx->job_abort = 0;

	spin_lock_irqsave(&ctx->dev->irq_lock, flags);

	if (ctx->vpu_src_fmt->fourcc == V4L2_PIX_FMT_MPEG2_FRAME) {
		if (!ctx->mpeg2_frame_hdr_ctrl) {
			v4l2_err(&ctx->dev->v4l2_dev,
				 "Invalid MPEG2 frame header control\n");
			ctx->job_abort = 1;
			goto unlock_complete;
		}

		mpeg2_frame_hdr = (struct v4l2_ctrl_mpeg2_frame_hdr *)
				ctx->mpeg2_frame_hdr_ctrl->p_new.p;

		sunxi_cedrus_mpeg2_setup(ctx, src_buf_addr, dst_luma_addr,
					 dst_chroma_addr, mpeg2_frame_hdr);

		mpeg1 = mpeg2_frame_hdr->type == MPEG1;
	} else {
		ctx->job_abort = 1;
	}

unlock_complete:
	spin_unlock_irqrestore(&ctx->dev->irq_lock, flags);

	/* Complete request(s) controls if needed. */

	if (src_req)
		v4l2_ctrl_request_complete(src_req, &ctx->hdl);

	if (dst_req && dst_req != src_req)
		v4l2_ctrl_request_complete(dst_req, &ctx->hdl);

	spin_lock_irqsave(&ctx->dev->irq_lock, flags);

	if (!ctx->job_abort) {
		if (ctx->vpu_src_fmt->fourcc == V4L2_PIX_FMT_MPEG2_FRAME)
			sunxi_cedrus_mpeg2_trigger(ctx, mpeg1);
	} else {
		v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_ERROR);
		v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_ERROR);
	}

	spin_unlock_irqrestore(&ctx->dev->irq_lock, flags);

	if (ctx->job_abort)
		v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);
}

void sunxi_cedrus_job_abort(void *priv)
{
	struct sunxi_cedrus_ctx *ctx = priv;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	unsigned long flags;

	ctx->job_abort = 1;

	/*
	 * V4L2 m2m and request API cleanup is done here while hardware state
	 * cleanup is done in the interrupt context. Doing all the cleanup in
	 * the interrupt context is a bit risky, since the job_abort call might
	 * originate from the release hook, where interrupts have already been
	 * disabled.
	 */

	spin_lock_irqsave(&ctx->dev->irq_lock, flags);

	src_buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	if (src_buf)
		v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_ERROR);

	dst_buf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	if (dst_buf)
		v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_ERROR);

	spin_unlock_irqrestore(&ctx->dev->irq_lock, flags);

	v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);
}

void sunxi_cedrus_request_complete(struct media_request *req)
{
	struct sunxi_cedrus_ctx *ctx;

	ctx = (struct sunxi_cedrus_ctx *)
			vb2_core_request_find_buffer_priv(req);
	if (ctx == NULL)
		return;

	v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
}
