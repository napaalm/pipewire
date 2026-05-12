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
	bool     persistent;

	/* Persistent DAG. NULL on first apply, on driver reset, and
	 * after a reconcile_drop. */
	dag_t   *dag;
	uint64_t dag_period;
	uint64_t topo_gen_applied;

	/* Back-off counter. Consecutive reconcile_apply failures. Reset
	 * on success or when the topology generation bumps from the
	 * value last *seen* by reconcile_apply (not just the last
	 * successful one) -- otherwise a sequence of failures on the
	 * same generation never accumulates, since topo_gen_applied
	 * stays at the last success.
	 */
	uint32_t consecutive_failures;
	uint64_t topo_gen_seen;
	bool     topo_gen_seen_valid;
};

#define RECONCILE_FAILURE_BACKOFF 16u

reconcile_state_t *reconcile_init(uint32_t n_cpus,
		double cpu_utilization,
		double recalc_threshold,
		bool persistent)
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
	state->persistent = persistent;
	return state;
}

bool reconcile_state_has_persistent_dag(const reconcile_state_t *state)
{
	return state != NULL && state->persistent && state->dag != NULL;
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

/* Build a brand-new DAG from `topo`. Returns the new dag_t on
 * success, NULL on failure (the kept_ids buffer is freed in both
 * paths). On success the caller installs it in state->dag. */
static dag_t *build_dag_from_topo(reconcile_state_t *state,
		const reconcile_topo_t *topo)
{
	dag_t *dag;
	uint32_t *kept_ids;
	uint32_t n_kept = 0;
	uint32_t i;

	dag = dag_create(topo->period, topo->period,
			state->cpu_utilization, state->n_cpus);
	if (!dag)
		return NULL;

	kept_ids = calloc(topo->n_followers, sizeof(*kept_ids));
	if (!kept_ids) {
		dag_destroy(dag);
		return NULL;
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
			return NULL;
		}
		kept_ids[n_kept++] = f->id;
	}

	if (n_kept == 0) {
		free(kept_ids);
		dag_destroy(dag);
		errno = EAGAIN;
		return NULL;
	}

	if (reconcile_add_edges(dag, topo, kept_ids, n_kept) < 0) {
		free(kept_ids);
		dag_destroy(dag);
		return NULL;
	}

	free(kept_ids);
	return dag;
}

/* WCET drift check: returns true if |new - cached| / max(new,cached)
 * exceeds the configured threshold. Threshold == 0 makes every
 * difference significant (no gating). The arithmetic stays in
 * double space; the cached and new WCETs are both ns values which
 * fit comfortably in 53 bits of mantissa. */
static bool wcet_drift_significant(reconcile_state_t *state,
		uint64_t cached, uint64_t neww)
{
	double maxv;
	double diff;

	if (cached == neww)
		return false;
	if (state->recalc_threshold == 0.0)
		return true;

	maxv = (double)(cached > neww ? cached : neww);
	if (maxv == 0.0)
		return false;
	diff = (double)(cached > neww ? cached - neww : neww - cached);
	return (diff / maxv) > state->recalc_threshold;
}

/* Persistent path: keep state->dag across calls, update only the
 * deltas. The DAG's own dirty bit (set when a mutation actually
 * changes the stored value) drives the recalc inside
 * dag_foreach_node; a fully no-op reconcile (no topology change,
 * no significant WCET drift) does zero work past the freshness
 * checks. */
static int reconcile_apply_persistent(reconcile_state_t *state,
		const reconcile_topo_t *topo,
		reconcile_sched_cb_t sched_cb, void *sched_data)
{
	uint32_t i;
	bool topology_changed = topo->generation != state->topo_gen_applied;
	bool period_changed = topo->period != state->dag_period;

	/* Cold start, period change, or post-failure rebuild. */
	if (state->dag == NULL || period_changed || topology_changed) {
		if (state->dag) {
			dag_destroy(state->dag);
			state->dag = NULL;
		}
		state->dag = build_dag_from_topo(state, topo);
		if (state->dag == NULL) {
			if (errno == EAGAIN)
				return 0;
			state->consecutive_failures++;
			return -1;
		}
		state->dag_period = topo->period;
		state->topo_gen_applied = topo->generation;
	} else {
		/* No topology / period change: just mirror WCET drift.
		 * dag_set_node_wcet is gated by the threshold and is
		 * itself a no-op when the value matches; either way the
		 * dirty bit fires only when something actually changes. */
		for (i = 0; i < topo->n_followers; i++) {
			const reconcile_follower_t *f = &topo->followers[i];
			dag_node_t *dn;
			uint64_t budget;

			if (!follower_schedulable(f))
				continue;
			budget = (uint64_t)(f->wcet * 1.05);
			dn = dag_find_node(state->dag, f->id);
			if (dn == NULL)
				continue;
			if (wcet_drift_significant(state, dn->wcet, budget))
				dag_set_node_wcet(state->dag, f->id, budget);
		}
	}

	if (dag_foreach_node(state->dag, sched_cb, sched_data) < 0) {
		dag_destroy(state->dag);
		state->dag = NULL;
		state->dag_period = 0;
		state->consecutive_failures++;
		return -1;
	}

	state->consecutive_failures = 0;
	return 0;
}

/* Legacy path: destroy and rebuild every call. Selected when the
 * caller passed persistent=false to reconcile_init, i.e. the
 * recalc.persistent=false kill switch. */
static int reconcile_apply_legacy(reconcile_state_t *state,
		const reconcile_topo_t *topo,
		reconcile_sched_cb_t sched_cb, void *sched_data)
{
	dag_t *dag;

	if (state->dag) {
		dag_destroy(state->dag);
		state->dag = NULL;
	}

	dag = build_dag_from_topo(state, topo);
	if (dag == NULL) {
		if (errno == EAGAIN)
			return 0;
		state->consecutive_failures++;
		return -1;
	}

	if (dag_recalculate(dag) < 0) {
		dag_destroy(dag);
		state->consecutive_failures++;
		return -1;
	}

	if (dag_foreach_node(dag, sched_cb, sched_data) < 0) {
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

int reconcile_apply(reconcile_state_t *state,
		const reconcile_topo_t *topo,
		reconcile_sched_cb_t sched_cb, void *sched_data)
{
	if (!state || !topo || !sched_cb) {
		errno = EINVAL;
		return -1;
	}

	/* Reset the back-off counter when the topology generation
	 * differs from the last value reconcile_apply observed. The
	 * comparison uses topo_gen_seen (every-call cursor), NOT
	 * topo_gen_applied (success-only cursor) -- otherwise a
	 * sequence of failures on the same generation never
	 * accumulates the counter past 1. */
	if (state->topo_gen_seen_valid &&
			topo->generation != state->topo_gen_seen) {
		state->consecutive_failures = 0;
	}
	state->topo_gen_seen = topo->generation;
	state->topo_gen_seen_valid = true;

	if (state->consecutive_failures >= RECONCILE_FAILURE_BACKOFF) {
		/* Hit the back-off; do nothing until the topology
		 * generation bumps (which resets the counter above). */
		errno = EAGAIN;
		return -1;
	}

	if (topo->n_followers == 0) {
		/* Empty topology: drop any persistent state so a future
		 * topology change starts from scratch. */
		reconcile_drop(state);
		return 0;
	}

	return state->persistent ?
			reconcile_apply_persistent(state, topo, sched_cb, sched_data) :
			reconcile_apply_legacy(state, topo, sched_cb, sched_data);
}
