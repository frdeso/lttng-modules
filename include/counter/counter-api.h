/* SPDX-License-Identifier: (GPL-2.0-only or LGPL-2.1-only)
 *
 * counter/counter-api.h
 *
 * LTTng Counters API, requiring counter/config.h
 *
 * Copyright (C) 2020 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef _LTTNG_COUNTER_API_H
#define _LTTNG_COUNTER_API_H

#include <linux/types.h>
#include <linux/percpu.h>
#include <linux/bitops.h>
#include <counter/counter.h>
#include <counter/counter-internal.h>

static inline int64_t lttng_counter_underflow_index(struct lib_counter_dimension *dimension)
{
	return dimension->max_nr_elem + 1;
}

static inline int64_t lttng_counter_overflow_index(struct lib_counter_dimension *dimension)
{
	return dimension->max_nr_elem + 2;
}

/* Update dimension_indexes if index is outside range. */
static inline void lttng_counter_validate_indexes(const struct lib_counter_config *config,
						  struct lib_counter *counter,
						  int64_t *dimension_indexes)
{
	size_t nr_dimensions = counter->nr_dimensions, i;

	for (i = 0; i < nr_dimensions; i++) {
		struct lib_counter_dimension *dimension = &counter->dimensions[i];
		int64_t *dimension_index = &dimension_indexes[i];

		if (unlikely(*dimension_index < 0))
			*dimension_index = lttng_counter_underflow_index(dimension);
		else if (unlikely(*dimension_index >= counter->dimensions[i].max_nr_elem))
			*dimension_index = lttng_counter_overflow_index(dimension);
	}
}

/*
 * Returns the amount which should be summed into global counter.
 * Using unsigned arithmetic because overflow is defined.
 */
static inline int64_t __lttng_counter_add(const struct lib_counter_config *config,
				       enum lib_counter_config_alloc alloc,
				       enum lib_counter_config_sync sync,
				       struct lib_counter *counter,
				       int64_t *dimension_indexes, int64_t v)
{
	int64_t index = lttng_counter_get_index(config, counter, dimension_indexes);
	bool overflow = false, underflow = false;
	struct lib_counter_layout *layout;
	int64_t move_sum = 0;

	if (unlikely(index >= counter->allocated_elem)) {
		WARN_ON_ONCE(1);
		return 0;
	}
	switch (alloc) {
	case COUNTER_ALLOC_PER_CPU:
		layout = per_cpu_ptr(counter->percpu_counters, smp_processor_id());
		break;
	case COUNTER_ALLOC_GLOBAL:
		layout = &counter->global_counters;
		break;
	}
	switch (config->counter_size) {
	case COUNTER_SIZE_8_BIT:
	{
		int8_t *int_p = (int8_t *) layout->counters + index;
		int8_t old, n, res;
		int8_t global_sum_step = counter->global_sum_step.s8;

		res = *int_p;
		switch (sync) {
		case COUNTER_SYNC_PER_CPU:
		{
			do {
				move_sum = 0;
				old = res;
				n = (int8_t) ((uint8_t) old + (uint8_t) v);
				if (unlikely(n > (int8_t) global_sum_step))
					move_sum = (int8_t) global_sum_step / 2;
				else if (unlikely(n < -(int8_t) global_sum_step))
					move_sum = -((int8_t) global_sum_step / 2);
				n -= move_sum;
				res = cmpxchg_local(int_p, old, n);
			} while (old != res);
			break;
		}
		case COUNTER_SYNC_GLOBAL:
		{
			do {
				old = res;
				n = (int8_t) ((uint8_t) old + (uint8_t) v);
				res = cmpxchg(int_p, old, n);
			} while (old != res);
			break;
		}
		}
		if (v > 0 && (v >= U8_MAX || n < old))
			overflow = true;
		else if (v < 0 && (v <= -U8_MAX || n > old))
			underflow = true;
		break;
	}
	case COUNTER_SIZE_16_BIT:
	{
		int16_t *int_p = (int16_t *) layout->counters + index;
		int16_t old, n, res;
		int16_t global_sum_step = counter->global_sum_step.s16;

		res = *int_p;
		switch (sync) {
		case COUNTER_SYNC_PER_CPU:
		{
			do {
				move_sum = 0;
				old = res;
				n = (int16_t) ((uint16_t) old + (uint16_t) v);
				if (unlikely(n > (int16_t) global_sum_step))
					move_sum = (int16_t) global_sum_step / 2;
				else if (unlikely(n < -(int16_t) global_sum_step))
					move_sum = -((int16_t) global_sum_step / 2);
				n -= move_sum;
				res = cmpxchg_local(int_p, old, n);
			} while (old != res);
			break;
		}
		case COUNTER_SYNC_GLOBAL:
		{
			do {
				old = res;
				n = (int16_t) ((uint16_t) old + (uint16_t) v);
				res = cmpxchg(int_p, old, n);
			} while (old != res);
			break;
		}
		}
		if (v > 0 && (v >= U16_MAX || n < old))
			overflow = true;
		else if (v < 0 && (v <= -U16_MAX || n > old))
			underflow = true;
		break;
	}
	case COUNTER_SIZE_32_BIT:
	{
		int32_t *int_p = (int32_t *) layout->counters + index;
		int32_t old, n, res;
		int32_t global_sum_step = counter->global_sum_step.s32;

		res = *int_p;
		switch (sync) {
		case COUNTER_SYNC_PER_CPU:
		{
			do {
				move_sum = 0;
				old = res;
				n = (int32_t) ((uint32_t) old + (uint32_t) v);
				if (unlikely(n > (int32_t) global_sum_step))
					move_sum = (int32_t) global_sum_step / 2;
				else if (unlikely(n < -(int32_t) global_sum_step))
					move_sum = -((int32_t) global_sum_step / 2);
				n -= move_sum;
				res = cmpxchg_local(int_p, old, n);
			} while (old != res);
			break;
		}
		case COUNTER_SYNC_GLOBAL:
		{
			do {
				old = res;
				n = (int32_t) ((uint32_t) old + (uint32_t) v);
				res = cmpxchg(int_p, old, n);
			} while (old != res);
			break;
		}
		}
		if (v > 0 && (v >= U32_MAX || n < old))
			overflow = true;
		else if (v < 0 && (v <= -U32_MAX || n > old))
			underflow = true;
		break;
	}
#if BITS_PER_LONG == 64
	case COUNTER_SIZE_64_BIT:
	{
		int64_t *int_p = (int64_t *) layout->counters + index;
		int64_t old, n, res;
		int64_t global_sum_step = counter->global_sum_step.s64;

		res = *int_p;
		switch (sync) {
		case COUNTER_SYNC_PER_CPU:
		{
			do {
				move_sum = 0;
				old = res;
				n = (int64_t) ((uint64_t) old + (uint64_t) v);
				if (unlikely(n > (int64_t) global_sum_step))
					move_sum = (int64_t) global_sum_step / 2;
				else if (unlikely(n < -(int64_t) global_sum_step))
					move_sum = -((int64_t) global_sum_step / 2);
				n -= move_sum;
				res = cmpxchg_local(int_p, old, n);
			} while (old != res);
			break;
		}
		case COUNTER_SYNC_GLOBAL:
		{
			do {
				old = res;
				n = (int64_t) ((uint64_t) old + (uint64_t) v);
				res = cmpxchg(int_p, old, n);
			} while (old != res);
			break;
		}
		}
		if (v > 0 && n < old)
			overflow = true;
		else if (v < 0 && n > old)
			underflow = true;
		break;
	}
#endif
	default:
		WARN_ON_ONCE(1);
	}
	if (unlikely(overflow && !test_bit(index, layout->overflow_bitmap)))
		set_bit(index, layout->overflow_bitmap);
	else if (unlikely(underflow && !test_bit(index, layout->underflow_bitmap)))
		set_bit(index, layout->underflow_bitmap);
	return move_sum;
}

