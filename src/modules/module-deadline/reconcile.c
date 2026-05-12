/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>

#include "reconcile.h"
#include "dag.h"

#include <pipewire/log.h>

PW_LOG_TOPIC_STATIC(reconcile_topic, "mod.deadline.reconcile");
#define PW_LOG_TOPIC_DEFAULT reconcile_topic

struct reconcile_state {
	/* Cached configuration. */
	uint32_t n_cpus;
	double   cpu_utilization;
	double   recalc_threshold;

	/* The persistent DAG lives here from the next commit. In this
	 * commit we still allocate-and-destroy per reconcile_apply, so
	 * the field is unused but reserved to make the next commit's
	 * diff minimal. */
	dag_t   *dag;
	uint64_t dag_period;
	uint64_t topo_gen_applied;

	/* Back-off counter (R16): consecutive reconcile_apply failures.
	 * Reset on success or when the topology generation bumps. */
	uint32_t consecutive_failures;
};

#define RECONCILE_FAILURE_BACKOFF 16u

reconcile_state_t *reconcile_init(uint32_t n_cpus,
		double cpu_utilization,
		double recalc_threshold)
{
	reconcile_state_t *state;

	if (n_cpus == 0 || cpu_utilization <= 0.0 || cpu_utilization > 1.0 ||
			recalc_threshold < 0.0 || recalc_threshold >= 1.0) {
		errno = EINVAL;
		return NULL;
	}

	state = calloc(1, sizeof(*state));
	if (!state)
		return NULL;

	state->n_cpus = n_cpus;
	state->cpu_utilization = cpu_utilization;
	state->recalc_threshold = recalc_threshold;
	return state;
}

void reconcile_drop(reconcile_state_t *state)
{
	if (!state)
		return;
	if (state->dag) {
		dag_destroy(state->dag);
		state->dag = NULL;
	}
	state->dag_period = 0;
	state->topo_gen_applied = 0;
}

void reconcile_fini(reconcile_state_t *state)
{
	if (!state)
		return;
	reconcile_drop(state);
	free(state);
}

/* Soft-failure check: a follower with wcet == 0 (or wcet * 1.05 == 0
 * because of UINT64 overflow) is excluded from the DAG. The factor
 * 1.05 matches the safety margin module-deadline applies when
 * setting the DAG node's WCET. */
static bool follower_schedulable(const reconcile_follower_t *f)
{
	if (f->wcet == 0)
		return false;
	if ((uint64_t)(f->wcet * 1.05) == 0)
		return false;
	return true;
}

/* Add follower's edges that survive the kept-set filter. The kept_ids
 * array holds the ids of followers that were successfully added to
 * the DAG; an edge whose endpoint id is missing from the set is
 * silently dropped (soft failure peer). */
static int reconcile_add_edges(dag_t *dag,
		const reconcile_topo_t *topo,
		const uint32_t *kept_ids, uint32_t n_kept)
{
	uint32_t i, j;

	for (i = 0; i < topo->n_edges; i++) {
		bool src_kept = false, dst_kept = false;
		uint32_t src = topo->edges[i].src;
		uint32_t dst = topo->edges[i].dst;

		for (j = 0; j < n_kept; j++) {
			if (kept_ids[j] == src)
				src_kept = true;
			if (kept_ids[j] == dst)
				dst_kept = true;
			if (src_kept && dst_kept)
				break;
		}
		if (!src_kept || !dst_kept)
			continue;

		if (dag_add_edge(dag, src, dst) < 0) {
			/* EEXIST is fine (idempotent add); ELOOP / other
			 * errors are not -- the caller drops the DAG. */
			if (errno != EEXIST)
				return -1;
		}
	}
	return 0;
}

int reconcile_apply(reconcile_state_t *state,
		const reconcile_topo_t *topo,
		reconcile_sched_cb_t sched_cb, void *sched_data)
{
	dag_t *dag;
	uint32_t *kept_ids;
	uint32_t n_kept = 0;
	uint32_t i;
	int rc = -1;

	if (!state || !topo || !sched_cb) {
		errno = EINVAL;
		return -1;
	}

	if (state->consecutive_failures >= RECONCILE_FAILURE_BACKOFF &&
			topo->generation == state->topo_gen_applied) {
		/* Hit the back-off; do nothing until the topology
		 * generation bumps (which resets the counter below). */
		errno = EAGAIN;
		return -1;
	}
	if (topo->generation != state->topo_gen_applied)
		state->consecutive_failures = 0;

	/* The persistent-DAG short-circuit lands in the next commit.
	 * For now, every reconcile_apply destroys and rebuilds, which
	 * is exactly what worker_apply_dag was doing inline before this
	 * refactor. */
	if (state->dag) {
		dag_destroy(state->dag);
		state->dag = NULL;
	}

	if (topo->n_followers == 0)
		return 0;

	dag = dag_create(topo->period, topo->period,
			state->cpu_utilization, state->n_cpus);
	if (!dag) {
		state->consecutive_failures++;
		return -1;
	}

	kept_ids = calloc(topo->n_followers, sizeof(*kept_ids));
	if (!kept_ids) {
		dag_destroy(dag);
		state->consecutive_failures++;
		return -1;
	}

	for (i = 0; i < topo->n_followers; i++) {
		const reconcile_follower_t *f = &topo->followers[i];

		if (!follower_schedulable(f)) {
			pw_log_debug("reconcile: skip follower %u (wcet=%lu)",
					f->id, (unsigned long)f->wcet);
			continue;
		}
		if (dag_add_node(dag, f->id, (uint64_t)(f->wcet * 1.05),
				f->tid, false) < 0) {
			if (errno == EEXIST)
				continue;
			free(kept_ids);
			dag_destroy(dag);
			state->consecutive_failures++;
			return -1;
		}
		kept_ids[n_kept++] = f->id;
	}

	if (n_kept == 0) {
		/* Every follower is bootstrapping; try again later. */
		free(kept_ids);
		dag_destroy(dag);
		return 0;
	}

	if (reconcile_add_edges(dag, topo, kept_ids, n_kept) < 0) {
		free(kept_ids);
		dag_destroy(dag);
		state->consecutive_failures++;
		return -1;
	}

	if (dag_recalculate(dag) < 0) {
		free(kept_ids);
		dag_destroy(dag);
		state->consecutive_failures++;
		return -1;
	}

	rc = dag_foreach_node(dag, sched_cb, sched_data);
	free(kept_ids);

	if (rc < 0) {
		dag_destroy(dag);
		state->consecutive_failures++;
		return -1;
	}

	dag_destroy(dag);
	state->topo_gen_applied = topo->generation;
	state->consecutive_failures = 0;
	state->dag_period = topo->period;
	return 0;
}
