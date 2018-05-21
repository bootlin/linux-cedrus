.. SPDX-License-Identifier: GPL-2.0

.. _media_ioc_request_alloc:

*****************************
ioctl MEDIA_IOC_REQUEST_ALLOC
*****************************

Name
====

MEDIA_IOC_REQUEST_ALLOC - Allocate a request


Synopsis
========

.. c:function:: int ioctl( int fd, MEDIA_IOC_REQUEST_ALLOC, struct media_request_alloc *argp )
    :name: MEDIA_IOC_REQUEST_ALLOC


Arguments
=========

``fd``
    File descriptor returned by :ref:`open() <media-func-open>`.

``argp``


Description
===========

If the media device supports :ref:`requests <media-request-api>`, then
this ioctl can be used to allocate a request. If it is not supported, then
``errno`` is set to ``ENOTTY``. A request is accessed through a file descriptor
that is returned in struct :c:type:`media_request_alloc`.

If the request was successfully allocated, then the request file descriptor
can be passed to the :ref:`VIDIOC_QBUF <VIDIOC_QBUF>`,
:ref:`VIDIOC_G_EXT_CTRLS <VIDIOC_G_EXT_CTRLS>`,
:ref:`VIDIOC_S_EXT_CTRLS <VIDIOC_G_EXT_CTRLS>` and
:ref:`VIDIOC_TRY_EXT_CTRLS <VIDIOC_G_EXT_CTRLS>` ioctls.

In addition, the request can be queued by calling
:ref:`MEDIA_REQUEST_IOC_QUEUE` and re-initialized by calling
:ref:`MEDIA_REQUEST_IOC_REINIT`.

Finally, the file descriptor can be :ref:`polled <request-func-poll>` to wait
for the request to complete.

The request will remain allocated until the associated file descriptor is
closed by :ref:`close() <request-func-close>` and the driver no longer uses
the request internally.

.. c:type:: media_request_alloc

.. tabularcolumns:: |p{4.4cm}|p{4.4cm}|p{8.7cm}|

.. flat-table:: struct media_request_alloc
    :header-rows:  0
    :stub-columns: 0
    :widths:       1 1 2

    *  -  __s32
       -  ``fd``
       -  The file descriptor of the request.

Return Value
============

On success 0 is returned, on error -1 and the ``errno`` variable is set
appropriately. The generic error codes are described at the
:ref:`Generic Error Codes <gen-errors>` chapter.

ENOTTY
    The driver has no support for requests.
