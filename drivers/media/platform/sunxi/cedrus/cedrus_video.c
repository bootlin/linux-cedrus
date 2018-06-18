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

/* Flags that indicate a format can be used for capture/output. */
#define CEDRUS_CAPTURE	BIT(0)
#define CEDRUS_OUTPUT	BIT(1)

#define CEDRUS_MIN_WIDTH	16U
#define CEDRUS_MIN_HEIGHT	16U
#define CEDRUS_MAX_WIDTH	3840U
#define CEDRUS_MAX_HEIGHT	2160U

static struct cedrus_fmt formats[] = {
	{
		.fourcc = V4L2_PIX_FMT_MB32_NV12,
		.types	= CEDRUS_CAPTURE,
		.depth = 2,
		.num_planes = 2,
	},
	{
		.fourcc = V4L2_PIX_FMT_MPEG2_SLICE,
		.types	= CEDRUS_OUTPUT,
		.num_planes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE,
		.types	= CEDRUS_OUTPUT,
		.num_planes = 1,
	},
};

#define NUM_FORMATS ARRAY_SIZE(formats)

static struct cedrus_fmt *find_format(struct v4l2_format *f)
{
	struct cedrus_fmt *fmt;
	unsigned int k;

	for (k = 0; k < NUM_FORMATS; k++) {
		fmt = &formats[k];
		if (fmt->fourcc == f->fmt.pix_mp.pixelformat)
			break;
	}

	if (k == NUM_FORMATS)
		return NULL;

	return &formats[k];
}

static inline struct cedrus_ctx *file2ctx(struct file *file)
{
	return container_of(file->private_data, struct cedrus_ctx, fh);
}

static int vidioc_querycap(struct file *file, void *priv,
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

static int enum_fmt(struct v4l2_fmtdesc *f, u32 type)
{
	struct cedrus_fmt *fmt;
	int i, num = 0;

	for (i = 0; i < NUM_FORMATS; ++i) {
		if (formats[i].types & type) {
			/* index-th format of type type found ? */
			if (num == f->index)
				break;
			/*
			 * Correct type but haven't reached our index yet,
			 * just increment per-type index
			 */
			++num;
		}
	}

	if (i < NUM_FORMATS) {
		fmt = &formats[i];
		f->pixelformat = fmt->fourcc;
		return 0;
	}

	return -EINVAL;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	return enum_fmt(f, CEDRUS_CAPTURE);
}

static int vidioc_enum_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	return enum_fmt(f, CEDRUS_OUTPUT);
}

static int vidioc_g_fmt(struct cedrus_ctx *ctx, struct v4l2_format *f)
{
	struct cedrus_dev *dev = ctx->dev;

	switch (f->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		f->fmt.pix_mp = ctx->dst_fmt;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		f->fmt.pix_mp = ctx->src_fmt;
		break;
	default:
		v4l2_err(&dev->v4l2_dev,
			 "Invalid buffer type for getting format\n");
		return -EINVAL;
	}

	return 0;
}

static int vidioc_g_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	return vidioc_g_fmt(file2ctx(file), f);
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	return vidioc_g_fmt(file2ctx(file), f);
}

static int vidioc_try_fmt(struct v4l2_format *f, struct cedrus_fmt *fmt)
{
	int i;
	__u32 bpl;

	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	f->fmt.pix_mp.num_planes = fmt->num_planes;

	switch (f->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		if (f->fmt.pix_mp.plane_fmt[0].sizeimage == 0)
			return -EINVAL;

		f->fmt.pix_mp.plane_fmt[0].bytesperline = 0;
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		/* Limit to hardware min/max. */
		f->fmt.pix_mp.width = clamp(f->fmt.pix_mp.width,
					    CEDRUS_MIN_WIDTH, CEDRUS_MAX_WIDTH);
		f->fmt.pix_mp.height = clamp(f->fmt.pix_mp.height,
					     CEDRUS_MIN_HEIGHT,
					     CEDRUS_MAX_HEIGHT);

		for (i = 0; i < f->fmt.pix_mp.num_planes; ++i) {
			bpl = (f->fmt.pix_mp.width * fmt->depth) >> 3;
			f->fmt.pix_mp.plane_fmt[i].bytesperline = bpl;
			f->fmt.pix_mp.plane_fmt[i].sizeimage =
				f->fmt.pix_mp.height * bpl;
		}
		break;
	}
	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct cedrus_fmt *fmt;
	struct cedrus_ctx *ctx = file2ctx(file);
	struct cedrus_dev *dev = ctx->dev;

	fmt = find_format(f);
	if (!fmt) {
		f->fmt.pix_mp.pixelformat = formats[0].fourcc;
		fmt = find_format(f);
	}
	if (!(fmt->types & CEDRUS_CAPTURE)) {
		v4l2_err(&dev->v4l2_dev, "Invalid destination format: %08x\n",
			 f->fmt.pix_mp.pixelformat);
		return -EINVAL;
	}

	return vidioc_try_fmt(f, fmt);
}

