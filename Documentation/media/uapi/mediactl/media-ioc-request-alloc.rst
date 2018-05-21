.. SPDX-License-Identifier: GPL-2.0-only

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
this ioctl can be used to allocate a request. A request is accessed through
a file descriptor that is returned in struct :c:type:`media_request_alloc`.

If the request was successfully allocated, then the request file descriptor
can be passed to :ref:`ioctl VIDIOC_QBUF <VIDIOC_QBUF>`,
:ref:`ioctl VIDIOC_G_EXT_CTRLS <VIDIOC_G_EXT_CTRLS>`,
:ref:`ioctl VIDIOC_S_EXT_CTRLS <VIDIOC_G_EXT_CTRLS>` and
:ref:`ioctl VIDIOC_TRY_EXT_CTRLS <VIDIOC_G_EXT_CTRLS>`.

In addition, the request can be queued by calling
:ref:`MEDIA_REQUEST_IOC_QUEUE` and re-initialized by calling
:ref:`MEDIA_REQUEST_IOC_REINIT`.

Finally, the file descriptor can be polled to wait for the request to
complete.

To free the request the file descriptor has to be closed.

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
