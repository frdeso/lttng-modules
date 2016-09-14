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

#include <linux/module.h>
#include "../wrapper/uprobes.h"
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include "../lttng-events.h"
#include "../wrapper/ringbuffer/frontend_types.h"
#include "../wrapper/vmalloc.h"
#include <wrapper/irqflags.h>
#include "../lttng-tracer.h"

static
int lttng_uprobes_handler_pre(struct uprobe_consumer *uc, struct pt_regs *regs)
{
	struct lttng_event *event =
		container_of(uc, struct lttng_event, u.uprobe.up_consumer);
	struct lttng_probe_ctx lttng_probe_ctx = {
		.event = event,
		.interruptible = !lttng_regs_irqs_disabled(regs),
	};
	struct lttng_channel *chan = event->chan;
	struct lib_ring_buffer_ctx ctx;
	int ret;

	printk(KERN_WARNING "%s-1 %u\n", __func__, event->id);
	if (unlikely(!ACCESS_ONCE(chan->session->active)))
		return 0;
	if (unlikely(!ACCESS_ONCE(chan->enabled)))
		return 0;
	if (unlikely(!ACCESS_ONCE(event->enabled)))
		return 0;

	struct {
		int a;
	 } payload;

	printk(KERN_WARNING "%s-2\n", __func__);
	lib_ring_buffer_ctx_init(&ctx, chan->chan, &lttng_probe_ctx, sizeof(payload),
			lttng_alignof(payload), -1);

	printk(KERN_WARNING "%s-3\n", __func__);
	ret = chan->ops->event_reserve(&ctx, event->id);
	printk(KERN_WARNING "%s-4\n", __func__);
	if (ret < 0)
		return 0;
	payload.a = 1337;
	printk(KERN_WARNING "%s-5\n", __func__);
	lib_ring_buffer_align_ctx(&ctx, lttng_alignof(payload));
	chan->ops->event_write(&ctx, &payload, sizeof(payload));
	chan->ops->event_commit(&ctx);
	return 0;
}

/*
 * Create event description
 */
static
int lttng_create_uprobe_event(const char *name, struct lttng_event *event)
{
	struct lttng_event_desc *desc;
	int ret;
	printk(KERN_WARNING "%s\n", __func__);

	desc = kzalloc(sizeof(*event->desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;
	desc->name = kstrdup(name, GFP_KERNEL);
	if (!desc->name) {
		ret = -ENOMEM;
		goto error_str;
	}
	desc->owner = THIS_MODULE;
	event->desc = desc;

	return 0;

error_str:
	kfree(desc);
	return ret;
}

int lttng_uprobes_register(const char *name,
			   const char *path_name,
			   uint64_t offset,
			   struct lttng_event *event)
{
	int ret;

	printk(KERN_WARNING "A offset:%d\n", offset);
	printk(KERN_WARNING "A name:%s\n",name);
	printk(KERN_WARNING "A path:%s\n",path_name);

//	if (!access_ok(VERIFY_READ, path_name, 1)){
//		printk(KERN_WARNING "AA\n");
//		return -EFAULT;
//	}

	printk(KERN_WARNING "AAA\n");
	if (path_name[0] == '\0')
		path_name = NULL;

	ret = lttng_create_uprobe_event(name, event);
	printk(KERN_WARNING "B\n");
	if (ret)
		goto error;
	memset(&event->u.uprobe.up_consumer, 0,
		sizeof(event->u.uprobe.up_consumer));
	event->u.uprobe.up_consumer.handler = lttng_uprobes_handler_pre;
	if (path_name) {
		struct path path;
		ret = kern_path(path_name, LOOKUP_FOLLOW, &path);
		if (ret)
			goto path_error;

		event->u.uprobe.inode = igrab(path.dentry->d_inode);
	}
	printk(KERN_WARNING "C\n");
	event->u.uprobe.offset = offset;

	 /* Ensure the memory we just allocated don't trigger page faults. */
	wrapper_vmalloc_sync_all();
	printk(KERN_DEBUG"Registering probe on inode %llu and offset %llu\n", event->u.uprobe.inode, event->u.uprobe.offset);
	ret = wrapper_uprobe_register(event->u.uprobe.inode,
			event->u.uprobe.offset,
			&event->u.uprobe.up_consumer);
	if (ret)
		goto register_error;
	return 0;

register_error:
	printk(KERN_DEBUG"Register error\n");
	iput(event->u.uprobe.inode);
path_error:
	kfree(event->desc->name);
	kfree(event->desc);
error:
	return ret;
}
EXPORT_SYMBOL_GPL(lttng_uprobes_register);

void lttng_uprobes_unregister(struct lttng_event *event)
{
	printk(KERN_WARNING "%s\n", __func__);
	wrapper_uprobe_unregister(event->u.uprobe.inode,
			event->u.uprobe.offset,
			&event->u.uprobe.up_consumer);
}
EXPORT_SYMBOL_GPL(lttng_uprobes_unregister);

void lttng_uprobes_destroy_private(struct lttng_event *event)
{
	printk(KERN_WARNING "%s\n", __func__);
	iput(event->u.uprobe.inode);
	kfree(event->desc->name);
	kfree(event->desc);
}
EXPORT_SYMBOL_GPL(lttng_uprobes_destroy_private);

MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Yannick Brosseau");
MODULE_DESCRIPTION("Linux Trace Toolkit Uprobes Support");
