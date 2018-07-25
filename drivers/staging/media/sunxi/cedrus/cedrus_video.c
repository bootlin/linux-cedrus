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
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>

#include "cedrus.h"
#include "cedrus_video.h"
#include "cedrus_dec.h"
#include "cedrus_hw.h"

#define CEDRUS_DECODE_SRC	BIT(0)
#define CEDRUS_DECODE_DST	BIT(1)

#define CEDRUS_MIN_WIDTH	16U
#define CEDRUS_MIN_HEIGHT	16U
#define CEDRUS_MAX_WIDTH	3840U
#define CEDRUS_MAX_HEIGHT	2160U

static struct cedrus_format cedrus_formats[] = {
	{
		.pixelformat	= V4L2_PIX_FMT_MPEG2_SLICE,
		.directions	= CEDRUS_DECODE_SRC,
		.num_planes	= 1,
		.num_buffers	= 1,
	},
	{
		.pixelformat	= V4L2_PIX_FMT_H264_SLICE,
		.directions	= CEDRUS_DECODE_SRC,
		.num_planes	= 1,
		.num_buffers	= 1,
	},
	{
		.pixelformat	= V4L2_PIX_FMT_MB32_NV12,
		.directions	= CEDRUS_DECODE_DST,
		.num_planes	= 2,
		.num_buffers	= 1,
	},
	{
		.pixelformat	= V4L2_PIX_FMT_NV12,
		.directions	= CEDRUS_DECODE_DST,
		.num_planes	= 2,
		.num_buffers	= 1,
		.capabilities	= CEDRUS_CAPABILITY_UNTILED,
	},
};

#define CEDRUS_FORMATS_COUNT	ARRAY_SIZE(cedrus_formats)

static inline struct cedrus_ctx *cedrus_file2ctx(struct file *file)
{
	return container_of(file->private_data, struct cedrus_ctx, fh);
}

static struct cedrus_format *cedrus_find_format(u32 pixelformat, u32 directions,
						unsigned int capabilities)
{
	struct cedrus_format *fmt;
	unsigned int i;

	for (i = 0; i < CEDRUS_FORMATS_COUNT; i++) {
		fmt = &cedrus_formats[i];

		if (fmt->capabilities && (fmt->capabilities & capabilities) !=
		    fmt->capabilities)
			continue;

		if (fmt->pixelformat == pixelformat &&
		    (fmt->directions & directions) != 0)
			break;
	}

	if (i == CEDRUS_FORMATS_COUNT)
		return NULL;

	return &cedrus_formats[i];
}

static void cedrus_prepare_plane_format(struct cedrus_format *fmt,
					struct v4l2_format *f,
					unsigned int i)
{
	struct v4l2_plane_pix_format *plane_fmt = &f->fmt.pix_mp.plane_fmt[i];
	unsigned int width = f->fmt.pix_mp.width;
	unsigned int height = f->fmt.pix_mp.height;
	unsigned int sizeimage = plane_fmt->sizeimage;
	unsigned int bytesperline = plane_fmt->bytesperline;

	switch (fmt->pixelformat) {
	case V4L2_PIX_FMT_MPEG2_SLICE:
	case V4L2_PIX_FMT_H264_SLICE:
		/* Zero bytes per line. */
		bytesperline = 0;
		break;

	case V4L2_PIX_FMT_MB32_NV12:
		/* 32-aligned stride. */
		bytesperline = ALIGN(width, 32);

		/* 32-aligned (luma) height. */
		height = ALIGN(height, 32);

		if (i == 0)
			/* 32-aligned luma size. */
			sizeimage = bytesperline * height;
		else if (i == 1)
			/* 32-aligned chroma size with 2x2 sub-sampling. */
			sizeimage = bytesperline * ALIGN(height / 2, 32);

		break;

	case V4L2_PIX_FMT_NV12:
		/* 32-aligned stride. */
		bytesperline = ALIGN(width, 32);

		if (i == 0)
			/* Regular luma size. */
			sizeimage = bytesperline * height;
		else if (i == 1)
			/* Regular chroma size with 2x2 sub-sampling. */
			sizeimage = bytesperline * height / 2;

		break;
	}

	f->fmt.pix_mp.width = width;
	f->fmt.pix_mp.height = height;

	plane_fmt->bytesperline = bytesperline;
	plane_fmt->sizeimage = sizeimage;
}

static void cedrus_prepare_format(struct cedrus_format *fmt,
				  struct v4l2_format *f)
{
	unsigned int i;

	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	f->fmt.pix_mp.num_planes = fmt->num_planes;

	for (i = 0; i < fmt->num_planes; i++)
		cedrus_prepare_plane_format(fmt, f, i);
}

