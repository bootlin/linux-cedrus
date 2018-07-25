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

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>

#include "cedrus.h"
#include "cedrus_dec.h"
#include "cedrus_hw.h"

void cedrus_device_run(void *priv)
{
	struct cedrus_ctx *ctx = priv;
	struct cedrus_dev *dev = ctx->dev;
	struct cedrus_run run = { 0 };
	struct media_request *src_req;
	unsigned long flags;

	run.src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	run.dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	/* Apply request(s) controls if needed. */
	src_req = run.src->vb2_buf.req_obj.req;

	if (src_req)
		v4l2_ctrl_request_setup(src_req, &ctx->hdl);

	ctx->job_abort = 0;

	spin_lock_irqsave(&ctx->dev->irq_lock, flags);

	switch (ctx->src_fmt.pixelformat) {
	case V4L2_PIX_FMT_MPEG2_SLICE:
		run.mpeg2.slice_params = cedrus_find_control_data(ctx,
			V4L2_CID_MPEG_VIDEO_MPEG2_SLICE_PARAMS);
		run.mpeg2.quantization = cedrus_find_control_data(ctx,
			V4L2_CID_MPEG_VIDEO_MPEG2_QUANTIZATION);
		break;

	case V4L2_PIX_FMT_H264_SLICE:
		run.h264.decode_param = cedrus_find_control_data(ctx,
			V4L2_CID_MPEG_VIDEO_H264_DECODE_PARAMS);
		run.h264.pps = cedrus_find_control_data(ctx,
			V4L2_CID_MPEG_VIDEO_H264_PPS);
		run.h264.scaling_matrix = cedrus_find_control_data(ctx,
			V4L2_CID_MPEG_VIDEO_H264_SCALING_MATRIX);
		run.h264.slice_param = cedrus_find_control_data(ctx,
			V4L2_CID_MPEG_VIDEO_H264_SLICE_PARAMS);
		run.h264.scaling_matrix = cedrus_find_control_data(ctx,
			V4L2_CID_MPEG_VIDEO_H264_SCALING_MATRIX);
		run.h264.sps = cedrus_find_control_data(ctx,
			V4L2_CID_MPEG_VIDEO_H264_SPS);
		break;

	default:
		ctx->job_abort = 1;
	}

	if (!ctx->job_abort)
		dev->dec_ops[ctx->current_codec]->setup(ctx, &run);

	spin_unlock_irqrestore(&ctx->dev->irq_lock, flags);

	/* Complete request(s) controls if needed. */

	if (src_req)
		v4l2_ctrl_request_complete(src_req, &ctx->hdl);

	spin_lock_irqsave(&ctx->dev->irq_lock, flags);

	if (!ctx->job_abort) {
		dev->dec_ops[ctx->current_codec]->trigger(ctx);
	} else {
		v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		v4l2_m2m_buf_done(run.src, VB2_BUF_STATE_ERROR);

		v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
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
