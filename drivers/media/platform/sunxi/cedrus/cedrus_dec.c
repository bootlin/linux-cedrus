// SPDX-License-Identifier: GPL-2.0
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
 */

#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>

#include "cedrus.h"
#include "cedrus_dec.h"
#include "cedrus_hw.h"

static inline void *get_ctrl_ptr(struct cedrus_ctx *ctx,
				 enum cedrus_control_id id)
{
	struct v4l2_ctrl *ctrl = ctx->ctrls[id];

	return ctrl->p_cur.p;
}

void cedrus_device_work(struct work_struct *work)
{
	struct cedrus_ctx *ctx = container_of(work,
					      struct cedrus_ctx, run_work);
	struct cedrus_dev *dev = ctx->dev;
	struct cedrus_buffer *buffer_entry;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	unsigned long flags;

	spin_lock_irqsave(&ctx->dev->irq_lock, flags);

	if (list_empty(&ctx->src_list) || list_empty(&ctx->dst_list)) {
		v4l2_err(&dev->v4l2_dev,
			 "Empty source and/or destination buffer lists\n");
		spin_unlock_irqrestore(&ctx->dev->irq_lock, flags);
		return;
	}

	buffer_entry = list_last_entry(&ctx->src_list, struct cedrus_buffer, list);
	list_del(ctx->src_list.prev);

	src_buf = &buffer_entry->vb;
	v4l2_m2m_buf_done(src_buf, buffer_entry->state);

	buffer_entry = list_last_entry(&ctx->dst_list, struct cedrus_buffer, list);
	list_del(ctx->dst_list.prev);

	dst_buf = &buffer_entry->vb;
	v4l2_m2m_buf_done(dst_buf, buffer_entry->state);

	spin_unlock_irqrestore(&ctx->dev->irq_lock, flags);

	v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);
}

void cedrus_device_run(void *priv)
{
	struct cedrus_ctx *ctx = priv;
	struct cedrus_dev *dev = ctx->dev;
	struct cedrus_run run = { 0 };
	struct media_request *src_req;
	unsigned long flags;

	run.src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	if (!run.src) {
		v4l2_err(&dev->v4l2_dev, "No source buffer to prepare\n");
		return;
	}

	run.dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!run.dst) {
		v4l2_err(&dev->v4l2_dev, "No destination buffer to prepare\n");
		return;
	}

	/* Apply request(s) controls if needed. */
	src_req = run.src->vb2_buf.req_obj.req;

	if (src_req)
		v4l2_ctrl_request_setup(src_req, &ctx->hdl);

	ctx->job_abort = 0;

	spin_lock_irqsave(&ctx->dev->irq_lock, flags);

#define CHECK_CONTROL(ctx, ctrl)					\
	if (!ctx->ctrls[(ctrl)]) {					\
		v4l2_err(&(ctx)->dev->v4l2_dev, "Invalid " #ctrl " control\n"); \
		(ctx)->job_abort = 1;					\
		goto unlock_complete;					\
	}

	switch (ctx->vpu_src_fmt->fourcc) {
	case V4L2_PIX_FMT_MPEG2_SLICE:
		CHECK_CONTROL(ctx, CEDRUS_CTRL_DEC_MPEG2_SLICE_HEADER);
		run.mpeg2.hdr = get_ctrl_ptr(ctx, CEDRUS_CTRL_DEC_MPEG2_SLICE_HEADER);
		break;

	default:
		ctx->job_abort = 1;
	}
#undef CHECK_CONTROL

	if (!ctx->job_abort)
		dev->dec_ops[ctx->current_codec]->setup(ctx, &run);

unlock_complete:
	spin_unlock_irqrestore(&ctx->dev->irq_lock, flags);

	/* Complete request(s) controls if needed. */

	if (src_req)
		v4l2_ctrl_request_complete(src_req, &ctx->hdl);

	spin_lock_irqsave(&ctx->dev->irq_lock, flags);

	if (!ctx->job_abort) {
		dev->dec_ops[ctx->current_codec]->trigger(ctx);
	} else {
		v4l2_m2m_buf_done(run.src, VB2_BUF_STATE_ERROR);
		v4l2_m2m_buf_done(run.dst, VB2_BUF_STATE_ERROR);
	}

	spin_unlock_irqrestore(&ctx->dev->irq_lock, flags);

	if (ctx->job_abort)
		v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);
}

void cedrus_job_abort(void *priv)
{
	struct cedrus_ctx *ctx = priv;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	unsigned long flags;

	ctx->job_abort = 1;

	/*
	 * V4L2 M2M and request API cleanup is done here while hardware state
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
