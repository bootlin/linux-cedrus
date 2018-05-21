.. SPDX-License-Identifier: GPL-2.0
.. -*- coding: utf-8; mode: rst -*-

.. _request-func-close:

***************
request close()
***************

Name
====

request-close - Close a request file descriptor


Synopsis
========

.. code-block:: c

    #include <unistd.h>


.. c:function:: int close( int fd )
    :name: req-close

Arguments
=========

``fd``
    File descriptor returned by :ref:`MEDIA_IOC_REQUEST_ALLOC`.


Description
===========

Closes the request file descriptor. Resources associated with the file descriptor
are freed once the driver has completed the request and no longer needs to
reference it.


Return Value
============

:ref:`close() <request-func-close>` returns 0 on success. On error, -1 is
returned, and ``errno`` is set appropriately. Possible error codes are:

EBADF
    ``fd`` is not a valid open file descriptor.
