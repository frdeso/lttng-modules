/* SPDX-License-Identifier: (GPL-2.0-only OR LGPL-2.1-only)
 *
 * counter.c
 *
 * Copyright (C) 2020 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <linux/module.h>
#include <lttng/tracer.h>
#include <linux/cpumask.h>
#include <counter/counter.h>
#include <counter/counter-internal.h>
#include <wrapper/vmalloc.h>

/* Includes underflow and overflow elements. */
static size_t lttng_counter_get_dimension_nr_elements(struct lib_counter_dimension *dimension)
{
	return dimension->max_nr_elem + 2;
}

static void lttng_counter_init_stride(const struct lib_counter_config *config,
				      struct lib_counter *counter)
{
	size_t nr_dimensions = counter->nr_dimensions, stride = 1;
	ssize_t i;

	for (i = nr_dimensions - 1; i >= 0; i--) {
		struct lib_counter_dimension *dimension = &counter->dimensions[i];

		dimension->stride = stride;
		stride *= lttng_counter_get_dimension_nr_elements(dimension);
	}
}

static int lttng_counter_layout_init(struct lib_counter *counter, int cpu)
{
	struct lib_counter_layout *layout;
	size_t counter_size, nr_elem = counter->allocated_elem;

	if (cpu == -1)
		layout = &counter->global_counters;
	else
		layout = per_cpu_ptr(counter->percpu_counters, cpu);
	switch (counter->config.counter_size) {
	case COUNTER_SIZE_8_BIT:
	case COUNTER_SIZE_16_BIT:
	case COUNTER_SIZE_32_BIT:
	case COUNTER_SIZE_64_BIT:
		counter_size = (size_t) counter->config.counter_size;
		break;
	default:
		return -EINVAL;
	}
	layout->counters = lttng_kvzalloc_node(ALIGN(counter_size * nr_elem,
						     1 << INTERNODE_CACHE_SHIFT),
					       GFP_KERNEL | __GFP_NOWARN,
					       cpu_to_node(max(cpu, 0)));
	if (!layout->counters)
		return -ENOMEM;
	layout->overflow_bitmap = lttng_kvzalloc_node(ALIGN(ALIGN(nr_elem, 8) / 8,
						     1 << INTERNODE_CACHE_SHIFT),
					       GFP_KERNEL | __GFP_NOWARN,
					       cpu_to_node(max(cpu, 0)));
	if (!layout->overflow_bitmap)
		return -ENOMEM;
	layout->underflow_bitmap = lttng_kvzalloc_node(ALIGN(ALIGN(nr_elem, 8) / 8,
						     1 << INTERNODE_CACHE_SHIFT),
					       GFP_KERNEL | __GFP_NOWARN,
					       cpu_to_node(max(cpu, 0)));
	if (!layout->underflow_bitmap)
		return -ENOMEM;
	return 0;
}

static void lttng_counter_layout_fini(struct lib_counter *counter, int cpu)
{
	struct lib_counter_layout *layout;

	if (cpu == -1)
		layout = &counter->global_counters;
	else
		layout = per_cpu_ptr(counter->percpu_counters, cpu);

	lttng_kvfree(layout->counters);
	lttng_kvfree(layout->overflow_bitmap);
	lttng_kvfree(layout->underflow_bitmap);
}

