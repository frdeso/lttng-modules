/*
 * probes/lttng-uretprobes.c
 *
 * LTTng uprobes integration module.
 *
 * Copyright (C) 2017 Francis Deslauriers <francis.deslauriers@efficios.com>
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

#include <linux/module.h>
#include <lttng-events.h>
#include <linux/slab.h>
#include <wrapper/ringbuffer/frontend_types.h>
#include <wrapper/vmalloc.h>
#include <wrapper/irqflags.h>
#include <wrapper/uprobes.h>
#include <lttng-tracer.h>

#include "lttng-probe-utils.h"

enum lttng_uretprobe_type {
	EVENT_ENTRY	= 0,
	EVENT_RETURN	= 1,
};

struct lttng_urp {
	struct uprobe_consumer up_consumer;
	struct lttng_event *event[2];
	off_t offset;
	struct inode *inode;
	struct kref kref_register;
	struct kref kref_inode;
};

static
int _lttng_uretprobes_handler(struct uprobe_consumer *uc, struct pt_regs *regs,
					 enum lttng_uretprobe_type type)
{
	struct lttng_urp *urp = container_of(uc, struct lttng_urp, up_consumer);
	struct lttng_event *event = urp->event[type];
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

	/* Event payload.*/
	payload.ip = regs->ip;

	lib_ring_buffer_align_ctx(&ctx, lttng_alignof(payload));
	chan->ops->event_write(&ctx, &payload, sizeof(payload));
	chan->ops->event_commit(&ctx);
	return 0;
}

static
int lttng_uretprobes_handler_entry(struct uprobe_consumer *uc,
				   struct pt_regs *regs)
{
	return _lttng_uretprobes_handler(uc, regs, EVENT_ENTRY);
}

static
int lttng_uretprobes_handler_return(struct uprobe_consumer *uc,
					unsigned long func,
					struct pt_regs *regs)
{
	return _lttng_uretprobes_handler(uc, regs, EVENT_RETURN);
}

/*
 * Create event description.
 */
static
int lttng_create_uprobe_event(const char *name, struct lttng_event *event,
					  enum lttng_uretprobe_type type)
{
	struct lttng_event_desc *desc;
	struct lttng_event_field *fields;
	int ret;
	char *suffix, *alloc_name;
	size_t name_len;

	desc = kzalloc(sizeof(*event->desc), GFP_KERNEL);
	if (!desc) {
		ret = -ENOMEM;
		goto error_desc;
	}

	/*
	 * Append the event type to the name provided by the user
	 */
	name_len = strlen(name);
	switch (type) {
	case EVENT_ENTRY:
		suffix = "_entry";
		break;
	case EVENT_RETURN:
		suffix = "_return";
		break;
	default:
		ret = -1;
		goto error_str;
	}

	name_len += strlen(suffix);
	alloc_name = kmalloc(name_len + 1, GFP_KERNEL);
	if (!alloc_name) {
		ret = -ENOMEM;
		goto error_str;
	}
	strcpy(alloc_name, name);
	strcat(alloc_name, suffix);
	desc->name = alloc_name;

	desc->nr_fields = 1;
	desc->fields = fields =
		kzalloc(desc->nr_fields * sizeof(struct lttng_event_field),
			GFP_KERNEL);

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
error_desc:
	return ret;
}

int lttng_uretprobes_register(const char *name,
			   int fd,
			   uint64_t offset,
			   struct lttng_event *event_entry,
			   struct lttng_event *event_return)
{
	int ret;
	struct inode *inode;
	struct lttng_urp *lttng_urp;

	/*
	 * Create an event for both the entry and the return of the target
	 * function.
	 */
	ret = lttng_create_uprobe_event(name, event_entry, EVENT_ENTRY);
	if (ret)
		goto error;
	ret = lttng_create_uprobe_event(name, event_return, EVENT_RETURN);
	if (ret)
		goto event_return_error;

	lttng_urp = kzalloc(sizeof(*lttng_urp), GFP_KERNEL);
	if (!lttng_urp)
		goto urp_error;

	lttng_urp->event[EVENT_ENTRY] = event_entry;
	lttng_urp->event[EVENT_RETURN] = event_return;
	lttng_urp->up_consumer.handler = lttng_uretprobes_handler_entry;
	lttng_urp->up_consumer.ret_handler = lttng_uretprobes_handler_return;
	lttng_urp->offset = offset;

