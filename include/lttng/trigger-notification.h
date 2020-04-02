/* SPDX-License-Identifier: (GPL-2.0-only or LGPL-2.1-only)
 *
 * lttng/trigger-notification.h
 *
 * Copyright (C) 2020 Francis Deslauriers <francis.deslauriers@efficios.com>
 */

#ifndef _LTTNG_TRIGGER_NOTIFICATION_H
#define _LTTNG_TRIGGER_NOTIFICATION_H

#include <lttng/events.h>

void lttng_trigger_notification_send(struct lttng_trigger *trigger,
		struct lttng_probe_ctx *lttng_probe_ctx,
		const char *stack_data);

#endif /* _LTTNG_TRIGGER_NOTIFICATION_H */
