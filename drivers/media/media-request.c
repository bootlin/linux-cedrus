/*
 * Request base implementation
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

#include <linux/anon_inodes.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/list.h>
#include <linux/device.h>
#include <linux/media-request.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>

#include <media/media-request.h>

const struct file_operations request_fops;

static const char * const media_request_states[] __maybe_unused = {
	"IDLE",
	"SUBMITTED",
	"COMPLETED",
	"INVALID",
};

struct media_request *media_request_get(struct media_request *req)
{
	get_file(req->file);
	return req;
}
EXPORT_SYMBOL_GPL(media_request_get);

struct media_request *media_request_get_from_fd(int fd)
{
	struct file *f;

	f = fget(fd);
	if (!f)
		return NULL;

	/* Not a request FD? */
	if (f->f_op != &request_fops) {
		fput(f);
		return NULL;
	}

	return f->private_data;
}
EXPORT_SYMBOL_GPL(media_request_get_from_fd);

void media_request_put(struct media_request *req)
{
	if (WARN_ON(req == NULL))
		return;

	fput(req->file);
}
EXPORT_SYMBOL_GPL(media_request_put);

struct media_request_entity_data *
media_request_get_entity_data(struct media_request *req,
			      struct media_request_entity *entity)
{
	struct media_request_entity_data *data;

	/* First check that this entity is valid for this request at all */
	if (!req->mgr->ops->entity_valid(req, entity))
		return ERR_PTR(-EINVAL);

	mutex_lock(&req->lock);

	/* Lookup whether we already have entity data */
	list_for_each_entry(data, &req->data, list) {
		if (data->entity == entity)
			goto out;
	}

	/* No entity data found, let's create it */
	data = entity->ops->data_alloc(req, entity);
	if (IS_ERR(data))
		goto out;

	data->entity = entity;
	list_add_tail(&data->list, &req->data);

out:
	mutex_unlock(&req->lock);

	return data;
}
EXPORT_SYMBOL_GPL(media_request_get_entity_data);

static unsigned int media_request_poll(struct file *file, poll_table *wait)
{
	struct media_request *req = file->private_data;

	poll_wait(file, &req->complete_wait, wait);

	if (req->state == MEDIA_REQUEST_STATE_COMPLETED)
		return POLLIN | POLLRDNORM;

	return 0;
}

static int media_request_release(struct inode *inode, struct file *filp)
{
	struct media_request *req = filp->private_data;

	if (req == NULL)
		return 0;

	req->mgr->ops->release(req);
	return 0;
}

static long media_request_ioctl_submit(struct media_request *req)
{
	mutex_lock(&req->lock);

	if (req->state != MEDIA_REQUEST_STATE_IDLE) {
		dev_warn(req->mgr->dev, "cannot submit request in state %s\n",
			 media_request_states[req->state]);
		mutex_unlock(&req->lock);
		return -EINVAL;
	}

	req->state = MEDIA_REQUEST_STATE_SUBMITTED;

	/*
	 * Nothing can change our state when we are submitted - no need to keep
	 * holding that lock.
	 */
	mutex_unlock(&req->lock);

	/* Keep a reference to the request until it is completed */
	media_request_get(req);

	return req->mgr->ops->submit(req);
}

static long media_request_ioctl_reinit(struct media_request *req)
{
	struct media_request_entity_data *data, *next;
        LIST_HEAD(to_delete);

	mutex_lock(&req->lock);

	if (req->state == MEDIA_REQUEST_STATE_SUBMITTED) {
		dev_warn(req->mgr->dev,
			"%s: unable to reinit submitted request\n", __func__);
		mutex_unlock(&req->lock);
		return -EINVAL;
	}

	/* delete all entity data */
	list_for_each_entry_safe(data, next, &req->data, list) {
		list_del(&data->list);
                list_add(&data->list, &to_delete);
	}

	/* reinitialize request to idle state */
	req->state = MEDIA_REQUEST_STATE_IDLE;

	mutex_unlock(&req->lock);

        list_for_each_entry_safe(data, next, &to_delete, list)
		data->entity->ops->data_free(data);

	return 0;
}