	inode = lttng_get_inode_from_fd(fd);
	if (!inode) {
		printk(KERN_WARNING "Cannot get inode from fd\n");
		ret = -EBADF;
		goto inode_error;
	}

	lttng_urp->inode = inode;

	event_entry->u.uretprobe.lttng_urp = lttng_urp;
	event_return->u.uretprobe.lttng_urp = lttng_urp;

	/*
	 * Both events must be unregistered before the uretprobe is
	 * unregistered. Same for memory allocation.
	 */
	kref_init(&lttng_urp->kref_inode);
	kref_init(&lttng_urp->kref_register);

	/* inc refcount to 2, no overflow. */
	kref_get(&lttng_urp->kref_inode);
	/* inc refcount to 2, no overflow. */
	kref_get(&lttng_urp->kref_register);

	 /* Ensure the memory we just allocated don't trigger page faults. */
	wrapper_vmalloc_sync_all();
	ret = wrapper_uprobe_register(lttng_urp->inode,
			lttng_urp->offset,
			&lttng_urp->up_consumer);
	if (ret) {
		printk(KERN_WARNING "Error registering probe on inode %lu "
					"and offset %ld\n",
				lttng_urp->inode->i_ino, lttng_urp->offset);
		goto register_error;
	}
	return 0;

register_error:
	iput(lttng_urp->inode);
inode_error:
	kfree(lttng_urp);
urp_error:
	kfree(event_return->desc->fields);
	kfree(event_return->desc->name);
	kfree(event_return->desc);
event_return_error:
	kfree(event_entry->desc->fields);
	kfree(event_entry->desc->name);
	kfree(event_entry->desc);
error:
	return ret;
}
EXPORT_SYMBOL_GPL(lttng_uretprobes_register);

int lttng_uretprobes_event_enable_state(struct lttng_event *event,
		int enable)
{
	struct lttng_event *event_return;
	struct lttng_urp *lttng_urp;

	if (event->instrumentation != LTTNG_KERNEL_URETPROBE) {
		return -EINVAL;
	}
	if (event->enabled == enable) {
		return -EBUSY;
	}

	lttng_urp = event->u.uretprobe.lttng_urp;
	event_return = lttng_urp->event[EVENT_RETURN];
	ACCESS_ONCE(event->enabled) = enable;
	ACCESS_ONCE(event_return->enabled) = enable;
	return 0;
}
EXPORT_SYMBOL_GPL(lttng_uretprobes_event_enable_state);

static
void _lttng_uretprobes_unregister_release(struct kref *kref)
{
	struct lttng_urp *lttng_urp =
		container_of(kref, struct lttng_urp, kref_register);
	wrapper_uprobe_unregister(lttng_urp->inode,
				  lttng_urp->offset,
				  &lttng_urp->up_consumer);
}
void lttng_uretprobes_unregister(struct lttng_event *event)
{
	kref_put(&event->u.uretprobe.lttng_urp->kref_register,
		_lttng_uretprobes_unregister_release);
}
EXPORT_SYMBOL_GPL(lttng_uretprobes_unregister);

static
void _lttng_uretprobes_release(struct kref *kref)
{
	struct lttng_urp *lttng_urp =
		container_of(kref, struct lttng_urp, kref_inode);
	iput(lttng_urp->inode);
}
void lttng_uretprobes_destroy_private(struct lttng_event *event)
{
	kfree(event->desc->fields);
	kfree(event->desc->name);
	kfree(event->desc);
	kref_put(&event->u.uretprobe.lttng_urp->kref_inode,
		_lttng_uretprobes_release);
}
EXPORT_SYMBOL_GPL(lttng_uretprobes_destroy_private);

MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Francis Deslauriers");
MODULE_DESCRIPTION("Linux Trace Toolkit Uretprobes Support");
MODULE_VERSION(__stringify(LTTNG_MODULES_MAJOR_VERSION) "."
	__stringify(LTTNG_MODULES_MINOR_VERSION) "."
	__stringify(LTTNG_MODULES_PATCHLEVEL_VERSION)
	LTTNG_MODULES_EXTRAVERSION);
