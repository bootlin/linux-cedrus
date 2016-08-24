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

#include "sunxi_cedrus_common.h"

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/of.h>

#include <linux/platform_device.h>
#include <linux/videodev2.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-dma-contig.h>

#include "sunxi_cedrus_dec.h"
#include "sunxi_cedrus_hw.h"

static int sunxi_cedrus_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sunxi_cedrus_ctx *ctx =
		container_of(ctrl->handler, struct sunxi_cedrus_ctx, hdl);

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_MPEG2_FRAME_HDR:
		/* This is kept in memory and used directly. */
		break;
	default:
		v4l2_err(&ctx->dev->v4l2_dev, "Invalid control\n");
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops sunxi_cedrus_ctrl_ops = {
	.s_ctrl = sunxi_cedrus_s_ctrl,
};

static const struct v4l2_ctrl_config sunxi_cedrus_ctrl_mpeg2_frame_hdr = {
	.ops = &sunxi_cedrus_ctrl_ops,
	.id = V4L2_CID_MPEG_VIDEO_MPEG2_FRAME_HDR,
	.elem_size = sizeof(struct v4l2_ctrl_mpeg2_frame_hdr),
};

struct media_request_entity_data *
sunxi_cedrus_entity_data_alloc(struct media_request *req,
			       struct media_request_entity *entity)
{
	struct sunxi_cedrus_ctx *ctx;

	ctx = container_of(entity, struct sunxi_cedrus_ctx, req_entity.base);
	return v4l2_request_entity_data_alloc(req, &ctx->hdl);
}

static int sunxi_cedrus_request_submit(struct media_request *req,
				       struct media_request_entity_data *_data)
{
	struct v4l2_request_entity_data *data;
	struct sunxi_cedrus_ctx *ctx;
	int rc;

	data = to_v4l2_entity_data(_data);

	ctx = container_of(_data->entity, struct sunxi_cedrus_ctx,
			   req_entity.base);

	rc = vb2_request_submit(data);
	if (rc)
		return rc;

	v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);

	return 0;
}

static const struct media_request_entity_ops sunxi_cedrus_request_entity_ops = {
	.data_alloc	= sunxi_cedrus_entity_data_alloc,
	.data_free	= v4l2_request_entity_data_free,
	.submit		= sunxi_cedrus_request_submit,
};

/*
 * File operations
 */
static int sunxi_cedrus_open(struct file *file)
{
	struct sunxi_cedrus_dev *dev = video_drvdata(file);
	struct sunxi_cedrus_ctx *ctx = NULL;
	struct v4l2_ctrl_handler *hdl;
	int rc = 0;

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		rc = -ENOMEM;
		goto open_unlock;
	}

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	ctx->dev = dev;
	hdl = &ctx->hdl;
	v4l2_ctrl_handler_init(hdl, 1);
	v4l2_request_entity_init(&ctx->req_entity,
				 &sunxi_cedrus_request_entity_ops,
				 &ctx->dev->vfd);
	ctx->fh.entity = &ctx->req_entity.base;

	ctx->mpeg2_frame_hdr_ctrl = v4l2_ctrl_new_custom(hdl,
			&sunxi_cedrus_ctrl_mpeg2_frame_hdr, NULL);

	if (hdl->error) {
		rc = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		goto open_unlock;
	}
	ctx->fh.ctrl_handler = hdl;
	v4l2_ctrl_handler_setup(hdl);

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx, &queue_init);

	if (IS_ERR(ctx->fh.m2m_ctx)) {
		rc = PTR_ERR(ctx->fh.m2m_ctx);

		v4l2_ctrl_handler_free(hdl);
		kfree(ctx);
		goto open_unlock;
	}

	v4l2_fh_add(&ctx->fh);

	dev_dbg(dev->dev, "Created instance: %p, m2m_ctx: %p\n",
		ctx, ctx->fh.m2m_ctx);

open_unlock:
	mutex_unlock(&dev->dev_mutex);
	return rc;
}

static int sunxi_cedrus_release(struct file *file)
{
	struct sunxi_cedrus_dev *dev = video_drvdata(file);
	struct sunxi_cedrus_ctx *ctx = container_of(file->private_data,
			struct sunxi_cedrus_ctx, fh);

	dev_dbg(dev->dev, "Releasing instance %p\n", ctx);

	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->hdl);
	ctx->mpeg2_frame_hdr_ctrl = NULL;
	mutex_lock(&dev->dev_mutex);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	mutex_unlock(&dev->dev_mutex);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations sunxi_cedrus_fops = {
	.owner		= THIS_MODULE,
	.open		= sunxi_cedrus_open,
	.release	= sunxi_cedrus_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static struct video_device sunxi_cedrus_viddev = {
	.name		= SUNXI_CEDRUS_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &sunxi_cedrus_fops,
	.ioctl_ops	= &sunxi_cedrus_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release_empty,
};

static struct v4l2_m2m_ops m2m_ops = {
	.device_run	= device_run,
	.job_abort	= job_abort,
};

static int sunxi_cedrus_probe(struct platform_device *pdev)
{
	struct sunxi_cedrus_dev *dev;
	struct video_device *vfd;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->dev = &pdev->dev;
	dev->pdev = pdev;

	ret = sunxi_cedrus_hw_probe(dev);
	if (ret) {
		dev_err(&pdev->dev, "sunxi_cedrus_hw_probe failed\n");
		return ret;
	}

	v4l2_request_mgr_init(&dev->req_mgr, &dev->vfd,
			      &v4l2_request_ops);

	spin_lock_init(&dev->irqlock);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		return ret;

	mutex_init(&dev->dev_mutex);

	dev->vfd = sunxi_cedrus_viddev;
	vfd = &dev->vfd;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;
	vfd->req_mgr = &dev->req_mgr.base;

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, 0);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device\n");
		goto unreg_dev;
	}

	video_set_drvdata(vfd, dev);
	snprintf(vfd->name, sizeof(vfd->name), "%s", sunxi_cedrus_viddev.name);
	v4l2_info(&dev->v4l2_dev,
		  "Device registered as /dev/video%d\n", vfd->num);

	platform_set_drvdata(pdev, dev);

	dev->m2m_dev = v4l2_m2m_init(&m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(dev->m2m_dev);
		goto err_m2m;
	}

	return 0;

err_m2m:
	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(&dev->vfd);
unreg_dev:
	v4l2_device_unregister(&dev->v4l2_dev);

	return ret;
}

static int sunxi_cedrus_remove(struct platform_device *pdev)
{
	struct sunxi_cedrus_dev *dev = platform_get_drvdata(pdev);

	v4l2_info(&dev->v4l2_dev, "Removing " SUNXI_CEDRUS_NAME);
	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(&dev->vfd);
	v4l2_device_unregister(&dev->v4l2_dev);
	v4l2_request_mgr_free(&dev->req_mgr);
	sunxi_cedrus_hw_remove(dev);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id of_sunxi_cedrus_match[] = {
	{ .compatible = "allwinner,sun4i-a10-video-engine" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_sunxi_cedrus_match);
#endif

static struct platform_driver sunxi_cedrus_driver = {
	.probe		= sunxi_cedrus_probe,
	.remove		= sunxi_cedrus_remove,
	.driver		= {
		.name	= SUNXI_CEDRUS_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(of_sunxi_cedrus_match),
	},
};
module_platform_driver(sunxi_cedrus_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Florent Revest <florent.revest@free-electrons.com>");
MODULE_DESCRIPTION("Sunxi Cedrus codec driver");