#define MEDIA_REQUEST_IOC(__cmd, func)					\
	[_IOC_NR(MEDIA_REQUEST_IOC_##__cmd) - 0x80] = {			\
		.cmd = MEDIA_REQUEST_IOC_##__cmd,			\
		.fn = func,						\
	}

struct media_request_ioctl_info {
	unsigned int cmd;
	long (*fn)(struct media_request *req);
};

static const struct media_request_ioctl_info ioctl_info[] = {
	MEDIA_REQUEST_IOC(SUBMIT, media_request_ioctl_submit),
	MEDIA_REQUEST_IOC(REINIT, media_request_ioctl_reinit),
};

static long media_request_ioctl(struct file *filp, unsigned int cmd,
				unsigned long __arg)
{
	struct media_request *req = filp->private_data;
	const struct media_request_ioctl_info *info;

	if ((_IOC_NR(cmd) < 0x80) ||
	     _IOC_NR(cmd) >= 0x80 + ARRAY_SIZE(ioctl_info) ||
	     ioctl_info[_IOC_NR(cmd) - 0x80].cmd != cmd)
		return -ENOIOCTLCMD;

	info = &ioctl_info[_IOC_NR(cmd) - 0x80];

	return info->fn(req);
}

const struct file_operations request_fops = {
	.owner = THIS_MODULE,
	.poll = media_request_poll,
	.release = media_request_release,
	.unlocked_ioctl = media_request_ioctl,
};

static void media_request_complete(struct media_request *req)
{
	struct device *dev = req->mgr->dev;

	mutex_lock(&req->lock);

	if (WARN_ON(req->state != MEDIA_REQUEST_STATE_SUBMITTED)) {
		dev_warn(dev, "can't complete request in state %s\n",
			media_request_states[req->state]);
		mutex_unlock(&req->lock);
		return;
	}

	req->state = MEDIA_REQUEST_STATE_COMPLETED;

	wake_up_interruptible(&req->complete_wait);

	mutex_unlock(&req->lock);

	/* Release the reference acquired when we submitted the request */
	media_request_put(req);
}

void media_request_entity_complete(struct media_request *req,
				   struct media_request_entity *completed)
{
	struct media_request_entity_data *data;
	int cpt = 0;

	list_for_each_entry(data, &req->data, list) {
		if (data->entity == completed)
			data->completed = true;
		if (!data->completed)
			++cpt;
	}

	if (cpt == 0)
		media_request_complete(req);
}
EXPORT_SYMBOL_GPL(media_request_entity_complete);

long media_request_ioctl_new(struct media_request_mgr *mgr,
			     struct media_request_new *new)
{
	struct media_request *req;
	int err;
	int fd;

	/* User only wants to check the availability of requests? */
	if (new->flags & MEDIA_REQUEST_FLAG_TEST)
		return 0;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;

	req = mgr->ops->alloc(mgr);
	if (IS_ERR(req)) {
		err = PTR_ERR(req);
		goto out_fd;
	}

	req->file = anon_inode_getfile("request", &request_fops, req,
				       O_CLOEXEC);
	if (IS_ERR(req->file)) {
		err = PTR_ERR(req->file);
		mgr->ops->release(req);
		goto out_fd;
	}

	fd_install(fd, req->file);
	new->fd = fd;

	return 0;

out_fd:
	put_unused_fd(fd);
	return err;
}
EXPORT_SYMBOL_GPL(media_request_ioctl_new);

void media_request_entity_init(struct media_request_entity *entity,
			       enum media_request_entity_type type,
			       const struct media_request_entity_ops *ops)
{
	entity->type = type;
	entity->ops = ops;
}
EXPORT_SYMBOL_GPL(media_request_entity_init);

void media_request_mgr_init(struct media_request_mgr *mgr, struct device *dev,
			    const struct media_request_ops *ops)
{
	mgr->dev = dev;
	mutex_init(&mgr->mutex);
	INIT_LIST_HEAD(&mgr->requests);
	mgr->ops = ops;
}
EXPORT_SYMBOL_GPL(media_request_mgr_init);

void media_request_mgr_free(struct media_request_mgr *mgr)
{
	struct device *dev = mgr->dev;

	/* Just a sanity check - we should have no remaining requests */
	while (!list_empty(&mgr->requests)) {
		struct media_request *req;

		req = list_first_entry(&mgr->requests, typeof(*req), list);
		dev_warn(dev,
			"%s: request still referenced, deleting forcibly!\n",
			__func__);
		mgr->ops->release(req);
	}
}
EXPORT_SYMBOL_GPL(media_request_mgr_free);

MODULE_AUTHOR("Alexandre Courbot <acourbot@chromium.org>");
MODULE_DESCRIPTION("Core support for media request API");
MODULE_LICENSE("GPL");
