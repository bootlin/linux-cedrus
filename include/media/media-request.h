/*
 * Media requests.
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

#ifndef _MEDIA_REQUEST_H
#define _MEDIA_REQUEST_H

struct media_request;
struct media_request_entity;
struct media_request_entity_data;
struct media_request_mgr;
struct media_request_new;

#include <linux/kconfig.h>

enum media_request_state {
	MEDIA_REQUEST_STATE_IDLE,
	MEDIA_REQUEST_STATE_SUBMITTED,
	MEDIA_REQUEST_STATE_COMPLETED,
	MEDIA_REQUEST_STATE_INVALID,
};

#if IS_ENABLED(CONFIG_MEDIA_REQUEST_API)

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <linux/wait.h>

/**
 * struct media_request_entity_ops - request entity operations
 *
 * @data_alloc:	allocate memory to store that entity's relevant state
 * @data_free:	free state previously allocated with data_alloc
 * @submit:	perform all required actions to be ready to process that request
 */
struct media_request_entity_ops {
	struct media_request_entity_data *
		(*data_alloc)(struct media_request *req,
			      struct media_request_entity *entity);
	void (*data_free)(struct media_request_entity_data *data);
	int (*submit)(struct media_request *req,
		      struct media_request_entity_data *data);
};

/**
 * enum media_request_entity_type - describe type of an entity
 *
 * This type lets us know the upper kind of a struct media_request_entity
 * instance.
 *
 * @MEDIA_REQUEST_ENTITY_TYPE_V4L2:	instance can be upcasted to
 * 					v4l2_request_entity
 * @MEDIA_REQUEST_ENTITY_TYPE_MC:	instance can be updated to
 * 					mc_request_entity
 *
 */
enum media_request_entity_type {
	MEDIA_REQUEST_ENTITY_TYPE_V4L2,
	MEDIA_REQUEST_ENTITY_TYPE_MC,
};

/**
 * struct media_request_entity - request entity base structure
 *
 * @type:	type of entity, indicating which upcast is safe to perform
 * @ops:	operations that this entity can perform
 *
 * This structure is supposed to be embedded into a larger structure
 * better representing the specifics of the instance (e.g. v4l2_request_entity
 * for controlling V4L2 devices).
 *
 */
struct media_request_entity {
	enum media_request_entity_type type;
	const struct media_request_entity_ops *ops;
};

/**
 * media_request_entity_init() - initialize a request entity's base properties
 *
 * @entity:	entity to initialize
 * @type:	type of this entity
 * @ops:	operations that this entity will perform
 */
void media_request_entity_init(struct media_request_entity *entity,
			       enum media_request_entity_type type,
			       const struct media_request_entity_ops *ops);

/**
 * struct media_request_entity_data - per-entity request data
 *
 * @request:	request instance this data belongs to
 * @entity:	entity that stores data here
 * @list:	entry in media_request::data
 * @completed:	whether this entity has completed its part of the request
 *
 * Base structure used to store request state data. To be extended by actual
 * implementation.
 *
 */
struct media_request_entity_data {
	struct media_request *request;
	struct media_request_entity *entity;
	struct list_head list;
	bool completed;
};

/**
 * struct media_request - Media request base structure
 *
 * @mgr:	manager this request belongs to
 * @file:	used to export FDs to user-space and reference count
 * @list:	entry in the media_request_mgr::requests list
 * @lock:	protects following members against concurrent accesses
 * @state:	current state of the request
 * @data:	per-entity data list
 * @complete_wait:	wait queue that signals once the request has completed
 */
struct media_request {
	struct media_request_mgr *mgr;
	struct file *file;
	struct list_head list;

	struct mutex lock;
	enum media_request_state state;
	struct list_head data;
	wait_queue_head_t complete_wait;
};

/**
 * media_request_get() - increment the reference counter of a request
 *
 * The calling context must call media_request_put() once it does not need
 * the reference to the request anymore.
 *
 * Returns the request that has been passed as argument.
 *
 * @req:	request to acquire a reference of
 */
struct media_request *media_request_get(struct media_request *req);

/**
 * media_request_get_from_fd() - lookup request by fd and acquire a reference.
 *
 * Look a request up from its fd, acquire a reference and return a pointer to
 * the request. As for media_request_get(), media_request_put() must be called
 * once the reference is not used anymore.
 *
 * @req:	request to lookup and acquire.
 *
 */
struct media_request *media_request_get_from_fd(int fd);

/**
 * media_request_put() - decrement the reference counter of a request
 *
 * Mirror function of media_request_get() and media_request_get_from_fd(). Will
 * free the request if this was the last valid reference.
 *
 * @req:	request to release.
 */
void media_request_put(struct media_request *req);