static
int lttng_counter_set_global_sum_step(struct lib_counter *counter,
				      int64_t global_sum_step)
{
	if (global_sum_step < 0)
		return -EINVAL;

	switch (counter->config.counter_size) {
	case COUNTER_SIZE_8_BIT:
		if (global_sum_step > S8_MAX)
			return -EINVAL;
		counter->global_sum_step.s8 = (int8_t) global_sum_step;
		break;
	case COUNTER_SIZE_16_BIT:
		if (global_sum_step > S16_MAX)
			return -EINVAL;
		counter->global_sum_step.s16 = (int16_t) global_sum_step;
		break;
	case COUNTER_SIZE_32_BIT:
		if (global_sum_step > S32_MAX)
			return -EINVAL;
		counter->global_sum_step.s32 = (int32_t) global_sum_step;
		break;
	case COUNTER_SIZE_64_BIT:
		counter->global_sum_step.s64 = global_sum_step;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

struct lib_counter *lttng_counter_create(const struct lib_counter_config *config,
					 size_t nr_dimensions,
					 const size_t *max_nr_elem,
					 int64_t global_sum_step)
{
	struct lib_counter *counter;
	size_t dimension, nr_elem = 1;
	int cpu, ret;

	if (BITS_PER_LONG != 64 && config->counter_size == COUNTER_SIZE_64_BIT) {
		WARN_ON_ONCE(1);
		return NULL;
	}
	counter = kzalloc(sizeof(struct lib_counter), GFP_KERNEL);
	if (!counter)
		return NULL;
	counter->config = *config;
	if (lttng_counter_set_global_sum_step(counter, global_sum_step))
		goto error_sum_step;
	counter->nr_dimensions = nr_dimensions;
	counter->dimensions = kzalloc(nr_dimensions * sizeof(*counter->dimensions), GFP_KERNEL);
	if (!counter->dimensions)
		goto error_dimensions;
	for (dimension = 0; dimension < nr_dimensions; dimension++)
		counter->dimensions[dimension].max_nr_elem = max_nr_elem[dimension];
	if (config->alloc == COUNTER_ALLOC_PER_CPU) {
		counter->percpu_counters = alloc_percpu(struct lib_counter_layout);
		if (!counter->percpu_counters)
			goto error_alloc_percpu;
	}

	lttng_counter_init_stride(config, counter);
	//TODO saturation values.
	for (dimension = 0; dimension < counter->nr_dimensions; dimension++)
		nr_elem *= lttng_counter_get_dimension_nr_elements(&counter->dimensions[dimension]);
	counter->allocated_elem = nr_elem;
	ret = lttng_counter_layout_init(counter, -1);	/* global */
	if (ret)
		goto layout_init_error;
	//TODO: integrate with CPU hotplug and online cpus
	for (cpu = 0; cpu < num_possible_cpus(); cpu++) {
		ret = lttng_counter_layout_init(counter, cpu);
		if (ret)
			goto layout_init_error;
	}
	return counter;

layout_init_error:
	for (cpu = 0; cpu < num_possible_cpus(); cpu++)
		lttng_counter_layout_fini(counter, cpu);
	lttng_counter_layout_fini(counter, -1);
	free_percpu(counter->percpu_counters);
error_alloc_percpu:
	kfree(counter->dimensions);
error_dimensions:
error_sum_step:
	kfree(counter);
	return NULL;
}
EXPORT_SYMBOL_GPL(lttng_counter_create);

void lttng_counter_destroy(struct lib_counter *counter)
{
	int cpu;

	for (cpu = 0; cpu < num_possible_cpus(); cpu++)
		lttng_counter_layout_fini(counter, cpu);
	lttng_counter_layout_fini(counter, -1);
	free_percpu(counter->percpu_counters);
	kfree(counter->dimensions);
	kfree(counter);
}
EXPORT_SYMBOL_GPL(lttng_counter_destroy);

int lttng_counter_read(const struct lib_counter_config *config,
		       struct lib_counter *counter,
		       size_t *dimension_indexes,
		       int cpu, int64_t *value, bool *overflow,
		       bool *underflow)
{
	size_t index = lttng_counter_get_index(config, counter, dimension_indexes);
	struct lib_counter_layout *layout;

	if (unlikely(index >= counter->allocated_elem)) {
		WARN_ON_ONCE(1);
		return -EOVERFLOW;
	}
	switch (config->alloc) {
	case COUNTER_ALLOC_PER_CPU:
		if (cpu >= 0) {
			if (cpu >= num_possible_cpus())
				return -EINVAL;
			layout = per_cpu_ptr(counter->percpu_counters, cpu);
		} else {
			layout = &counter->global_counters;
		}
		break;
	case COUNTER_ALLOC_GLOBAL:
		if (cpu >= 0)
			return -EINVAL;
		layout = &counter->global_counters;
		break;
	default:
		return -EINVAL;
	}
	switch (config->counter_size) {
	case COUNTER_SIZE_8_BIT:
	{
		int8_t *int_p = (int8_t *) layout->counters + index;
		*value = (int64_t) READ_ONCE(*int_p);
		break;
	}
	case COUNTER_SIZE_16_BIT:
	{
		int16_t *int_p = (int16_t *) layout->counters + index;
		*value = (int64_t) READ_ONCE(*int_p);
		break;
	}
	case COUNTER_SIZE_32_BIT:
	{
		int32_t *int_p = (int32_t *) layout->counters + index;
		*value = (int64_t) READ_ONCE(*int_p);
		break;
	}
#if BITS_PER_LONG == 64
	case COUNTER_SIZE_64_BIT:
	{
		int64_t *int_p = (int64_t *) layout->counters + index;
		*value = READ_ONCE(*int_p);
		break;
	}
#endif
	default:
		WARN_ON_ONCE(1);
	}
	*overflow = test_bit(index, layout->overflow_bitmap);
	*underflow = test_bit(index, layout->underflow_bitmap);
	return 0;
}
EXPORT_SYMBOL_GPL(lttng_counter_read);

int lttng_counter_aggregate(const struct lib_counter_config *config,
			    struct lib_counter *counter,
			    size_t *dimension_indexes,
			    int64_t *value, bool *overflow,
			    bool *underflow)
{
	int cpu, ret;
	int64_t v, sum = 0;
	bool of, uf;

	*overflow = false;
	*underflow = false;
	/* Read global counter. */
	ret = lttng_counter_read(config, counter, dimension_indexes,
				 -1, &v, &of, &uf);
	if (ret < 0)
		return ret;
	sum += v;
	*overflow |= of;
	*underflow |= uf;

	switch (config->alloc) {
	case COUNTER_ALLOC_PER_CPU:
		//TODO: integrate with CPU hotplug and online cpus
		for (cpu = 0; cpu < num_possible_cpus(); cpu++) {
			int64_t old = sum;

			ret = lttng_counter_read(config, counter, dimension_indexes,
						 cpu, &v, &of, &uf);
			if (ret < 0)
				return ret;
			*overflow |= of;
			*underflow |= uf;
			/* Overflow is defined on unsigned types. */
			sum = (int64_t) ((uint64_t) old + (uint64_t) v);
			if (v > 0 && sum < old)
				*overflow = true;
			else if (v < 0 && sum > old)
				*underflow = true;
		}
		break;
	case COUNTER_ALLOC_GLOBAL:
		break;
	default:
		return -EINVAL;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(lttng_counter_aggregate);

MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Mathieu Desnoyers <mathieu.desnoyers@efficios.com>");
MODULE_DESCRIPTION("LTTng counter library");
MODULE_VERSION(__stringify(LTTNG_MODULES_MAJOR_VERSION) "."
	__stringify(LTTNG_MODULES_MINOR_VERSION) "."
	__stringify(LTTNG_MODULES_PATCHLEVEL_VERSION)
	LTTNG_MODULES_EXTRAVERSION);
