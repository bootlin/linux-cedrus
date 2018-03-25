// SPDX-License-Identifier: GPL-2.0
/*
 * Media device request objects
 *
 * Copyright (C) 2018 Intel Corporation
 * Copyright (C) 2018, The Chromium OS Authors.  All rights reserved.
 *
 * Author: Sakari Ailus <sakari.ailus@linux.intel.com>
 */

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/string.h>

#include <media/media-device.h>
#include <media/media-request.h>

static const char * const request_state[] = {
	"idle",
	"queueing",
	"queued",
	"complete",
	"cleaning",
};

static const char *
media_request_state_str(enum media_request_state state)
{
	if (WARN_ON(state >= ARRAY_SIZE(request_state)))
		return "unknown";
	return request_state[state];
}

static void media_request_clean(struct media_request *req)
{
	struct media_request_object *obj, *obj_safe;

	WARN_ON(req->state != MEDIA_REQUEST_STATE_CLEANING);

	list_for_each_entry_safe(obj, obj_safe, &req->objects, list) {
		media_request_object_unbind(obj);
		media_request_object_put(obj);
	}

	req->num_incomplete_objects = 0;
	wake_up_interruptible(&req->poll_wait);
}

static void media_request_release(struct kref *kref)
{
	struct media_request *req =
		container_of(kref, struct media_request, kref);
	struct media_device *mdev = req->mdev;
	unsigned long flags;

	dev_dbg(mdev->dev, "request: release %s\n", req->debug_str);

	spin_lock_irqsave(&req->lock, flags);
	req->state = MEDIA_REQUEST_STATE_CLEANING;
	spin_unlock_irqrestore(&req->lock, flags);

	media_request_clean(req);

	if (mdev->ops->req_free)
		mdev->ops->req_free(req);
	else
		kfree(req);
}

void media_request_put(struct media_request *req)
{
	kref_put(&req->kref, media_request_release);
}
EXPORT_SYMBOL_GPL(media_request_put);

void media_request_cancel(struct media_request *req)
{
	struct media_request_object *obj, *obj_safe;

	if (req->state != MEDIA_REQUEST_STATE_QUEUED)
		return;

	list_for_each_entry_safe(obj, obj_safe, &req->objects, list)
		if (obj->ops->cancel)
			obj->ops->cancel(obj);
}
EXPORT_SYMBOL_GPL(media_request_cancel);

static int media_request_close(struct inode *inode, struct file *filp)
{
	struct media_request *req = filp->private_data;

	media_request_put(req);
	return 0;
}

static unsigned int media_request_poll(struct file *filp,
				       struct poll_table_struct *wait)
{
	struct media_request *req = filp->private_data;
	unsigned long flags;
	enum media_request_state state;

	if (!(poll_requested_events(wait) & POLLPRI))
		return 0;

	spin_lock_irqsave(&req->lock, flags);
	state = req->state;
	spin_unlock_irqrestore(&req->lock, flags);

	if (state == MEDIA_REQUEST_STATE_COMPLETE)
		return POLLPRI;
	if (state == MEDIA_REQUEST_STATE_IDLE)
		return POLLERR;

	poll_wait(filp, &req->poll_wait, wait);
	return 0;
}

static long media_request_ioctl_queue(struct media_request *req)
{
	struct media_device *mdev = req->mdev;
	unsigned long flags;
	int ret = 0;

	dev_dbg(mdev->dev, "request: queue %s\n", req->debug_str);

	spin_lock_irqsave(&req->lock, flags);
	if (req->state != MEDIA_REQUEST_STATE_IDLE) {
		dev_dbg(mdev->dev,
			"request: unable to queue %s, request in state %s\n",
			req->debug_str, media_request_state_str(req->state));
		spin_unlock_irqrestore(&req->lock, flags);
		return -EINVAL;
	}
	req->state = MEDIA_REQUEST_STATE_QUEUEING;

	spin_unlock_irqrestore(&req->lock, flags);

	/*
	 * Ensure the request that is validated will be the one that gets queued
	 * next by serialising the queueing process.
	 */
	mutex_lock(&mdev->req_queue_mutex);

	ret = mdev->ops->req_queue(req);
	spin_lock_irqsave(&req->lock, flags);
	req->state = ret ? MEDIA_REQUEST_STATE_IDLE : MEDIA_REQUEST_STATE_QUEUED;
	spin_unlock_irqrestore(&req->lock, flags);
	mutex_unlock(&mdev->req_queue_mutex);

	if (ret) {
		dev_dbg(mdev->dev, "request: can't queue %s (%d)\n",
			req->debug_str, ret);
	} else {
		media_request_get(req);
	}

	return ret;
}

