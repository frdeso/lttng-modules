/* SPDX-License-Identifier: (GPL-2.0-only or LGPL-2.1-only)
 *
 * lttng-trigger-notification.c
 *
 * Copyright (C) 2020 Francis Deslauriers <francis.deslauriers@efficios.com>
 */

#include <lttng/events.h>
#include <lttng/trigger-notification.h>

void lttng_trigger_notification_send(struct lttng_trigger *trigger)
{
	struct lttng_trigger_group *trigger_group = trigger->group;
	struct lib_ring_buffer_ctx ctx;
	int ret;

	if (unlikely(!READ_ONCE(trigger->enabled)))
		return;

	lib_ring_buffer_ctx_init(&ctx, trigger_group->chan, NULL, sizeof(trigger->id),
			lttng_alignof(trigger->id), -1);
	ret = trigger_group->ops->event_reserve(&ctx, 0);
	if (ret < 0) {
		//TODO: error handling with counter maps
		//silently drop for now.
		WARN_ON_ONCE(1);
		return;
	}
	lib_ring_buffer_align_ctx(&ctx, lttng_alignof(trigger->id));
	trigger_group->ops->event_write(&ctx, &trigger->id, sizeof(trigger->id));
	trigger_group->ops->event_commit(&ctx);
	irq_work_queue(&trigger_group->wakeup_pending);
}