static int cedrus_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strncpy(cap->driver, CEDRUS_NAME, sizeof(cap->driver) - 1);
	strncpy(cap->card, CEDRUS_NAME, sizeof(cap->card) - 1);
	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:%s", CEDRUS_NAME);

	cap->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int cedrus_enum_fmt(struct file *file, struct v4l2_fmtdesc *f,
			   u32 direction)
{
	struct cedrus_ctx *ctx = cedrus_file2ctx(file);
	struct cedrus_dev *dev = ctx->dev;
	unsigned int capabilities = dev->capabilities;
	struct cedrus_format *fmt;
	unsigned int i, index;

	/* Index among formats that match the requested direction. */
	index = 0;

	for (i = 0; i < CEDRUS_FORMATS_COUNT; i++) {
		fmt = &cedrus_formats[i];

		if (fmt->capabilities && (fmt->capabilities & capabilities) !=
		    fmt->capabilities)
			continue;

		if (!(cedrus_formats[i].directions & direction))
			continue;

		if (index == f->index)
			break;

		index++;
	}

	/* Matched format. */
	if (i < CEDRUS_FORMATS_COUNT) {
		f->pixelformat = cedrus_formats[i].pixelformat;

		return 0;
	}

	return -EINVAL;
}

static int cedrus_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	return cedrus_enum_fmt(file, f, CEDRUS_DECODE_DST);
}

static int cedrus_enum_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	return cedrus_enum_fmt(file, f, CEDRUS_DECODE_SRC);
}

static int cedrus_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct cedrus_ctx *ctx = cedrus_file2ctx(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;

	f->fmt.pix_mp = ctx->dst_fmt;

	return 0;
}

static int cedrus_g_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct cedrus_ctx *ctx = cedrus_file2ctx(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	f->fmt.pix_mp = ctx->src_fmt;

	return 0;
}

static int cedrus_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct cedrus_ctx *ctx = cedrus_file2ctx(file);
	struct cedrus_dev *dev = ctx->dev;
	struct cedrus_format *fmt;

	fmt = cedrus_find_format(f->fmt.pix_mp.pixelformat, CEDRUS_DECODE_DST,
				 dev->capabilities);
	if (!fmt)
		return -EINVAL;

	cedrus_prepare_format(fmt, f);

	/* Limit to hardware min/max. */
	f->fmt.pix_mp.width = clamp(f->fmt.pix_mp.width, CEDRUS_MIN_WIDTH,
				    CEDRUS_MAX_WIDTH);
	f->fmt.pix_mp.height = clamp(f->fmt.pix_mp.height, CEDRUS_MIN_HEIGHT,
				     CEDRUS_MAX_HEIGHT);

	return 0;
}

static int cedrus_try_fmt_vid_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct cedrus_ctx *ctx = cedrus_file2ctx(file);
	struct cedrus_dev *dev = ctx->dev;
	struct cedrus_format *fmt;
	struct v4l2_plane_pix_format *plane_fmt;
	unsigned int i;

	fmt = cedrus_find_format(f->fmt.pix_mp.pixelformat, CEDRUS_DECODE_SRC,
				 dev->capabilities);
	if (!fmt)
		return -EINVAL;

	cedrus_prepare_format(fmt, f);

	for (i = 0; i < f->fmt.pix_mp.num_planes; i++) {
		plane_fmt = &f->fmt.pix_mp.plane_fmt[i];

		/* Source image size has to be given by userspace. */
		if (plane_fmt->sizeimage == 0)
			return -EINVAL;
	}

	return 0;
}

static int cedrus_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct cedrus_ctx *ctx = cedrus_file2ctx(file);
	struct cedrus_dev *dev = ctx->dev;
	int ret;

	ret = cedrus_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	ctx->dst_fmt = f->fmt.pix_mp;

	cedrus_dst_format_set(dev, &ctx->dst_fmt);

	return 0;
}

static int cedrus_s_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct cedrus_ctx *ctx = cedrus_file2ctx(file);
	int ret;

	ret = cedrus_try_fmt_vid_out(file, priv, f);
	if (ret)
		return ret;

	ctx->src_fmt = f->fmt.pix_mp;

	return 0;
}