static long media_request_ioctl_reinit(struct media_request *req)
{
	struct media_device *mdev = req->mdev;
	unsigned long flags;

	spin_lock_irqsave(&req->lock, flags);
	if (req->state != MEDIA_REQUEST_STATE_IDLE &&
	    req->state != MEDIA_REQUEST_STATE_COMPLETE) {
		dev_dbg(mdev->dev,
			"request: %s not in idle or complete state, cannot reinit\n",
			req->debug_str);
		spin_unlock_irqrestore(&req->lock, flags);
		return -EINVAL;
	}
	req->state = MEDIA_REQUEST_STATE_CLEANING;
	spin_unlock_irqrestore(&req->lock, flags);

	media_request_clean(req);

	spin_lock_irqsave(&req->lock, flags);
	req->state = MEDIA_REQUEST_STATE_IDLE;
	spin_unlock_irqrestore(&req->lock, flags);
	return 0;
}

static long media_request_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	struct media_request *req = filp->private_data;

	switch (cmd) {
	case MEDIA_REQUEST_IOC_QUEUE:
		return media_request_ioctl_queue(req);
	case MEDIA_REQUEST_IOC_REINIT:
		return media_request_ioctl_reinit(req);
	default:
		return -ENOIOCTLCMD;
	}
}

static const struct file_operations request_fops = {
	.owner = THIS_MODULE,
	.poll = media_request_poll,
	.unlocked_ioctl = media_request_ioctl,
	.release = media_request_close,
};

/**
 * media_request_find - Find a request based on the file descriptor
 * @mdev: The media device
 * @request: The request file handle
 *
 * Find and return the request associated with the given file descriptor, or
 * an error if no such request exists.
 *
 * When the function returns a request it increases its reference count. The
 * caller is responsible for releasing the reference by calling
 * media_request_put() on the request.
 */
struct media_request *
media_request_find(struct media_device *mdev, int request_fd)
{
	struct file *filp;
	struct media_request *req;

	if (!mdev || !mdev->ops || !mdev->ops->req_queue)
		return ERR_PTR(-EPERM);

	filp = fget(request_fd);
	if (!filp)
		return ERR_PTR(-ENOENT);

	if (filp->f_op != &request_fops)
		goto err_fput;
	req = filp->private_data;
	media_request_get(req);

	if (req->mdev != mdev)
		goto err_kref_put;

	fput(filp);

	return req;

err_kref_put:
	media_request_put(req);

err_fput:
	fput(filp);

	return ERR_PTR(-ENOENT);
}
EXPORT_SYMBOL_GPL(media_request_find);

int media_request_alloc(struct media_device *mdev,
			struct media_request_alloc *alloc)
{
	struct media_request *req;
	struct file *filp;
	char comm[TASK_COMM_LEN];
	int fd;
	int ret;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;

	filp = anon_inode_getfile("request", &request_fops, NULL, O_CLOEXEC);
	if (IS_ERR(filp)) {
		ret = PTR_ERR(filp);
		goto err_put_fd;
	}

	if (mdev->ops->req_alloc)
		req = mdev->ops->req_alloc(mdev);
	else
		req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto err_fput;
	}

	filp->private_data = req;
	req->mdev = mdev;
	req->state = MEDIA_REQUEST_STATE_IDLE;
	req->num_incomplete_objects = 0;
	kref_init(&req->kref);
	INIT_LIST_HEAD(&req->objects);
	spin_lock_init(&req->lock);
	init_waitqueue_head(&req->poll_wait);

	alloc->fd = fd;

	get_task_comm(comm, current);
	snprintf(req->debug_str, sizeof(req->debug_str), "%s:%d",
		 comm, fd);
	dev_dbg(mdev->dev, "request: allocated %s\n", req->debug_str);

	fd_install(fd, filp);

	return 0;

err_fput:
	fput(filp);

err_put_fd:
	put_unused_fd(fd);

	return ret;
}

