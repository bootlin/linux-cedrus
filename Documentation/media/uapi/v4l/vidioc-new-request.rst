.. -*- coding: utf-8; mode: rst -*-

.. _VIDIOC_NEW_REQUEST:

************************
ioctl VIDIOC_NEW_REQUEST
************************

Name
====

VIDIOC_NEW_REQUEST - Allocate a request for given video device.


Synopsis
========

.. c:function:: int ioctl( int fd, VIDIOC_NEW_REQUEST, struct media_request_new *argp )
    :name: VIDIOC_NEW_REQUEST

Arguments
=========

``fd``
    File descriptor returned by :ref:`open() <func-open>`.

``argp``
    Pointer to struct :c:type:`media_request_new`.


Description
===========

Applications call the ``VIDIOC_NEW_REQUEST`` ioctl to allocate a new request for a given V4L2 video device. The request will only be valid in the scope of the device that allocated it and cannot be used to coordinate multiple devices.

Applications can also check whether requests are supported by a given device by calling this ioctl with the MEDIA_REQUEST_FLAG_TEST bit of :c:type:`media_request_new`'s ``flags`` set. Doing so will not allocate a new request, but will return 0 is request allocation is supported by the device, or -1 and set ``errno`` to ENOTTY if they are not.

.. c:type:: media_request_new

.. tabularcolumns:: |p{4.4cm}|p{4.4cm}|p{8.7cm}|

.. flat-table:: struct media_request_new
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    * - __u32
      - ``flags``
      - Flags for this request creation. If ``MEDIA_REQUEST_FLAG_TEST`` is set, then no request is created and the call only checks for request availability. Written by the application.
    * - __s32
      - ``fd``
      - File descriptor referencing the created request. Written by the kernel.

Return Value
============

On success 0 is returned, and the ``fd`` field of ``argp`` is set to a file descriptor referencing the request. User-space can use this file descriptor to mention the request in other system calls, perform ``MEDIA_REQUEST_IOC_SUBMIT`` and ``MEDIA_REQUEST_IOC_REINIT`` ioctls on it, and close it to discard the request.

On error -1 is returned and the ``errno`` variable is set appropriately.  The
generic error codes are described in the :ref:`Generic Error Codes <gen-errors>`
chapter.

ENOTTY
    The device does not support the use of requests or request support is not built into the kernel.
