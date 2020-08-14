/* SPDX-License-Identifier: (GPL-2.0-only or LGPL-2.1-only)
 *
 * counter/counter-internal.h
 *
 * LTTng Counters Internal Header
 *
 * Copyright (C) 2020 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef _LTTNG_COUNTER_INTERNAL_H
#define _LTTNG_COUNTER_INTERNAL_H

#include <linux/types.h>
#include <linux/percpu.h>
#include <counter/counter-types.h>
#include <counter/config.h>

static inline int64_t lttng_counter_get_index(const struct lib_counter_config *config,
					     struct lib_counter *counter,
					     int64_t *dimension_indexes)
{
	size_t nr_dimensions = counter->nr_dimensions, i;
	int64_t index = 0;

	for (i = 0; i < nr_dimensions; i++) {
		struct lib_counter_dimension *dimension = &counter->dimensions[i];
		int64_t *dimension_index = &dimension_indexes[i];

		index += *dimension_index * dimension->stride;
	}
	return index;
}

#endif /* _LTTNG_COUNTER_INTERNAL_H */
