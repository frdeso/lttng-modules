/* SPDX-License-Identifier: (GPL-2.0-only or LGPL-2.1-only)
 *
 * counter/counter.h
 *
 * LTTng Counters API
 *
 * Copyright (C) 2020 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef _LTTNG_COUNTER_H
#define _LTTNG_COUNTER_H

#include <linux/types.h>
#include <linux/percpu.h>
#include <counter/config.h>
#include <counter/counter-types.h>

struct lib_counter *lttng_counter_create(const struct lib_counter_config *config,
					 size_t nr_dimensions,
					 struct lib_counter_dimension *dimensions);
void lttng_counter_destroy(struct lib_counter *counter);

int lttng_counter_set_global_sum_step(struct lib_counter *counter,
				      long global_sum_step);

int lttng_counter_read(const struct lib_counter_config *config,
		      struct lib_counter *counter,
		      size_t *dimension_indexes,
		      int cpu, int64_t *value,
		      bool *overflow, bool *underflow);
int lttng_counter_aggregate(const struct lib_counter_config *config,
			    struct lib_counter *counter,
			    size_t *dimension_indexes,
			    int64_t *value,
			    bool *overflow, bool *underflow);

#endif /* _LTTNG_COUNTER_H */
