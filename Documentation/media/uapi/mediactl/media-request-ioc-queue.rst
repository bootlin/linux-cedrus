.. SPDX-License-Identifier: GPL-2.0-only

.. _media_request_ioc_queue:

*****************************
ioctl MEDIA_REQUEST_IOC_QUEUE
*****************************

Name
====

MEDIA_REQUEST_IOC_QUEUE - Queue a request


Synopsis
========

.. c:function:: int ioctl( int request_fd, MEDIA_REQUEST_IOC_QUEUE )
    :name: MEDIA_REQUEST_IOC_QUEUE


Arguments
=========

``request_fd``
    File descriptor returned by :ref:`MEDIA_IOC_REQUEST_ALLOC`.


Description
===========

If the media device supports :ref:`requests <media-request-api>`, then
this request ioctl can be used to queue a previously allocated request.

If the request was successfully queued, then the file descriptor can be
polled to wait for the request to complete.

If the request was already queued before, then '`EBUSY'` is returned.
Other errors can be returned if the contents of the request contained
invalid or inconsistent data.

Return Value
============

On success 0 is returned, on error -1 and the ``errno`` variable is set
appropriately. The generic error codes are described at the
:ref:`Generic Error Codes <gen-errors>` chapter.

EBUSY
    The request was already queued.