static void media_request_object_release(struct kref *kref)
{
	struct media_request_object *obj =
		container_of(kref, struct media_request_object, kref);
	struct media_request *req = obj->req;

	if (req)
		media_request_object_unbind(obj);
	obj->ops->release(obj);
}

struct media_request_object *
media_request_object_find(struct media_request *req,
			  const struct media_request_object_ops *ops,
			  void *priv)
{
	struct media_request_object *obj;
	struct media_request_object *found = NULL;
	unsigned long flags;

	if (!ops && !priv)
		return NULL;

	spin_lock_irqsave(&req->lock, flags);
	list_for_each_entry(obj, &req->objects, list) {
		if ((!ops || obj->ops == ops) &&
		    (!priv || obj->priv == priv)) {
			media_request_object_get(obj);
			found = obj;
			break;
		}
	}
	spin_unlock_irqrestore(&req->lock, flags);
	return found;
}
EXPORT_SYMBOL_GPL(media_request_object_find);

void media_request_object_put(struct media_request_object *obj)
{
	kref_put(&obj->kref, media_request_object_release);
}
EXPORT_SYMBOL_GPL(media_request_object_put);

void media_request_object_init(struct media_request_object *obj)
{
	obj->ops = NULL;
	obj->req = NULL;
	obj->priv = NULL;
	obj->completed = false;
	INIT_LIST_HEAD(&obj->list);
	kref_init(&obj->kref);
}
EXPORT_SYMBOL_GPL(media_request_object_init);

void media_request_object_bind(struct media_request *req,
			       const struct media_request_object_ops *ops,
			       void *priv,
			       struct media_request_object *obj)
{
	unsigned long flags;

	if (WARN_ON(!ops->release || !ops->cancel))
		return;

	obj->req = req;
	obj->ops = ops;
	obj->priv = priv;
	spin_lock_irqsave(&req->lock, flags);
	if (WARN_ON(req->state != MEDIA_REQUEST_STATE_IDLE))
		goto unlock;
	list_add_tail(&obj->list, &req->objects);
	req->num_incomplete_objects++;
unlock:
	spin_unlock_irqrestore(&req->lock, flags);
}
EXPORT_SYMBOL_GPL(media_request_object_bind);

void media_request_object_unbind(struct media_request_object *obj)
{
	struct media_request *req = obj->req;
	unsigned long flags;
	bool completed = false;

	if (!req)
		return;

	spin_lock_irqsave(&req->lock, flags);
	list_del(&obj->list);
	obj->req = NULL;

	if (req->state == MEDIA_REQUEST_STATE_COMPLETE ||
	    req->state == MEDIA_REQUEST_STATE_CLEANING)
		goto unlock;

	if (WARN_ON(req->state == MEDIA_REQUEST_STATE_QUEUEING))
		goto unlock;

	if (WARN_ON(!req->num_incomplete_objects))
		goto unlock;

	req->num_incomplete_objects--;
	if (req->state == MEDIA_REQUEST_STATE_QUEUED &&
	    !req->num_incomplete_objects) {
		req->state = MEDIA_REQUEST_STATE_COMPLETE;
		completed = true;
		wake_up_interruptible(&req->poll_wait);
	}
unlock:
	spin_unlock_irqrestore(&req->lock, flags);
	if (obj->ops->unbind)
		obj->ops->unbind(obj);
	if (completed)
		media_request_put(req);
}
EXPORT_SYMBOL_GPL(media_request_object_unbind);

void media_request_object_complete(struct media_request_object *obj)
{
	struct media_request *req = obj->req;
	unsigned long flags;
	bool completed = false;

	spin_lock_irqsave(&req->lock, flags);
	if (obj->completed)
		goto unlock;
	obj->completed = true;
	if (WARN_ON(!req->num_incomplete_objects) ||
	    WARN_ON(req->state != MEDIA_REQUEST_STATE_QUEUED))
		goto unlock;

	if (!--req->num_incomplete_objects) {
		req->state = MEDIA_REQUEST_STATE_COMPLETE;
		wake_up_interruptible(&req->poll_wait);
		completed = true;
	}
unlock:
	spin_unlock_irqrestore(&req->lock, flags);
	if (completed)
		media_request_put(req);
}
EXPORT_SYMBOL_GPL(media_request_object_complete);