const struct v4l2_ioctl_ops cedrus_ioctl_ops = {
	.vidioc_querycap		= cedrus_querycap,

	.vidioc_enum_fmt_vid_cap_mplane	= cedrus_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap_mplane	= cedrus_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap_mplane	= cedrus_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap_mplane	= cedrus_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_out_mplane = cedrus_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out_mplane	= cedrus_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out_mplane	= cedrus_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out_mplane	= cedrus_s_fmt_vid_out,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf		= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,

	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

static int cedrus_queue_setup(struct vb2_queue *vq, unsigned int *nbufs,
			      unsigned int *nplanes, unsigned int sizes[],
			      struct device *alloc_devs[])
{
	struct cedrus_ctx *ctx = vb2_get_drv_priv(vq);
	struct cedrus_dev *dev = ctx->dev;
	struct v4l2_pix_format_mplane *mplane_fmt;
	struct cedrus_format *fmt;
	unsigned int i;

	switch (vq->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		mplane_fmt = &ctx->src_fmt;
		fmt = cedrus_find_format(mplane_fmt->pixelformat,
					 CEDRUS_DECODE_SRC,
					 dev->capabilities);
		break;

	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		mplane_fmt = &ctx->dst_fmt;
		fmt = cedrus_find_format(mplane_fmt->pixelformat,
					 CEDRUS_DECODE_DST,
					 dev->capabilities);
		break;

	default:
		return -EINVAL;
	}

	if (!fmt)
		return -EINVAL;

	if (fmt->num_buffers == 1) {
		sizes[0] = 0;

		for (i = 0; i < fmt->num_planes; i++)
			sizes[0] += mplane_fmt->plane_fmt[i].sizeimage;
	} else if (fmt->num_buffers == fmt->num_planes) {
		for (i = 0; i < fmt->num_planes; i++)
			sizes[i] = mplane_fmt->plane_fmt[i].sizeimage;
	} else {
		return -EINVAL;
	}

	*nplanes = fmt->num_buffers;

	return 0;
}

static int cedrus_buf_init(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct cedrus_ctx *ctx = vb2_get_drv_priv(vq);

	if (vq->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		ctx->dst_bufs[vb->index] = vb;

	return 0;
}

static void cedrus_buf_cleanup(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct cedrus_ctx *ctx = vb2_get_drv_priv(vq);

	if (vq->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		ctx->dst_bufs[vb->index] = NULL;
}

static int cedrus_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct cedrus_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *fmt;
	unsigned int buffer_size = 0;
	unsigned int format_size = 0;
	unsigned int i;

	if (vq->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		fmt = &ctx->src_fmt;
	else if (vq->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		fmt = &ctx->dst_fmt;
	else
		return -EINVAL;

	for (i = 0; i < vb->num_planes; i++)
		buffer_size += vb2_plane_size(vb, i);

	for (i = 0; i < fmt->num_planes; i++)
		format_size += fmt->plane_fmt[i].sizeimage;

	if (buffer_size < format_size)
		return -EINVAL;

	return 0;
}

static int cedrus_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct cedrus_ctx *ctx = vb2_get_drv_priv(q);
	struct cedrus_dev *dev = ctx->dev;
	int ret = 0;

	switch (ctx->src_fmt.pixelformat) {
	case V4L2_PIX_FMT_MPEG2_SLICE:
		ctx->current_codec = CEDRUS_CODEC_MPEG2;
		break;
	case V4L2_PIX_FMT_H264_SLICE:
		ctx->current_codec = CEDRUS_CODEC_H264;
		break;
	default:
		return -EINVAL;
	}

	if (V4L2_TYPE_IS_OUTPUT(q->type) &&
	    dev->dec_ops[ctx->current_codec]->start)
		ret = dev->dec_ops[ctx->current_codec]->start(ctx);

	return ret;
}

static void cedrus_stop_streaming(struct vb2_queue *q)
{
	struct cedrus_ctx *ctx = vb2_get_drv_priv(q);
	struct cedrus_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *vbuf;
	unsigned long flags;

	flush_scheduled_work();

	if (V4L2_TYPE_IS_OUTPUT(q->type) &&
	    dev->dec_ops[ctx->current_codec]->stop)
		dev->dec_ops[ctx->current_codec]->stop(ctx);

	for (;;) {
		spin_lock_irqsave(&ctx->dev->irq_lock, flags);

		if (V4L2_TYPE_IS_OUTPUT(q->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		spin_unlock_irqrestore(&ctx->dev->irq_lock, flags);

		if (!vbuf)
			return;

		v4l2_ctrl_request_complete(vbuf->vb2_buf.req_obj.req,
					   &ctx->hdl);
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

static void cedrus_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct cedrus_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void cedrus_buf_request_complete(struct vb2_buffer *vb)
{
	struct cedrus_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->hdl);
}

static struct vb2_ops cedrus_qops = {
	.queue_setup		= cedrus_queue_setup,
	.buf_prepare		= cedrus_buf_prepare,
	.buf_init		= cedrus_buf_init,
	.buf_cleanup		= cedrus_buf_cleanup,
	.buf_queue		= cedrus_buf_queue,
	.buf_request_complete	= cedrus_buf_request_complete,
	.start_streaming	= cedrus_start_streaming,
	.stop_streaming		= cedrus_stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

int cedrus_queue_init(void *priv, struct vb2_queue *src_vq,
		      struct vb2_queue *dst_vq)
{
	struct cedrus_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct cedrus_buffer);
	src_vq->allow_zero_bytesused = 1;
	src_vq->min_buffers_needed = 1;
	src_vq->ops = &cedrus_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->dev_mutex;
	src_vq->dev = ctx->dev->dev;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct cedrus_buffer);
	dst_vq->allow_zero_bytesused = 1;
	dst_vq->min_buffers_needed = 1;
	dst_vq->ops = &cedrus_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->dev_mutex;
	dst_vq->dev = ctx->dev->dev;

	return vb2_queue_init(dst_vq);
}
