/*
 * probes/lttng-uprobes.c
 *
 * LTTng uprobes integration module.
 *
 * Copyright (C) 2013 Yannick Brosseau <yannick.brosseau@gmail.com>
 * Copyright (C) 2009-2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; only
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <linux/fdtable.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <lttng-events.h>
#include <lttng-tracer.h>
#include <wrapper/irqflags.h>
#include <wrapper/ringbuffer/frontend_types.h>
#include <wrapper/uprobes.h>
#include <wrapper/vmalloc.h>

static
int lttng_uprobes_handler_pre(struct uprobe_consumer *uc, struct pt_regs *regs)
{
	struct lttng_uprobe_handler *uprobe_handler =
		container_of(uc, struct lttng_uprobe_handler, up_consumer);
	struct lttng_event *event = uprobe_handler->event;
	struct lttng_probe_ctx lttng_probe_ctx = {
		.event = event,
		.interruptible = !lttng_regs_irqs_disabled(regs),
	};
	struct lttng_channel *chan = event->chan;
	struct lib_ring_buffer_ctx ctx;
	int ret;

	struct {
		unsigned long ip;
	} payload;

	if (unlikely(!ACCESS_ONCE(chan->session->active)))
		return 0;
	if (unlikely(!ACCESS_ONCE(chan->enabled)))
		return 0;
	if (unlikely(!ACCESS_ONCE(event->enabled)))
		return 0;

	lib_ring_buffer_ctx_init(&ctx, chan->chan, &lttng_probe_ctx,
		sizeof(payload), lttng_alignof(payload), -1);

	ret = chan->ops->event_reserve(&ctx, event->id);
	if (ret < 0)
		return 0;

	/* Event payload. */
	payload.ip = regs->ip;

	lib_ring_buffer_align_ctx(&ctx, lttng_alignof(payload));
	chan->ops->event_write(&ctx, &payload, sizeof(payload));
	chan->ops->event_commit(&ctx);
	return 0;
}

/*
 * Create event description.
 */
