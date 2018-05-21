.. SPDX-License-Identifier: GPL-2.0
.. -*- coding: utf-8; mode: rst -*-

.. _request-func-poll:

**************
request poll()
**************

Name
====

request-poll - Wait for some event on a file descriptor


Synopsis
========

.. code-block:: c

    #include <sys/poll.h>


.. c:function:: int poll( struct pollfd *ufds, unsigned int nfds, int timeout )
   :name: request-poll

Arguments
=========

``ufds``
   List of FD events to be watched

``nfds``
   Number of FD events at the \*ufds array

``timeout``
   Timeout to wait for events


Description
===========

With the :c:func:`poll() <request-func-poll>` function applications can wait
for a request to complete.

On success :c:func:`poll() <request-func-poll>` returns the number of file
descriptors that have been selected (that is, file descriptors for which the
``revents`` field of the respective struct :c:type:`pollfd`
is non-zero). Request file descriptor set the ``POLLPRI`` flag in ``revents``
when the request was completed.  When the function times out it returns
a value of zero, on failure it returns -1 and the ``errno`` variable is
set appropriately.


Return Value
============

On success, :c:func:`poll() <request-func-poll>` returns the number of
structures which have non-zero ``revents`` fields, or zero if the call
timed out. On error -1 is returned, and the ``errno`` variable is set
appropriately:

``EBADF``
    One or more of the ``ufds`` members specify an invalid file
    descriptor.

``EFAULT``
    ``ufds`` references an inaccessible memory area.

``EINTR``
    The call was interrupted by a signal.

``EINVAL``
    The ``nfds`` argument is greater than ``OPEN_MAX``.