static inline void __lttng_counter_add_percpu(const struct lib_counter_config *config,
				     struct lib_counter *counter,
				     int64_t *dimension_indexes, int64_t v)
{
	int64_t move_sum;

	move_sum = __lttng_counter_add(config, config->alloc, config->sync,
				       counter, dimension_indexes, v);
	if (unlikely(move_sum))
		(void) __lttng_counter_add(config, COUNTER_ALLOC_GLOBAL, COUNTER_SYNC_GLOBAL,
					   counter, dimension_indexes, move_sum);
}

static inline void __lttng_counter_add_global(const struct lib_counter_config *config,
				     struct lib_counter *counter,
				     int64_t *dimension_indexes, int64_t v)
{
	(void) __lttng_counter_add(config, config->alloc, config->sync, counter,
				   dimension_indexes, v);
}

static inline void lttng_counter_add(const struct lib_counter_config *config,
				     struct lib_counter *counter,
				     int64_t *dimension_indexes, int64_t v)
{
	switch (config->alloc) {
	case COUNTER_ALLOC_PER_CPU:
		__lttng_counter_add_percpu(config, counter, dimension_indexes, v);
		break;
	case COUNTER_ALLOC_GLOBAL:
		__lttng_counter_add_global(config, counter, dimension_indexes, v);
		break;
	}
}

static inline void lttng_counter_inc(const struct lib_counter_config *config,
				     struct lib_counter *counter,
				     int64_t *dimension_indexes)
{
	lttng_counter_add(config, counter, dimension_indexes, 1);
}

static inline void lttng_counter_dec(const struct lib_counter_config *config,
				     struct lib_counter *counter,
				     int64_t *dimension_indexes)
{
	lttng_counter_add(config, counter, dimension_indexes, -1);
}

#endif /* _LTTNG_COUNTER_API_H */