static int vidioc_try_fmt_vid_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct cedrus_fmt *fmt;
	struct cedrus_ctx *ctx = file2ctx(file);
	struct cedrus_dev *dev = ctx->dev;

	fmt = find_format(f);
	if (!fmt) {
		f->fmt.pix_mp.pixelformat = formats[0].fourcc;
		fmt = find_format(f);
	}
	if (!(fmt->types & CEDRUS_OUTPUT)) {
		v4l2_err(&dev->v4l2_dev, "Invalid source format: %08x\n",
			 f->fmt.pix_mp.pixelformat);
		return -EINVAL;
	}

	return vidioc_try_fmt(f, fmt);
}

static int vidioc_s_fmt(struct cedrus_ctx *ctx, struct v4l2_format *f)
{
	struct cedrus_dev *dev = ctx->dev;
	struct v4l2_pix_format_mplane *pix_fmt_mp = &f->fmt.pix_mp;
	struct cedrus_fmt *fmt;
	int i, ret = 0;

	switch (f->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		ctx->vpu_src_fmt = find_format(f);
		ctx->src_fmt = *pix_fmt_mp;
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		fmt = find_format(f);
		ctx->vpu_dst_fmt = fmt;

		for (i = 0; i < fmt->num_planes; ++i) {
			pix_fmt_mp->plane_fmt[i].bytesperline =
				pix_fmt_mp->width * fmt->depth;
			pix_fmt_mp->plane_fmt[i].sizeimage =
				pix_fmt_mp->plane_fmt[i].bytesperline
				* pix_fmt_mp->height;
		}
		ctx->dst_fmt = *pix_fmt_mp;
		break;
	default:
		v4l2_err(&dev->v4l2_dev,
			 "Invalid buffer type for setting format\n");
		return -EINVAL;
	}

	return ret;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct cedrus_ctx *ctx = file2ctx(file);
	int ret;

	ret = vidioc_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	return vidioc_s_fmt(ctx, f);
}

static int vidioc_s_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct cedrus_ctx *ctx = file2ctx(file);
	int ret;

	ret = vidioc_try_fmt_vid_out(file, priv, f);
	if (ret)
		return ret;

	return vidioc_s_fmt(ctx, f);
}

const struct v4l2_ioctl_ops cedrus_ioctl_ops = {
	.vidioc_querycap		= vidioc_querycap,

	.vidioc_enum_fmt_vid_cap	= vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap_mplane	= vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap_mplane	= vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap_mplane	= vidioc_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_out_mplane = vidioc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out_mplane	= vidioc_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out_mplane	= vidioc_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out_mplane	= vidioc_s_fmt_vid_out,

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

	switch (vq->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		*nplanes = ctx->vpu_src_fmt->num_planes;

		sizes[0] = ctx->src_fmt.plane_fmt[0].sizeimage;
		break;

	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		*nplanes = ctx->vpu_dst_fmt->num_planes;

		sizes[0] = round_up(ctx->dst_fmt.plane_fmt[0].sizeimage, 8);
		sizes[1] = sizes[0];
		break;

	default:
		v4l2_err(&dev->v4l2_dev,
			 "Invalid buffer type for queue setup\n");
		return -EINVAL;
	}

	return 0;
}

static int cedrus_buf_init(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct cedrus_ctx *ctx = container_of(vq->drv_priv,
					      struct cedrus_ctx, fh);

	if (vq->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		ctx->dst_bufs[vb->index] = vb;

	return 0;
}

static void cedrus_buf_cleanup(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct cedrus_ctx *ctx = container_of(vq->drv_priv,
					      struct cedrus_ctx, fh);

	if (vq->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		ctx->dst_bufs[vb->index] = NULL;
}

static int cedrus_buf_prepare(struct vb2_buffer *vb)
{
	struct cedrus_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct cedrus_dev *dev = ctx->dev;
	struct vb2_queue *vq = vb->vb2_queue;
	int i;

	switch (vq->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		if (vb2_plane_size(vb, 0)
		    < ctx->src_fmt.plane_fmt[0].sizeimage) {
			v4l2_err(&dev->v4l2_dev,
				 "Buffer plane size too small for output\n");
			return -EINVAL;
		}
		break;

	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		for (i = 0; i < ctx->vpu_dst_fmt->num_planes; ++i) {
			if (vb2_plane_size(vb, i)
			    < ctx->dst_fmt.plane_fmt[i].sizeimage) {
				v4l2_err(&dev->v4l2_dev,
					 "Buffer plane %d size too small for capture\n",
					 i);
				break;
			}
		}

		if (i != ctx->vpu_dst_fmt->num_planes)
			return -EINVAL;
		break;

	default:
		v4l2_err(&dev->v4l2_dev,
			 "Invalid buffer type for buffer preparation\n");
		return -EINVAL;
	}

	return 0;
}

static int cedrus_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct cedrus_ctx *ctx = vb2_get_drv_priv(q);
	struct cedrus_dev *dev = ctx->dev;
	int ret = 0;

	switch (ctx->vpu_src_fmt->fourcc) {
	case V4L2_PIX_FMT_H264_SLICE:
		ctx->current_codec = CEDRUS_CODEC_H264;
		break;
	case V4L2_PIX_FMT_MPEG2_SLICE:
		ctx->current_codec = CEDRUS_CODEC_MPEG2;
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

		if (vbuf == NULL)
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
