.. -*- coding: utf-8; mode: rst -*-

.. _media-request-api:

Request API
===========

The Request API has been designed to allow V4L2 to deal with requirements of
modern devices (stateless codecs, MIPI cameras, ...) and APIs (Android Codec
v2). One such requirement is the ability for devices belonging to the same
pipeline to reconfigure and collaborate closely on a per-frame basis. Another is
efficient support of stateless codecs, which need per-frame controls to be set
asynchronously in order to be efficiently used.

Supporting these features without the Request API is possible but terribly
inefficient: user-space would have to flush all activity on the media pipeline,
reconfigure it for the next frame, queue the buffers to be processed with that
configuration, and wait until they are all available for dequeing before
considering the next frame. This defeats the purpose of having buffer queues
since in practice only one buffer would be queued at a time.

The Request API allows a specific configuration of the pipeline (media
controller topology + controls for each device) to be associated with specific
buffers. The parameters are applied by each participating device as buffers
associated to a request flow in. This allows user-space to schedule several
tasks ("requests") with different parameters in advance, knowing that the
parameters will be applied when needed to get the expected result. Controls
values at the time of request completion are also available for reading.

Usage
=====

The Request API is used on top of standard media controller and V4L2 calls,
which are augmented with an extra ``request_fd`` parameter. Request themselves
are allocated from either a supporting V4L2 device node, or a supporting media
controller node. The origin of requests determine their scope: requests
allocated from a V4L2 device node can only act on that device, whereas requests
allocated from a media controller node can control the whole pipeline of the
controller.

Request Allocation
------------------

User-space allocates requests using the ``VIDIOC_NEW_REQUEST`` (for V4L2 device
requests) or ``MEDIA_IOC_NEW_REQUEST`` (for media controller requests) on an
opened device or media node. This returns a file descriptor representing the
request. Typically, several such requests will be allocated.

Request Preparation
-------------------

Standard V4L2 ioctls can then receive a request file descriptor to express the
fact that the ioctl is part of said request, and is not to be applied
immediately. V4L2 ioctls supporting this are :c:func:`VIDIOC_S_EXT_CTRLS` and
:c:func:`VIDIOC_QBUF`. Controls set with a request parameter are stored instead
of being immediately applied, and queued buffers not enter the regular buffer
queue until the request is submitted. Only one buffer can be queued to a given
queue for a given request.

Request Submission
------------------

Once the parameters and buffers of the request are specified, it can be
submitted by calling the ``MEDIA_REQUEST_IOC_SUBMIT`` ioctl on the request FD.
This will make the buffers associated to the request available to their driver,
which can then apply the saved controls as buffers are processed. A submitted
request cannot be modified anymore.

If several devices are part of the request, individual drivers may synchronize
so the requested pipeline's topology is applied before the buffers are
processed. This is at the discretion of media controller drivers and is not a
requirement.

Buffers queued without an associated request after a request-bound buffer will
be processed using the state of the hardware at the time of the request
completion. All the same, controls set without a request are applied
immediately, regardless of whether a request is in use or not.

User-space can ``poll()`` a request FD in order to wait until the request
completes. A request is considered complete once all its associated buffers are
available for dequeing. Note that user-space does not need to wait for the
request to complete to dequeue its buffers: buffers that are available halfway
through a request can be dequeued independently of the request's state.

A completed request includes the state of all devices that had queued buffers
associated with it at the time of the request completion. User-space can query
that state by calling :c:func:`VIDIOC_G_EXT_CTRLS` with the request FD.

Recycling and Destruction
-------------------------

Finally, completed request can either be discarded or be reused. Calling
``close()`` on a request FD will make that FD unusable, freeing the request if
it is not referenced elsewhere. The ``MEDIA_REQUEST_IOC_SUBMIT`` ioctl will
clear a request's state and make it available again. No state is retained by
this operation: the request is as if it had just been allocated.

Example for a M2M Device
------------------------

M2M devices are single-node V4L2 devices providing one OUTPUT queue (for
user-space
to provide input buffers) and one CAPTURE queue (to retrieve processed data).
They are perfectly symetric, i.e. one buffer of input will produce one buffer of
output. These devices are commonly used for frame processors or stateless
codecs.

In this use-case, the request API can be used to associate specific controls to
be applied by the driver before processing an OUTPUT buffer, allowing user-space
to queue many such buffers in advance. It can also take advantage of requests'
ability to capture the state of controls when the request completes to read back
information that may be subject to change.

Put into code, after obtaining a request, user-space can assign controls and one
OUTPUT buffer to it:

	struct v4l2_buf buf;
	struct v4l2_ext_controls ctrls;
	struct media_request_new new = { 0 };
	int req_fd;
	...
	ioctl(media_fd, VIDIOC_NEW_REQUEST, &new);
	req_fd = new.fd;
	...
	ctrls.request_fd = req_fd;
	ioctl(codec_fd, VIDIOC_S_EXT_CTRLS, &ctrls);
	...
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.request_fd = req_fd;
	ioctl(codec_fd, VIDIOC_QBUF, &buf);

Note that request_fd does not need to be specified for CAPTURE buffers: since
there is symetry between the OUTPUT and CAPTURE queues, and requests are
processed in order of submission, we can know which CAPTURE buffer corresponds
to which request.

Once the request is fully prepared, it can be submitted to the driver:

	ioctl(request_fd, MEDIA_REQUEST_IOC_SUBMIT, NULL);

User-space can then either wait for the request to complete by calling poll() on
its file descriptor, or start dequeuing CAPTURE buffers. Most likely, it will
want to get CAPTURE buffers as soon as possible and this can be done using a
regular DQBUF:

	struct v4l2_buf buf;

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(codec_fd, VIDIOC_DQBUF, &buf);

We can then, after ensuring that the request is completed via polling the
request FD, query control values at the time of its completion via an
annotated call to G_EXT_CTRLS. This is particularly useful for volatile controls
for which we want to query values as soon as the capture buffer is produced.

	struct pollfd pfd = { .events = POLLIN, .fd = request_fd };
	poll(&pfd, 1, -1);
	...
	ctrls.request_fd = req_fd;
	ioctl(codec_fd, VIDIOC_G_EXT_CTRLS, &ctrls);

Once we don't need the request anymore, we can either recycle it for reuse with
MEDIA_REQUEST_IOC_REINIT...

	ioctl(request, MEDIA_REQUEST_IOC_REINIT, NULL);

... or close its file descriptor to completely dispose of it.

	close(request_fd);

Example for a Simple Capture Device
-----------------------------------

With a simple capture device, requests can be used to specify controls to apply
to a given CAPTURE buffer. The driver will apply these controls before producing
the marked CAPTURE buffer.

	struct v4l2_buf buf;
	struct v4l2_ext_controls ctrls;
	struct media_request_new new = { 0 };
	int req_fd;
	...
	ioctl(camera_fd, VIDIOC_NEW_REQUEST, &new);
	req_fd = new.fd;
	...
	ctrls.request_fd = req_fd;
	ioctl(camera_fd, VIDIOC_S_EXT_CTRLS, &ctrls);
	...
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.request_fd = req_fd;
	ioctl(camera_fd, VIDIOC_QBUF, &buf);

Once the request is fully prepared, it can be submitted to the driver:

	ioctl(req_fd, MEDIA_REQUEST_IOC_SUBMIT, &cmd);

User-space can then dequeue buffers, wait for the request completion, query
controls and recycle the request as in the M2M example above.