static
int lttng_create_uprobe_event(const char *name, struct lttng_event *event)
{
	struct lttng_event_desc *desc;
	struct lttng_event_field *fields;
	int ret;

	desc = kzalloc(sizeof(*event->desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;
	desc->name = kstrdup(name, GFP_KERNEL);
	if (!desc->name) {
		ret = -ENOMEM;
		goto error_str;
	}

	desc->nr_fields = 1;
	desc->fields = fields =
		kzalloc(1 * sizeof(struct lttng_event_field), GFP_KERNEL);

	if (!desc->fields) {
		ret = -ENOMEM;
		goto error_fields;
	}
	fields[0].name = "ip";
	fields[0].type.atype = atype_integer;
	fields[0].type.u.basic.integer.size = sizeof(unsigned long) * CHAR_BIT;
	fields[0].type.u.basic.integer.alignment = lttng_alignof(unsigned long) * CHAR_BIT;
	fields[0].type.u.basic.integer.signedness = lttng_is_signed_type(unsigned long);
	fields[0].type.u.basic.integer.reverse_byte_order = 0;
	fields[0].type.u.basic.integer.base = 16;
	fields[0].type.u.basic.integer.encoding = lttng_encode_none;

	desc->owner = THIS_MODULE;
	event->desc = desc;

	return 0;

error_fields:
	kfree(desc->name);
error_str:
	kfree(desc);
	return ret;
}

/*
 * Returns the inode struct from the current task and an fd. The inode is
 * grabbed by this function and must be put once we are done with it using
 * iput().
 */
static struct inode *get_inode_from_fd(int fd)
{
	struct file *file;
	struct inode *inode;

	rcu_read_lock();
	/*
	 * Returns the file backing the given fd. Needs to be done inside an RCU
	 * critical section.
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

int lttng_uprobes_add_callsite(struct lttng_event *event,
	struct lttng_kernel_event_callsite_uprobe *callsite)
{
	int ret = 0;
	struct lttng_uprobe_handler *uprobe_handler;

	if (!event) {
		ret = -EINVAL;
		goto end;
	}

	uprobe_handler = kzalloc(sizeof(struct lttng_uprobe_handler), GFP_KERNEL);
	if (!uprobe_handler) {
		printk(KERN_WARNING "Error allocating uprobe_uprobe_handlers");
		ret = -ENOMEM;
		goto alloc_error;
	}

	/* If it's the first callsite for this event, init the list */
	if (event->u.uprobe.handler_list == NULL) {
		event->u.uprobe.handler_list = uprobe_handler;
		INIT_LIST_HEAD(&event->u.uprobe.handler_list->list);
	} else {
		list_add(&uprobe_handler->list, &event->u.uprobe.handler_list->list);
	}

	/* Ensure the memory we just allocated don't trigger page faults. */
	wrapper_vmalloc_sync_all();

	uprobe_handler->event = event;
	uprobe_handler->up_consumer.handler = lttng_uprobes_handler_pre;
	uprobe_handler->offset = callsite->offset;

	ret = wrapper_uprobe_register(event->u.uprobe.inode,
			      uprobe_handler->offset, &uprobe_handler->up_consumer);
	if (ret) {
		printk(KERN_WARNING "Error registering probe on inode %lu "
		       "and offset 0x%llx\n", event->u.uprobe.inode->i_ino,
		       uprobe_handler->offset);
		ret = -1;
		goto register_error;
	}
	return ret;

register_error:
	kfree(uprobe_handler);
alloc_error:
end:
	return ret;
}
EXPORT_SYMBOL_GPL(lttng_uprobes_add_callsite);

int lttng_uprobes_register(const char *name, int fd, struct lttng_event *event)
{
	int ret = 0;
	struct inode *inode;

	ret = lttng_create_uprobe_event(name, event);
	if (ret)
		goto error;

	inode = get_inode_from_fd(fd);
	if (!inode) {
		printk(KERN_WARNING "Cannot get inode from fd\n");
		ret = -EBADF;
		goto inode_error;
	}
	event->u.uprobe.inode = inode;

	return 0;

inode_error:
	kfree(event->desc->name);
	kfree(event->desc);
error:
	return ret;
}
EXPORT_SYMBOL_GPL(lttng_uprobes_register);

void lttng_uprobes_unregister(struct lttng_event *event)
{
	struct lttng_uprobe_handler *curr, *tmp, *last_handler;

	/*
	 * Verify that any handler were register for this event. This is needed
	 * because the enabling of the has two phases. First, we register the
	 * event using the FD and we later register callsites with offset in
	 * the previously passed FD.
	 */
	if (event->u.uprobe.handler_list == NULL)
		return;

	/*
	 * Iterate over the list of handler, remove each handler from the list
	 * and free the struct.
	 */
	list_for_each_entry_safe(curr, tmp, &event->u.uprobe.handler_list->list, list) {
		wrapper_uprobe_unregister(event->u.uprobe.inode, curr->offset,
			&curr->up_consumer);
		list_del(&curr->list);
		kfree(curr);
	}
	/*
	 * The list is currently only made of the head. unregister the uprobe
	 * and free the object.
	 */
	last_handler = event->u.uprobe.handler_list;
	wrapper_uprobe_unregister(event->u.uprobe.inode, last_handler->offset,
		&last_handler->up_consumer);

	/* Release reference to inode to free last handler */
	kfree(last_handler);

	event->u.uprobe.handler_list = NULL;
}
EXPORT_SYMBOL_GPL(lttng_uprobes_unregister);

void lttng_uprobes_destroy_private(struct lttng_event *event)
{
	iput(event->u.uprobe.inode);
	kfree(event->desc->name);
	kfree(event->desc);
}
EXPORT_SYMBOL_GPL(lttng_uprobes_destroy_private);

MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Yannick Brosseau");
MODULE_DESCRIPTION("Linux Trace Toolkit Uprobes Support");
