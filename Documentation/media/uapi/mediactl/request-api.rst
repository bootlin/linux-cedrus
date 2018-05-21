.. -*- coding: utf-8; mode: rst -*-

.. _media-request-api:

Request API
===========

The Request API has been designed to allow V4L2 to deal with requirements of
modern devices (stateless codecs, complex camera pipelines, ...) and APIs
(Android Codec v2). One such requirement is the ability for devices belonging to
the same pipeline to reconfigure and collaborate closely on a per-frame basis.
Another is efficient support of stateless codecs, which need per-frame controls
to be set synchronously in order to be used efficiently.

Supporting these features without the Request API is not always possible and if
it is, it is terribly inefficient: user-space would have to flush all activity
on the media pipeline, reconfigure it for the next frame, queue the buffers to
be processed with that configuration, and wait until they are all available for
dequeuing before considering the next frame. This defeats the purpose of having
buffer queues since in practice only one buffer would be queued at a time.

The Request API allows a specific configuration of the pipeline (media
controller topology + controls for each media entity) to be associated with
specific buffers. The parameters are applied by each participating device as
buffers associated to a request flow in. This allows user-space to schedule
several tasks ("requests") with different parameters in advance, knowing that
the parameters will be applied when needed to get the expected result. Control
values at the time of request completion are also available for reading.

Usage
=====

The Request API is used on top of standard media controller and V4L2 calls,
which are augmented with an extra ``request_fd`` parameter. Requests themselves
are allocated from the supporting media controller node.

Request Allocation
------------------

User-space allocates requests using :ref:`MEDIA_IOC_REQUEST_ALLOC`
for the media device node. This returns a file descriptor representing the
request. Typically, several such requests will be allocated.

Request Preparation
-------------------

Standard V4L2 ioctls can then receive a request file descriptor to express the
fact that the ioctl is part of said request, and is not to be applied
immediately. See :ref:`MEDIA_IOC_REQUEST_ALLOC` for a list of ioctls that
support this. Controls set with a ``request_fd`` parameter are stored instead
of being immediately applied, and buffers queued to a request do not enter the
regular buffer queue until the request itself is queued.

Request Submission
------------------

Once the parameters and buffers of the request are specified, it can be
queued by calling :ref:`MEDIA_REQUEST_IOC_QUEUE` on the request FD.
This will make the buffers associated to the request available to their driver,
which can then apply the associated controls as buffers are processed. A queued
request cannot be modified anymore.

If the request contains parameters for multiple entities, individual drivers may
synchronize so the requested pipeline's topology is applied before the buffers
are processed. Media controller drivers do a best effort implementation since
perfect atomicity may not be possible due to hardware limitations.

Once buffers are added to a request for the first time it is no longer possible
to queue non-request buffers of that buffer type until
:ref:`VIDIOC_STREAMOFF <VIDIOC_STREAMON>` is called.

Controls can still be set without a request and are applied immediately,
regardless of whether a request is in use or not. Setting the same control
through a request and directly can lead to undefined behavior!

User-space can ``poll()`` a request FD in order to wait until the request
completes. A request is considered complete once all its associated buffers are
available for dequeuing and all the associated controls have been updated with
the values at the time of completion. Note that user-space does not need to wait
for the request to complete to dequeue its buffers: buffers that are available
halfway through a request can be dequeued independently of the request's state.

A completed request contains the state of the request at the time of the
request completion. User-space can query that state by calling
:ref:`ioctl VIDIOC_G_EXT_CTRLS <VIDIOC_G_EXT_CTRLS>` with the request FD.

Recycling and Destruction
-------------------------

Finally, completed request can either be discarded or be reused. Calling
``close()`` on a request FD will make that FD unusable, freeing the request if
it is not in use by the kernel. That is, if the request is queued and then the
FD is closed, then it won't be freed until the driver completed the request.

The :ref:`MEDIA_REQUEST_IOC_REINIT` will clear a request's state and make it
available again. No state is retained by this operation: the request is as
if it had just been allocated.

Example for a Memory-to-Memory Device
-------------------------------------

