// SPDX-License-Identifier: GPL-2.0
/*
 * Media device request objects
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Author: Sakari Ailus <sakari.ailus@linux.intel.com>
 */

#ifndef MEDIA_REQUEST_H
#define MEDIA_REQUEST_H

#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <media/media-device.h>

enum media_request_state {
	MEDIA_REQUEST_STATE_IDLE,
	MEDIA_REQUEST_STATE_QUEUEING,
	MEDIA_REQUEST_STATE_QUEUED,
	MEDIA_REQUEST_STATE_COMPLETE,
	MEDIA_REQUEST_STATE_CLEANING,
};

struct media_request_object;

/**
 * struct media_request - Media device request
 * @mdev: Media device this request belongs to
 * @kref: Reference count
 * @debug_prefix: Prefix for debug messages (process name:fd)
 * @state: The state of the request
 * @objects: List of @struct media_request_object request objects
 * @num_objects: The number objects in the request
 * @num_completed_objects: The number of completed objects in the request
 * @poll_wait: Wait queue for poll
 * @lock: Serializes access to this struct
 */
struct media_request {
	struct media_device *mdev;
	struct kref kref;
	char debug_str[TASK_COMM_LEN + 11];
	enum media_request_state state;
	struct list_head objects;
	unsigned int num_incomplete_objects;
	struct wait_queue_head poll_wait;
	spinlock_t lock;
};

#ifdef CONFIG_MEDIA_CONTROLLER

static inline void media_request_get(struct media_request *req)
{
	kref_get(&req->kref);
}

void media_request_put(struct media_request *req);
void media_request_cancel(struct media_request *req);

int media_request_alloc(struct media_device *mdev,
			struct media_request_alloc *alloc);
#else
static inline void media_request_get(struct media_request *req)
{
}

static inline void media_request_put(struct media_request *req)
{
}

static inline void media_request_cancel(struct media_request *req)
{
}

#endif

struct media_request_object_ops {
	int (*prepare)(struct media_request_object *object);
	void (*unprepare)(struct media_request_object *object);
	void (*queue)(struct media_request_object *object);
	void (*unbind)(struct media_request_object *object);
	void (*cancel)(struct media_request_object *object);
	void (*release)(struct media_request_object *object);
};

/**
 * struct media_request_object - An opaque object that belongs to a media
 *				 request
 *
 * @priv: object's priv pointer
 * @list: List entry of the object for @struct media_request
 * @kref: Reference count of the object, acquire before releasing req->lock
 *
 * An object related to the request. This struct is embedded in the
 * larger object data.
 */
struct media_request_object {
	const struct media_request_object_ops *ops;
	void *priv;
	struct media_request *req;
	struct list_head list;
	struct kref kref;
	bool completed;
};

#ifdef CONFIG_MEDIA_CONTROLLER
static inline void media_request_object_get(struct media_request_object *obj)
{
	kref_get(&obj->kref);
}

/**
 * media_request_object_put - Put a media request object
 *
 * @obj: The object
 *
 * Put a media request object. Once all references are gone, the
 * object's memory is released.
 */
void media_request_object_put(struct media_request_object *obj);

/**
 * media_request_object_init - Initialise a media request object
 *
 * Initialise a media request object. The object will be released using the
 * release callback of the ops once it has no references (this function
 * initialises references to one).
 */
void media_request_object_init(struct media_request_object *obj);

/**
 * media_request_object_bind - Bind a media request object to a request
 */
void media_request_object_bind(struct media_request *req,
			       const struct media_request_object_ops *ops,
			       void *priv,
			       struct media_request_object *obj);

void media_request_object_unbind(struct media_request_object *obj);

/**
 * media_request_object_complete - Mark the media request object as complete
 */
void media_request_object_complete(struct media_request_object *obj);
#else
static inline void media_request_object_get(struct media_request_object *obj)
{
}

static inline void media_request_object_put(struct media_request_object *obj)
{
}

static inline void media_request_object_init(struct media_request_object *obj)
{
	obj->ops = NULL;
	obj->req = NULL;
}

static inline void media_request_object_bind(struct media_request *req,
			       const struct media_request_object_ops *ops,
			       void *priv,
			       struct media_request_object *obj)
{
}

static inline void media_request_object_unbind(struct media_request_object *obj)
{
}

static inline void media_request_object_complete(struct media_request_object *obj)
{
}
#endif

#endif
