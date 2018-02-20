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

#ifndef _MEDIA_V4L2_REQUEST_H
#define _MEDIA_V4L2_REQUEST_H

#include <linux/kconfig.h>
#include <media/media-request.h>

#if IS_ENABLED(CONFIG_MEDIA_REQUEST_API)

#include <linux/list.h>

#include <media/videobuf2-core.h>
#include <media/v4l2-ctrls.h>

/**
 * struct v4l2_request_entity - entity used with V4L2 instances
 *
 * @base:	base media_request_entity struct
 * @vdev:	video device that this entity represents
 *
 * This structure is used by V4L2 devices that support being controlled
 * by requests. If should be added to the device-specific structure that the
 * driver wishes to control using requests.
 *
 * V4L2 request entities are able to receive queued buffers using vb2 queues,
 * and control settings using the control framework.
 *
 */
struct v4l2_request_entity {
	struct media_request_entity base;
	struct video_device *vdev;
};
#define to_v4l2_entity(e) container_of(e, struct v4l2_request_entity, base)

/**
 * v4l2_request_entity_init() - initialize a struct v4l2_request_entity
 *
 * @entity:	entity to initialize
 * @ops:	entity ops to use
 * @vdev:	video device represented by this entity
 */
void v4l2_request_entity_init(struct v4l2_request_entity *entity,
			      const struct media_request_entity_ops *ops,
			      struct video_device *vdev);

/**
 * struct v4l2_vb2_request_buffer - record buffer queue on behalf of a request
 *
 * @queue:		vb2 queue
 * @pre_req_state:	keep track of the pre-QBUF state of the buffer
 * @v4l2_buf:		user-space buffer queue ioctl data
 * @node:		entry into v4l2_request_entity_data::queued_buffers
 */
struct v4l2_vb2_request_buffer {
	struct vb2_queue *queue;
	enum vb2_buffer_state pre_req_state;
	struct v4l2_buffer v4l2_buf;
	struct list_head node;
};

/**
 * struct v4l2_request_entity_data - per-request data for V4L2 entities
 *
 * @base:		base entity data structure
 * @ctrls:		record of controls set for this request
 * @queued_buffers:	record of buffers queued for this request
 */
struct v4l2_request_entity_data {
	struct media_request_entity_data base;
	struct v4l2_ctrl_handler ctrls;
	struct list_head queued_buffers;
};
static inline struct v4l2_request_entity_data *
to_v4l2_entity_data(struct media_request_entity_data *data)
{
	if (IS_ERR(data))
		return (struct v4l2_request_entity_data *)data;

	return container_of(data, struct v4l2_request_entity_data, base);
}

/**
 * v4l2_request_entity_data_alloc() - allocate data for a V4L2 entity
 *
 * @req:	request to allocate for
 * @hdl:	control handler of the device we will be controlling
 *
 * Helper function to be used from the media_request_entity_ops::data_alloc
 * hook.
 */
struct media_request_entity_data *
v4l2_request_entity_data_alloc(struct media_request *req,
			       struct v4l2_ctrl_handler *hdl);

/**
 * v4l2_request_entity_data_free() - free per-request data of an entity
 *
 * @data:	entity data to free
 *
 * Helper function to be usedfrom the media_request_entity_ops::data_free
 * hook.
 */
void
v4l2_request_entity_data_free(struct media_request_entity_data *_data);





/**
 * struct v4l2_request_mgr - request manager producing requests suitable
 *			     for managing single v4l2 devices.
 *
 * @base:	base manager structure
 * @vdev:	device that our requests can control
 */
struct v4l2_request_mgr {
	struct media_request_mgr base;
	struct video_device *vdev;
};

/**
 * v4l2_request_mgr_init() - initialize a v4l2_request_mgr
 *
 * @mgr:	manager to initialize
 * @vdev:	video device that our instances will control
 * @ops:	used to override ops if needed. &v4l2_request_ops is a good
 *		default
 */
void v4l2_request_mgr_init(struct v4l2_request_mgr *mgr,
			  struct video_device *vdev,
			  const struct media_request_ops *ops);

/**
 * v4l2_request_mgr_free() - free a v4l2 request manager
 *
 * @mgr:	manager to free
 */
void v4l2_request_mgr_free(struct v4l2_request_mgr *mgr);

extern const struct media_request_ops v4l2_request_ops;

#endif /* CONFIG_MEDIA_REQUEST_API */

#endif
