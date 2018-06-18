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

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of.h>

#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mem2mem.h>

#include "cedrus.h"
#include "cedrus_video.h"
#include "cedrus_dec.h"
#include "cedrus_hw.h"

static int cedrus_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct cedrus_ctx *ctx =
		container_of(ctrl->handler, struct cedrus_ctx, hdl);
	struct cedrus_dev *dev = ctx->dev;

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_H264_DECODE_PARAM:
	case V4L2_CID_MPEG_VIDEO_H264_SCALING_MATRIX:
	case V4L2_CID_MPEG_VIDEO_H264_SLICE_PARAM:
	case V4L2_CID_MPEG_VIDEO_H264_SPS:
	case V4L2_CID_MPEG_VIDEO_H264_PPS:
	case V4L2_CID_MPEG_VIDEO_MPEG2_SLICE_HEADER:
		/* This is kept in memory and used directly. */
		break;
	default:
		v4l2_err(&dev->v4l2_dev, "Invalid control to set\n");
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops cedrus_ctrl_ops = {
	.s_ctrl = cedrus_s_ctrl,
};

static const struct cedrus_control controls[] = {
	[CEDRUS_CTRL_DEC_H264_DECODE_PARAM] = {
		.id		= V4L2_CID_MPEG_VIDEO_H264_DECODE_PARAM,
		.elem_size	= sizeof(struct v4l2_ctrl_h264_decode_param),
	},
	[CEDRUS_CTRL_DEC_H264_SCALING_MATRIX] = {
		.id		= V4L2_CID_MPEG_VIDEO_H264_SCALING_MATRIX,
		.elem_size	= sizeof(struct v4l2_ctrl_h264_scaling_matrix),
	},
	[CEDRUS_CTRL_DEC_H264_SLICE_PARAM] = {
		.id		= V4L2_CID_MPEG_VIDEO_H264_SLICE_PARAM,
		.elem_size	= sizeof(struct v4l2_ctrl_h264_slice_param),
	},
	[CEDRUS_CTRL_DEC_H264_SPS] = {
		.id		= V4L2_CID_MPEG_VIDEO_H264_SPS,
		.elem_size	= sizeof(struct v4l2_ctrl_h264_sps),
	},
	[CEDRUS_CTRL_DEC_H264_PPS] = {
		.id		= V4L2_CID_MPEG_VIDEO_H264_PPS,
		.elem_size	= sizeof(struct v4l2_ctrl_h264_pps),
	},
	[CEDRUS_CTRL_DEC_MPEG2_SLICE_HEADER] = {
		.id		= V4L2_CID_MPEG_VIDEO_MPEG2_SLICE_HEADER,
		.elem_size	= sizeof(struct v4l2_ctrl_mpeg2_slice_header),
	},
};

static int cedrus_init_ctrls(struct cedrus_dev *dev, struct cedrus_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->hdl;
	unsigned int num_ctrls = ARRAY_SIZE(controls);
	unsigned int i;

	v4l2_ctrl_handler_init(hdl, num_ctrls);
	if (hdl->error) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to initialize control handler\n");
		return hdl->error;
	}

	for (i = 0; i < num_ctrls; i++) {
		struct v4l2_ctrl_config cfg = { 0 };

		cfg.ops = &cedrus_ctrl_ops;
		cfg.elem_size = controls[i].elem_size;
		cfg.id = controls[i].id;

		ctx->ctrls[i] = v4l2_ctrl_new_custom(hdl, &cfg, NULL);
		if (hdl->error) {
			v4l2_err(&dev->v4l2_dev,
				 "Failed to create new custom control\n");

			v4l2_ctrl_handler_free(hdl);
			return hdl->error;
		}
	}

	ctx->fh.ctrl_handler = hdl;
	v4l2_ctrl_handler_setup(hdl);

	return 0;
}

static int cedrus_open(struct file *file)
{
	struct cedrus_dev *dev = video_drvdata(file);
	struct cedrus_ctx *ctx = NULL;
	int ret;

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		mutex_unlock(&dev->dev_mutex);
		return -ENOMEM;
	}

	INIT_WORK(&ctx->run_work, cedrus_device_work);

	INIT_LIST_HEAD(&ctx->src_list);
	INIT_LIST_HEAD(&ctx->dst_list);

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	ctx->dev = dev;

	ret = cedrus_init_ctrls(dev, ctx);
	if (ret)
		goto err_free;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx,
					    &cedrus_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_ctrls;
	}

	v4l2_fh_add(&ctx->fh);

	mutex_unlock(&dev->dev_mutex);

	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&ctx->hdl);
err_free:
	kfree(ctx);
	mutex_unlock(&dev->dev_mutex);

	return ret;
}