/**
 * media_request_lock() - prevent concurrent access to that request
 *
 * @req:	request to lock
 */
static inline void media_request_lock(struct media_request *req)
{
	mutex_lock(&req->lock);
}

/**
 * media_request_unlock() - release previously acquired request lock
 *
 * @req:	request to release
 */
static inline void media_request_unlock(struct media_request *req)
{
	mutex_unlock(&req->lock);
}

/**
 * media_request_get_state() - get the state of a request
 *
 * @req:	request which state we are interested in
 *
 * The request lock should always be acquired when confirming this value
 * to avoid race conditions.
 *
 */
static inline enum media_request_state
media_request_get_state(struct media_request *req)
{
	return req->state;
}

/**
 * media_request_get_entity_data() - get per-entity data for a request
 * @req:	request to get entity data from
 * @entity:	entity to get data of
 *
 * Search and return the entity data associated associated to the request. If no
 * such data exists, it is allocated as per the entity operations.
 *
 * Returns the per-entity data, or an error code if a problem happened. -EINVAL
 * means that data for the entity already existed, but has been allocated under
 * a different cookie.
 */
struct media_request_entity_data *
media_request_get_entity_data(struct media_request *req,
			      struct media_request_entity *entity);

/**
 * media_request_entity_complete() - to be invoked when an entity completes its
 *				     part of the request
 *
 * @req:	request which has completed
 * @completed:	entity that has completed
 */
void media_request_entity_complete(struct media_request *req,
				   struct media_request_entity *completed);

/**
 * media_request_ioctl_new() - process a NEW_REQUEST ioctl
 *
 * @mgr:	request manager from which to allocate the request
 * @new:	media_request_new structure to be passed back to user-space
 *
 * This function is a helper to be called by actual handlers of *_NEW_REQUEST
 * ioctls.
 */
long media_request_ioctl_new(struct media_request_mgr *mgr,
			     struct media_request_new *new);

/**
 * struct media_request_ops - request operations
 *
 * @alloc:	allocate a request
 * @release:	free a previously allocated request
 * @entity_valid:	returns whether a given entity is valid for that request
 * @submit:	allow the request to be processed
 *
 * This structure allows to specialize requests to a specific scope. For
 * instance, requests obtained from a V4L2 device node should only be able to
 * control that device. On the other hand, requests created from a media
 * controller node will be able to control all the devices managed by this
 * controller, and may want to implement some form of synchronization between
 * them.
 */
struct media_request_ops {
	struct media_request *(*alloc)(struct media_request_mgr *mgr);
	void (*release)(struct media_request *req);
	bool (*entity_valid)(const struct media_request *req,
			     const struct media_request_entity *entity);
	int (*submit)(struct media_request *req);
};

/**
 * struct media_request_mgr - requests manager
 *
 * @dev:	device owning this manager
 * @ops:	implementation of the manager
 * @mutex:	protects the requests list_head
 * @requests:	list of alive requests produced by this manager
 *
 * This structure is mainly responsible for allocating requests. Although it is
 * not strictly required for that purpose, having it allows us to account for
 * all requests created by a given device, and to make sure they are all
 * discarded by the time the device is destroyed.
 */
struct media_request_mgr {
	struct device *dev;
	const struct media_request_ops *ops;

	struct mutex mutex;
	struct list_head requests;
};

/**
 * media_request_mgr_init() - initialize a request manager.
 *
 * @mgr:	manager to initialize
 */
void media_request_mgr_init(struct media_request_mgr *mgr, struct device *dev,
			    const struct media_request_ops *ops);

/**
 * media_request_mgr_free() - free a media manager
 *
 * This should only be called when all requests produced by this manager
 * has been destroyed. Will warn if that is not the case.
 */
void media_request_mgr_free(struct media_request_mgr *mgr);

#else /* CONFIG_MEDIA_REQUEST_API */

static inline void media_request_entity_complete(struct media_request *req,
					 struct media_request_entity *completed)
{
}

static inline struct media_request_entity_data *
media_request_get_entity_data(struct media_request *req,
			      struct media_request_entity *entity)
{
	return ERR_PTR(-ENOTSUPP);
}

static inline long media_request_ioctl_new(struct media_request_mgr *mgr,
					   struct media_request_new *new)
{
	return -ENOTTY;
}

static inline void media_request_put(struct media_request *req)
{
}

static inline void media_request_lock(struct media_request *req)
{
}

static inline void media_request_unlock(struct media_request *req)
{
}

static inline enum media_request_state
media_request_get_state(struct media_request *req)
{
	return MEDIA_REQUEST_STATE_INVALID;
}

#endif /* CONFIG_MEDIA_REQUEST_API */

#endif
