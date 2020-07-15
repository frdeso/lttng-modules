/* SPDX-License-Identifier: (GPL-2.0-only or LGPL-2.1-only)
 *
 * counter/counter-types.h
 *
 * LTTng Counters Types
 *
 * Copyright (C) 2020 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef _LTTNG_COUNTER_TYPES_H
#define _LTTNG_COUNTER_TYPES_H

#include <linux/types.h>
#include <linux/percpu.h>
#include <counter/config.h>

struct lib_counter_dimension {
	/*
	 * Max. number of indexable elements.
	 *
	 * Two additional elements are added after the last index of
	 * each dimension to keep track of index underflow and overflow
	 * for each dimension (later refered to as "underflow index" and
	 * "overflow index"). If the requested index is smaller than 0,
	 * the underflow index in used. If the requested index goes
	 * beyond the maximum number of indexable elements for a
	 * dimension, the overflow index is used.
	 *
	 * The allocated size for a dimension is max_nr_elem + 2 to keep
	 * room for the underflow and overflow indexes.
	 */
	size_t max_nr_elem;
	/*
	 * The stride for a dimension is the multiplication factor which
	 * should be applied to its index to take into account other
	 * dimensions nested inside.
	 */
	size_t stride;
};

struct lib_counter_layout {
	void *counters;	/* union v_atomic elements. */
	void *underflow_bitmap;
	void *overflow_bitmap;
};

enum lib_counter_arithmetic {
	LIB_COUNTER_ARITHMETIC_OVERFLOW,
	LIB_COUNTER_ARITHMETIC_SATURATE,
};

struct lib_counter {
	size_t nr_dimensions;
	struct lib_counter_dimension *dimensions;
	enum lib_counter_arithmetic arithmetic;
	union {
		struct {
			int32_t max, min;
		} limits_32_bit;
		struct {
			int64_t max, min;
		} limits_64_bit;
	} saturation;
	long split_counter_global_sum_step;		/* 0 if unused */
	struct lib_counter_config config;

	struct lib_counter_layout global_counters;
	struct lib_counter_layout __percpu *percpu_counters;
};

#endif /* _LTTNG_COUNTER_TYPES_H */
