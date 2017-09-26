/* SPDX-License-Identifier: (GPL-2.0 or LGPL-2.1)
 *
 * lttng-probe-utils.c
 *
 * Copyright (C) 2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <linux/uaccess.h>
#include <linux/module.h>
#include <probes/lttng-probe-utils.h>

/*
 * Calculate string length. Include final null terminating character if there is
 * one, or ends at first fault. Disabling page faults ensures that we can safely
 * call this from pretty much any context, including those where the caller
 * holds mmap_sem, or any lock which nests in mmap_sem.
 */
long lttng_strlen_user_inatomic(const char *addr)
{
	long count = 0;
	mm_segment_t old_fs;

	if (!addr)
		return 0;

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	pagefault_disable();
	for (;;) {
		char v;
		unsigned long ret;

		if (unlikely(!access_ok(VERIFY_READ,
				(__force const char __user *) addr,
				sizeof(v))))
			break;
		ret = __copy_from_user_inatomic(&v,
			(__force const char __user *)(addr),
			sizeof(v));
		if (unlikely(ret > 0))
			break;
		count++;
		if (unlikely(!v))
			break;
		addr++;
	}
	pagefault_enable();
	set_fs(old_fs);
	return count;
}
EXPORT_SYMBOL_GPL(lttng_strlen_user_inatomic);

/*
 * Returns the inode struct from the current task and an fd. The inode is grabed
 * by this function and most be put once we are done with it using iput()
 */
struct inode *lttng_get_inode_from_fd(int fd)
{
	struct file *file;
	struct inode *inode;

	rcu_read_lock();
	/*
	 * Returns the file backing the given fd. Needs to be done inside an RCU
	 * critical section
	 */
	file = fcheck(fd);
	if (file == NULL) {
		printk(KERN_WARNING "Cannot access file backing the fd(%d)\n", fd);
		inode = NULL;
		goto error;
	}

	/* Grab a reference on the inode. */
	inode = igrab(file->f_path.dentry->d_inode);
	if (inode == NULL)
		printk(KERN_WARNING "Cannot grab a reference on the inode.\n");

error:
	rcu_read_unlock();
	return inode;
}
EXPORT_SYMBOL_GPL(lttng_get_inode_from_fd);
