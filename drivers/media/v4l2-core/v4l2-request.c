/*
 * Media requests support for V4L2
 *
 * Copyright (C) 2018, The Chromium OS Authors.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/slab.h>

#include <media/v4l2-dev.h>
#include <media/v4l2-request.h>
#include <media/videobuf2-v4l2.h>

void v4l2_request_entity_init(struct v4l2_request_entity *entity,
			      const struct media_request_entity_ops *ops,
			      struct video_device *vdev)
{
	media_request_entity_init(&entity->base, MEDIA_REQUEST_ENTITY_TYPE_V4L2, ops);
	entity->vdev = vdev;
}
EXPORT_SYMBOL_GPL(v4l2_request_entity_init);

struct media_request_entity_data *
v4l2_request_entity_data_alloc(struct media_request *req,
			       struct v4l2_ctrl_handler *hdl)
{
	struct v4l2_request_entity_data *data;
	int ret;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return ERR_PTR(-ENOMEM);

	ret = v4l2_ctrl_request_init(&data->ctrls);
	if (ret) {
		kfree(data);
		return ERR_PTR(ret);
	}
	ret = v4l2_ctrl_request_clone(&data->ctrls, hdl, NULL);
	if (ret) {
		kfree(data);
		return ERR_PTR(ret);
	}

	INIT_LIST_HEAD(&data->queued_buffers);

	return &data->base;
}
EXPORT_SYMBOL_GPL(v4l2_request_entity_data_alloc);

void v4l2_request_entity_data_free(struct media_request_entity_data *_data)
{
	struct v4l2_request_entity_data *data;
	struct v4l2_vb2_request_buffer *qb, *n;

	data = to_v4l2_entity_data(_data);

	list_for_each_entry_safe(qb, n, &data->queued_buffers, node) {
		struct vb2_buffer *buf;
		dev_warn(_data->request->mgr->dev,
			 "entity data freed while buffer still queued!\n");

		/* give buffer back to user-space */
		buf = qb->queue->bufs[qb->v4l2_buf.index];
		buf->state = qb->pre_req_state;
		buf->request = NULL;

		kfree(qb);
	}

	v4l2_ctrl_handler_free(&data->ctrls);
	kfree(data);
}
EXPORT_SYMBOL_GPL(v4l2_request_entity_data_free);





static struct media_request *v4l2_request_alloc(struct media_request_mgr *mgr)
{
	struct media_request *req;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return ERR_PTR(-ENOMEM);

	req->mgr = mgr;
	req->state = MEDIA_REQUEST_STATE_IDLE;
	INIT_LIST_HEAD(&req->data);
	init_waitqueue_head(&req->complete_wait);
	mutex_init(&req->lock);

	mutex_lock(&mgr->mutex);
	list_add_tail(&req->list, &mgr->requests);
	mutex_unlock(&mgr->mutex);

	return req;
}

static void v4l2_request_free(struct media_request *req)
{
	struct media_request_mgr *mgr = req->mgr;
	struct media_request_entity_data *data, *next;

	mutex_lock(&mgr->mutex);
	list_del(&req->list);
	mutex_unlock(&mgr->mutex);

	list_for_each_entry_safe(data, next, &req->data, list) {
		list_del(&data->list);
		data->entity->ops->data_free(data);
	}

	kfree(req);
}

static bool v4l2_entity_valid(const struct media_request *req,
			      const struct media_request_entity *_entity)
{
	const struct v4l2_request_mgr *mgr;
	const struct v4l2_request_entity *entity;

	if (_entity->type != MEDIA_REQUEST_ENTITY_TYPE_V4L2)
		return false;

	entity = container_of(_entity, struct v4l2_request_entity, base);
	mgr = container_of(req->mgr, struct v4l2_request_mgr, base);

	/* Entity is valid if it is the video device that created the manager */
	return entity->vdev == mgr->vdev;
}

static int v4l2_request_submit(struct media_request *req)
{
	struct media_request_entity_data *data;

        /* Submit for each entity */
	list_for_each_entry(data, &req->data, list) {
		int ret = data->entity->ops->submit(req, data);
		/* TODO proper error handling, abort on other entities? */
		if (ret)
			return ret;
	}

	return 0;
}

const struct media_request_ops v4l2_request_ops = {
	.alloc = v4l2_request_alloc,
	.release = v4l2_request_free,
	.entity_valid = v4l2_entity_valid,
	.submit = v4l2_request_submit,
};
EXPORT_SYMBOL_GPL(v4l2_request_ops);

void v4l2_request_mgr_init(struct v4l2_request_mgr *mgr,
			  struct video_device *vdev,
			  const struct media_request_ops *ops)
{
	media_request_mgr_init(&mgr->base, &vdev->dev, ops);
	mgr->vdev = vdev;
}
EXPORT_SYMBOL_GPL(v4l2_request_mgr_init);

void v4l2_request_mgr_free(struct v4l2_request_mgr* mgr)
{
	media_request_mgr_free(&mgr->base);
}
EXPORT_SYMBOL_GPL(v4l2_request_mgr_free);
