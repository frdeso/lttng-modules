/* SPDX-License-Identifier: (GPL-2.0 or LGPL-2.1)
 *
 * lttng-ring-buffer-trigger-client.c
 *
 * LTTng lib ring buffer trigger client.
 *
 * Copyright (C) 2010-2020 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <linux/module.h>
#include <lttng/tracer.h>

#define RING_BUFFER_MODE_TEMPLATE		RING_BUFFER_DISCARD
#define RING_BUFFER_MODE_TEMPLATE_STRING	"trigger"
#define RING_BUFFER_OUTPUT_TEMPLATE		RING_BUFFER_NONE
#include "lttng-ring-buffer-trigger-client.h"
