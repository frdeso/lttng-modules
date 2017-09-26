/* SPDX-License-Identifier: (GPL-2.0 or LGPL-2.1)
 *
 * lttng-probe-utils.h
 *
 * Copyright (C) 2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef _LTTNG_PROBE_UTILS_H
#define _LTTNG_PROBE_UTILS_H

#include <linux/fdtable.h>

/*
 * Calculate string length. Include final null terminating character if there is
 * one, or ends at first fault.
 */
long lttng_strlen_user_inatomic(const char *addr);

/*
 * Returns the inode struct from the current task and an fd. The inode is grabed
 * by this function and most be put once we are done with it using iput()
 */
struct inode *lttng_get_inode_from_fd(int fd);

#endif /* _LTTNG_PROBE_UTILS_H */
