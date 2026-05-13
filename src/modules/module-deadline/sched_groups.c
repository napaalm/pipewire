/*
 * sched_groups.c
 *
 * Implementation of the per-TID accumulator. See sched_groups.h
 * for the contract.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sched_groups.h"

/* Initial geometric-growth capacity. Starts at 0 and only the first
 * insert allocates the backing array; this keeps the per-impl memory
 * cost at zero when chain-merge is off and the accumulator is never
 * touched. */
#define SCHED_GROUPS_INITIAL_CAP	8u

void sched_groups_init(struct sched_groups *sg)
{
	if (sg == NULL)
		return;
	sg->entries = NULL;
	sg->count = 0;
	sg->cap = 0;
}

void sched_groups_fini(struct sched_groups *sg)
{
	if (sg == NULL)
		return;
	free(sg->entries);
	sg->entries = NULL;
	sg->count = 0;
	sg->cap = 0;
}

void sched_groups_reset(struct sched_groups *sg)
{
	if (sg == NULL)
		return;
	sg->count = 0;
}

struct sched_group *sched_groups_find_or_insert(struct sched_groups *sg, pid_t tid)
{
	uint32_t i;

	if (sg == NULL)
		return NULL;

	for (i = 0; i < sg->count; i++) {
		if (sg->entries[i].tid == tid)
			return &sg->entries[i];
	}

	if (sg->count == sg->cap) {
		uint32_t new_cap = sg->cap ? sg->cap * 2 : SCHED_GROUPS_INITIAL_CAP;
		struct sched_group *resized = realloc(sg->entries,
				(size_t)new_cap * sizeof(*resized));
		if (resized == NULL) {
			errno = ENOMEM;
			return NULL;
		}
		sg->entries = resized;
		sg->cap = new_cap;
	}

	struct sched_group *g = &sg->entries[sg->count++];
	g->tid = tid;
	g->leader_id = UINT32_MAX;
	g->sum_runtime = 0;
	g->sum_deadline = 0;
	g->period = 0;
	g->cpu = 0;
	g->n_members = 0;
	return g;
}

int sched_groups_add(struct sched_groups *sg, uint32_t id, pid_t tid,
		uint64_t runtime, uint64_t deadline,
		uint64_t period, uint32_t cpu)
{
	struct sched_group *g;

	if (sg == NULL)
		return -EINVAL;
	if (tid <= 0)
		return -EINVAL;

	g = sched_groups_find_or_insert(sg, tid);
	if (g == NULL)
		return -ENOMEM;

	g->sum_runtime += runtime;
	g->sum_deadline += deadline;
	g->period = period;
	g->cpu = cpu;
	if (g->n_members == 0 || id < g->leader_id)
		g->leader_id = id;
	g->n_members++;
	return 0;
}