M2M devices are single-node V4L2 devices providing one OUTPUT queue (for
user-space to provide input buffers) and one CAPTURE queue (to retrieve
processed data). These devices are commonly used for frame processors or
stateless codecs.

In this use-case, the request API can be used to associate specific controls to
be applied by the driver for the OUTPUT buffer, allowing user-space
to queue many such buffers in advance. It can also take advantage of requests'
ability to capture the state of controls when the request completes to read back
information that may be subject to change.

Put into code, after obtaining a request, user-space can assign controls and one
OUTPUT buffer to it:

.. code-block:: c

	struct v4l2_buffer buf;
	struct v4l2_ext_controls ctrls;
	struct media_request_alloc alloc = { 0 };
	int req_fd;
	...
	ioctl(media_fd, MEDIA_IOC_REQUEST_ALLOC, &alloc);
	req_fd = alloc.fd;
	...
	ctrls.which = V4L2_CTRL_WHICH_REQUEST_VAL;
	ctrls.request_fd = req_fd;
	ioctl(codec_fd, VIDIOC_S_EXT_CTRLS, &ctrls);
	...
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.flags |= V4L2_BUF_FLAG_REQUEST_FD;
	buf.request_fd = req_fd;
	ioctl(codec_fd, VIDIOC_QBUF, &buf);

Note that there is typically no need to use the Request API for CAPTURE buffers
since there are no per-frame settings to report there.

Once the request is fully prepared, it can be queued to the driver:

.. code-block:: c

	ioctl(req_fd, MEDIA_REQUEST_IOC_QUEUE);

User-space can then either wait for the request to complete by calling poll() on
its file descriptor, or start dequeuing CAPTURE buffers. Most likely, it will
want to get CAPTURE buffers as soon as possible and this can be done using a
regular DQBUF:

.. code-block:: c

	struct v4l2_buffer buf;

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(codec_fd, VIDIOC_DQBUF, &buf);

Note that this example assumes for simplicity that for every OUTPUT buffer
there will be one CAPTURE buffer, but this does not have to be the case.

We can then, after ensuring that the request is completed via polling the
request FD, query control values at the time of its completion via an
annotated call to G_EXT_CTRLS. This is particularly useful for volatile controls
for which we want to query values as soon as the capture buffer is produced.

.. code-block:: c

	struct pollfd pfd = { .events = POLLPRI, .fd = request_fd };
	poll(&pfd, 1, -1);
	...
	ctrls.which = V4L2_CTRL_WHICH_REQUEST_VAL;
	ctrls.request_fd = req_fd;
	ioctl(codec_fd, VIDIOC_G_EXT_CTRLS, &ctrls);

Once we don't need the request anymore, we can either recycle it for reuse with
:ref:`MEDIA_REQUEST_IOC_REINIT`...

.. code-block:: c

	ioctl(req_fd, MEDIA_REQUEST_IOC_REINIT);

... or close its file descriptor to completely dispose of it.

.. code-block:: c

	close(req_fd);

Example for a Simple Capture Device
-----------------------------------

With a simple capture device, requests can be used to specify controls to apply
for a given CAPTURE buffer.

.. code-block:: c

	struct v4l2_buffer buf;
	struct v4l2_ext_controls ctrls;
	struct media_request_alloc alloc = { 0 };
	int req_fd;
	...
	ioctl(media_fd, MEDIA_IOC_REQUEST_ALLOC, &alloc);
	req_fd = alloc.fd;
	...
	ctrls.which = V4L2_CTRL_WHICH_REQUEST_VAL;
	ctrls.request_fd = req_fd;
	ioctl(camera_fd, VIDIOC_S_EXT_CTRLS, &ctrls);
	...
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.flags |= V4L2_BUF_FLAG_REQUEST_FD;
	buf.request_fd = req_fd;
	ioctl(camera_fd, VIDIOC_QBUF, &buf);

Once the request is fully prepared, it can be queued to the driver:

.. code-block:: c

	ioctl(req_fd, MEDIA_REQUEST_IOC_QUEUE);

User-space can then dequeue buffers, wait for the request completion, query
controls and recycle the request as in the M2M example above.