static int cedrus_release(struct file *file)
{
	struct cedrus_dev *dev = video_drvdata(file);
	struct cedrus_ctx *ctx = container_of(file->private_data,
					      struct cedrus_ctx, fh);

	mutex_lock(&dev->dev_mutex);

	v4l2_fh_del(&ctx->fh);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);

	v4l2_ctrl_handler_free(&ctx->hdl);

	v4l2_fh_exit(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);

	kfree(ctx);

	mutex_unlock(&dev->dev_mutex);

	return 0;
}

static const struct v4l2_file_operations cedrus_fops = {
	.owner		= THIS_MODULE,
	.open		= cedrus_open,
	.release	= cedrus_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct video_device cedrus_video_device = {
	.name		= CEDRUS_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &cedrus_fops,
	.ioctl_ops	= &cedrus_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release_empty,
};

static const struct v4l2_m2m_ops cedrus_m2m_ops = {
	.device_run	= cedrus_device_run,
	.job_abort	= cedrus_job_abort,
};

static const struct media_device_ops cedrus_m2m_media_ops = {
	.req_validate = vb2_request_validate,
	.req_queue = vb2_m2m_request_queue,
};

static int cedrus_probe(struct platform_device *pdev)
{
	struct cedrus_dev *dev;
	struct video_device *vfd;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->dev = &pdev->dev;
	dev->pdev = pdev;

	ret = cedrus_hw_probe(dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to probe hardware\n");
		return ret;
	}

	mutex_init(&dev->dev_mutex);
	spin_lock_init(&dev->irq_lock);

	dev->vfd = cedrus_video_device;
	vfd = &dev->vfd;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;

	dev->mdev.dev = &pdev->dev;
	strlcpy(dev->mdev.model, CEDRUS_NAME, sizeof(dev->mdev.model));

	media_device_init(&dev->mdev);
	dev->mdev.ops = &cedrus_m2m_media_ops;
	dev->v4l2_dev.mdev = &dev->mdev;
	dev->pad[0].flags = MEDIA_PAD_FL_SINK;
	dev->pad[1].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&vfd->entity, 2, dev->pad);
	if (ret) {
		dev_err(&pdev->dev, "Failed to initialize media entity pads\n");
		return ret;
	}

	dev->dec_ops[CEDRUS_CODEC_H264] = &cedrus_dec_ops_h264;
	dev->dec_ops[CEDRUS_CODEC_MPEG2] = &cedrus_dec_ops_mpeg2;

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register V4L2 device\n");
		return ret;
	}

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, 0);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device\n");
		goto err_v4l2;
	}

	video_set_drvdata(vfd, dev);
	snprintf(vfd->name, sizeof(vfd->name), "%s", cedrus_video_device.name);

	v4l2_info(&dev->v4l2_dev,
		  "Device registered as /dev/video%d\n", vfd->num);

	platform_set_drvdata(pdev, dev);

	dev->m2m_dev = v4l2_m2m_init(&cedrus_m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to initialize V4L2 M2M device\n");
		ret = PTR_ERR(dev->m2m_dev);
		goto err_video;
	}

	ret = media_device_register(&dev->mdev);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register media device\n");
		goto err_m2m;
	}

	return 0;

err_m2m:
	v4l2_m2m_release(dev->m2m_dev);
err_video:
	video_unregister_device(&dev->vfd);
err_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);

	return ret;
}

static int cedrus_remove(struct platform_device *pdev)
{
	struct cedrus_dev *dev = platform_get_drvdata(pdev);

	v4l2_info(&dev->v4l2_dev, "Removing " CEDRUS_NAME);

	if (media_devnode_is_registered(dev->mdev.devnode)) {
		media_device_unregister(&dev->mdev);
		media_device_cleanup(&dev->mdev);
	}

	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(&dev->vfd);
	v4l2_device_unregister(&dev->v4l2_dev);
	cedrus_hw_remove(dev);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id of_cedrus_match[] = {
	{ .compatible = "allwinner,sun4i-a10-video-engine" },
	{ .compatible = "allwinner,sun5i-a13-video-engine" },
	{ .compatible = "allwinner,sun7i-a20-video-engine" },
	{ .compatible = "allwinner,sun8i-a33-video-engine" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_cedrus_match);
#endif

static struct platform_driver cedrus_driver = {
	.probe		= cedrus_probe,
	.remove		= cedrus_remove,
	.driver		= {
		.name	= CEDRUS_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(of_cedrus_match),
	},
};
module_platform_driver(cedrus_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Florent Revest <florent.revest@free-electrons.com>");
MODULE_DESCRIPTION("Sunxi-Cedrus VPU driver");
