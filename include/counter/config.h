/* SPDX-License-Identifier: (GPL-2.0-only or LGPL-2.1-only)
 *
 * counter/config.h
 *
 * LTTng Counters Configuration
 *
 * Copyright (C) 2020 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef _LTTNG_COUNTER_CONFIG_H
#define _LTTNG_COUNTER_CONFIG_H

#include <linux/types.h>
#include <linux/percpu.h>

enum lib_counter_config_alloc {
	/*
	 * For per-cpu allocation, counters are allocated for
	 * each CPU, plus one extra for the global sum, which
	 * can also be used for restartable sequences fallback.
	 */
	COUNTER_ALLOC_PER_CPU,
	COUNTER_ALLOC_GLOBAL,
};

enum lib_counter_config_sync {
	COUNTER_SYNC_PER_CPU,
	COUNTER_SYNC_GLOBAL,
};

struct lib_counter_config {
	enum lib_counter_config_alloc alloc;
	enum lib_counter_config_sync sync;
	enum {
		COUNTER_ARITHMETIC_OVERFLOW,
		COUNTER_ARITHMETIC_SATURATE,	/* TODO */
	} arithmetic;
	enum {
		COUNTER_SIZE_8_BIT	= 1,
		COUNTER_SIZE_16_BIT	= 2,
		COUNTER_SIZE_32_BIT	= 4,
		COUNTER_SIZE_64_BIT	= 8,
	} counter_size;
};

#endif /* _LTTNG_COUNTER_CONFIG_H */
