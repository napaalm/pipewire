/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>

#include "reconcile.h"
#include "dag.h"
#include "contracted.h"
#include "fusion_validator.h"

#include <pipewire/log.h>

PW_LOG_TOPIC_STATIC(reconcile_topic, "mod.deadline.reconcile");
#define PW_LOG_TOPIC_DEFAULT reconcile_topic

struct reconcile_state {
	/* Cached configuration. */
	uint32_t n_cpus;
	double   cpu_utilization;
	/* Per-CPU relative capacity (length n_cpus). NULL means the
	 * homogeneous case -- forwarded to dag_create as NULL so the
	 * library installs its own uniform 1.0 vector. Heap-owned
	 * copy; freed in reconcile_fini. */
	double  *relative_capacity;
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

	/* Per-state feasibility classification, refreshed at every
	 * reconcile_dispatch_contracted call. See reconcile.h for
	 * the semantics of mode / consecutive_hard_passes and the
	 * hysteresis contract. */
	struct reconcile_feasibility feas;
};

#define RECONCILE_FAILURE_BACKOFF 16u

/* Hard-restoration hysteresis: a SOFT-to-HARD transition only
 * fires after this many consecutive feasibility passes; an
 * isolated good cycle in a stream of failures does not flip
 * the warning state and risk operator confusion. */
#define RECONCILE_HARD_RESTORE_HYSTERESIS 3u

reconcile_state_t *reconcile_init(uint32_t n_cpus,
		double cpu_utilization,
		const double *relative_capacity,
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

	if (relative_capacity != NULL) {
		state->relative_capacity = calloc(n_cpus,
				sizeof(*state->relative_capacity));
		if (!state->relative_capacity) {
			free(state);
			return NULL;
		}
		for (uint32_t i = 0; i < n_cpus; i++)
			state->relative_capacity[i] = relative_capacity[i];
	}

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

void reconcile_state_feasibility(const reconcile_state_t *state,
		struct reconcile_feasibility *out)
{
	if (out == NULL)
		return;
	if (state == NULL) {
		memset(out, 0, sizeof(*out));
		return;
	}
	*out = state->feas;
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
	free(state->relative_capacity);
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
			state->cpu_utilization, state->n_cpus,
			state->relative_capacity);
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

/* Stamp DAG co-location groups from shared TIDs in the topology
 * snapshot. When libpipewire's chain-merge consolidates several
 * follower nodes onto one thread, those nodes all publish the same
 * PW_KEY_NODE_LOOP_TID. The topology layer in module-deadline mirrors
 * that into reconcile_follower_t::tid, so a duplicate TID across the
 * follower list is the sole signal needed to declare the nodes
 * co-located.
 *
 * For each distinct shared TID we pick the lowest follower id as the
 * canonical group id (deterministic, independent of follower
 * ordering). Followers with a unique TID keep group_id = 0
 * (ungrouped, free CPU placement). The op is idempotent: when a TID
 * already maps to the right group, dag_set_node_group is a no-op
 * and the DAG stays clean.
 *
 * Returns 0 on success, -1 (with errno from dag_set_node_group) on
 * unrecoverable failure -- the caller treats that as it would treat
 * any DAG mutation failure (drop the DAG, bump the back-off). */
static int reconcile_apply_tid_groups(dag_t *dag, const reconcile_topo_t *topo)
{
	uint32_t i, j;

	if (dag == NULL || topo == NULL)
		return 0;

	for (i = 0; i < topo->n_followers; i++) {
		const reconcile_follower_t *fi = &topo->followers[i];
		uint32_t leader_id = fi->id;
		bool shared = false;
		dag_node_t *ni;

		if (fi->tid <= 0)
			continue;

		/* Find the lowest-id follower that shares this TID
		 * (including ourself). If no other follower shares the
		 * TID, the node is its own "group of one" -- still
		 * ungrouped. */
		for (j = 0; j < topo->n_followers; j++) {
			if (j == i)
				continue;
			const reconcile_follower_t *fj = &topo->followers[j];
			if (fj->tid != fi->tid)
				continue;
			shared = true;
			if (fj->id < leader_id)
				leader_id = fj->id;
		}

		ni = dag_find_node(dag, fi->id);
		if (ni == NULL)
			continue;  /* dropped follower -- skip silently */

		uint32_t desired = shared ? leader_id : 0;
		if (ni->group_id == desired)
			continue;

		if (dag_set_node_group(dag, fi->id, desired) < 0) {
			pw_log_warn("reconcile: dag_set_node_group(%u, %u) failed: %m",
					fi->id, desired);
			return -1;
		}
	}
	return 0;
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

/* Run the structural fusion validators on every group present in
 * `state_dag`'s group_id assignment and zero out group_ids that
 * fail. Each rejected group degrades back to singletons in the
 * contracted-DAG step, which is the same effect the dispatcher's
 * cycle-detection fallback achieves for non-convex groups but
 * with a clearer cause-of-rejection log line per group. The
 * caller-visible scheduling outcome is identical to the legacy
 * per-node path for rejected groups; the validator only matters
 * for groups the cost model proposes that violate one of the
 * predicates structurally.
 *
 * Today's reconcile inputs do not carry pw_impl_node
 * analyzability flags or per-member nonblocking capabilities;
 * the topology snapshot pre-filters main-loop / exported /
 * tid<=0 followers out of the candidate set, and every dynamic-
 * loop follower is implicitly treated as having
 * FUSION_CAP_NONBLOCKING_PROCESS. The structural predicates that
 * the current data path can actually run are predecessor
 * closure, precedence convexity, and externally atomic; the
 * analyzability and blocking-closure predicates become
 * load-bearing once the topo snapshot carries the missing bits.
 */
static void reconcile_filter_unsound_groups(dag_t *state_dag)
{
	dag_node_t *n;
	dag_edge_t *e;
	uint32_t i, j;
	uint32_t n_nodes = 0, n_edges = 0;
	uint32_t *member_ids = NULL;
	uint32_t *source_ids = NULL;
	struct fusion_edge_input *edges = NULL;
	uint32_t n_groups = 0;
	uint32_t *seen_groups = NULL;

	if (state_dag == NULL)
		return;

	/* Counts. */
	spa_list_for_each(n, &state_dag->nodes, link) {
		if (!n->fictitious)
			n_nodes++;
	}
	spa_list_for_each(e, &state_dag->edges, link) {
		if (!e->src->fictitious && !e->dst->fictitious)
			n_edges++;
	}
	if (n_nodes == 0)
		return;

	member_ids = malloc(n_nodes * sizeof(*member_ids));
	source_ids = malloc(n_nodes * sizeof(*source_ids));
	seen_groups = malloc(n_nodes * sizeof(*seen_groups));
	edges = n_edges ? malloc(n_edges * sizeof(*edges)) : NULL;
	if (member_ids == NULL || source_ids == NULL ||
			seen_groups == NULL ||
			(n_edges && edges == NULL)) {
		free(member_ids);
		free(source_ids);
		free(seen_groups);
		free(edges);
		return;
	}

	/* Fill the static views: all real nodes, all source ids
	 * (no incoming real edges), and the flat edge array. */
	{
		uint32_t k = 0;
		spa_list_for_each(n, &state_dag->nodes, link) {
			if (n->fictitious)
				continue;
			source_ids[k] = n->id;
			bool has_real_pred = false;
			dag_edge_t *pe;
			spa_list_for_each(pe, &n->incoming, dst_link) {
				if (!pe->src->fictitious) {
					has_real_pred = true;
					break;
				}
			}
			if (!has_real_pred)
				k++;
		}
		/* k holds the number of sources; trim the array
		 * length on the caller-side variable. */
		n_groups = k; /* repurposing variable */
	}
	uint32_t n_sources = n_groups;
	n_groups = 0;
	{
		uint32_t k = 0;
		spa_list_for_each(e, &state_dag->edges, link) {
			if (e->src->fictitious || e->dst->fictitious)
				continue;
			edges[k].src_id = e->src->id;
			edges[k].dst_id = e->dst->id;
			k++;
		}
	}

	/* Iterate distinct non-zero group_ids. For each, gather its
	 * members and run the three structural predicates the
	 * available data supports. */
	spa_list_for_each(n, &state_dag->nodes, link) {
		uint32_t gid;
		uint32_t n_members = 0;
		enum fusion_reject_reason reason;
		dag_node_t *m;
		bool already_seen = false;

		if (n->fictitious)
			continue;
		gid = n->group_id;
		if (gid == 0)
			continue;

		for (j = 0; j < n_groups; j++) {
			if (seen_groups[j] == gid) {
				already_seen = true;
				break;
			}
		}
		if (already_seen)
			continue;
		seen_groups[n_groups++] = gid;

		spa_list_for_each(m, &state_dag->nodes, link) {
			if (m->fictitious)
				continue;
			if (m->group_id == gid)
				member_ids[n_members++] = m->id;
		}
		if (n_members <= 1)
			continue;

		reason = FUSION_REJ_NONE;
		bool accepted =
			fusion_validator_predecessor_closure_accept(
				member_ids, n_members,
				edges, n_edges,
				source_ids, n_sources, &reason) &&
			fusion_validator_precedence_convex_accept(
				member_ids, n_members,
				edges, n_edges, &reason) &&
			fusion_validator_externally_atomic_accept(
				member_ids, n_members,
				edges, n_edges, &reason);

		if (!accepted) {
			pw_log_info("reconcile: rejecting fusion group %u "
				    "(%u members) -- reason=%s",
				    gid, n_members,
				    fusion_reject_reason_name(reason));
			for (i = 0; i < n_members; i++) {
				dag_node_t *dn = dag_find_node(state_dag,
						member_ids[i]);
				if (dn != NULL && dn->group_id != 0) {
					(void)dag_set_node_group(state_dag,
							member_ids[i], 0);
				}
			}
		}
	}

	free(member_ids);
	free(source_ids);
	free(seen_groups);
	free(edges);
}

/* Build a contracted DAG that mirrors `state_dag`'s topology plus
 * group assignment: every node with the same non-zero group_id
 * collapses into one macro-node; nodes with group_id == 0 form
 * singletons. The macro-node's wcet_ns is the sum of members'
 * wcet (which already carries the reconcile-side 1.05 safety
 * margin). Edges between members of the same macro-node disappear
 * as internal; external edges are inherited and deduplicated.
 *
 * Returns 0 on success with *out populated, -1 on failure (*out
 * stays NULL, errno is set). Caller owns the resulting
 * contracted_dag_t and frees it with contracted_dag_destroy.
 */
static int reconcile_build_contracted_from_dag(dag_t *state_dag,
		uint64_t period_ns, uint64_t deadline_ns,
		contracted_dag_t **out)
{
	struct contracted_member_input *members = NULL;
	struct contracted_edge_input *edges = NULL;
	uint32_t *groups = NULL;
	uint32_t n_members = 0, n_edges = 0;
	dag_node_t *n;
	dag_edge_t *e;
	int r;

	if (state_dag == NULL || out == NULL) {
		errno = EINVAL;
		return -1;
	}
	*out = NULL;

	/* First pass: count real (non-fictitious) nodes and edges. */
	spa_list_for_each(n, &state_dag->nodes, link) {
		if (!n->fictitious)
			n_members++;
	}
	spa_list_for_each(e, &state_dag->edges, link) {
		if (!e->src->fictitious && !e->dst->fictitious)
			n_edges++;
	}

	if (n_members == 0)
		return contracted_dag_build(period_ns, deadline_ns,
				NULL, NULL, 0, NULL, 0, out);

	members = calloc(n_members, sizeof(*members));
	groups = calloc(n_members, sizeof(*groups));
	if (members == NULL || groups == NULL) {
		r = -ENOMEM;
		goto fail;
	}
	if (n_edges > 0) {
		edges = calloc(n_edges, sizeof(*edges));
		if (edges == NULL) {
			r = -ENOMEM;
			goto fail;
		}
	}

	/* Second pass: fill the arrays. */
	{
		uint32_t i = 0;
		spa_list_for_each(n, &state_dag->nodes, link) {
			if (n->fictitious)
				continue;
			members[i].id = n->id;
			members[i].tid = n->tid;
			members[i].wcet_ns = n->wcet;
			groups[i] = n->group_id;
			i++;
		}
	}
	{
		uint32_t i = 0;
		spa_list_for_each(e, &state_dag->edges, link) {
			if (e->src->fictitious || e->dst->fictitious)
				continue;
			edges[i].src_id = e->src->id;
			edges[i].dst_id = e->dst->id;
			i++;
		}
	}

	r = contracted_dag_build(period_ns, deadline_ns,
			members, groups, n_members,
			edges, n_edges, out);

fail:
	free(members);
	free(groups);
	free(edges);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return 0;
}

/* Locate the macro-node owning the original member `follower_id` by
 * scanning every macro-node's member list. n_members is small in
 * practice (a few dozen at most per driver). */
static contracted_node_t *contracted_owner(contracted_dag_t *cg,
		uint32_t follower_id)
{
	contracted_node_t *cn;
	struct contracted_member *m;

	spa_list_for_each(cn, &cg->nodes, link) {
		spa_list_for_each(m, &cn->members, link) {
			if (m->id == follower_id)
				return cn;
		}
	}
	return NULL;
}

/* Return the lowest-id member of a macro-node (the leader the
 * sched_groups accumulator anchors on). */
static const struct contracted_member *macro_leader(const contracted_node_t *cn)
{
	struct contracted_member *m;
	const struct contracted_member *leader = NULL;
	spa_list_for_each(m, &cn->members, link) {
		if (leader == NULL || m->id < leader->id)
			leader = m;
	}
	return leader;
}

/* Run the contracted-DAG analysis on top of `state_dag` and emit
 * one sched_cb per real follower carrying the macro-node's
 * (cumulative_deadline, local_deadline, cpu). The macro-node's
 * residual overhead is folded into the leader follower's reported
 * runtime so the downstream per-TID accumulator's sum_runtime
 * lands at the macro-node's effective WCET.
 *
 * If the fusion partition is non-convex (a member of group A
 * appears on a path between two members of group B), the
 * contracted DAG develops a cycle. This is structurally unsound
 * and Sarkar 1989 §5.3 explicitly forbids it; the soundness
 * validator that lands later in this implementation cycle will
 * reject such partitions at the upstream cost model. Until that
 * lands, the dispatcher logs the cycle and falls back to a
 * per-original-node emission on state_dag so the existing
 * prototype's permissive behaviour is preserved for graphs the
 * cost model has not yet learned to reject.
 *
 * Returns 0 on success, -1 on a non-recoverable failure (errno set).
 */
static int reconcile_dispatch_contracted(reconcile_state_t *state,
		dag_t *state_dag, uint64_t period_ns,
		reconcile_sched_cb_t sched_cb, void *sched_data)
{
	contracted_dag_t *cg = NULL;
	struct dag *macro_dag = NULL;
	dag_node_t *n;
	int r;

	/* Filter unsound fusion groups before contraction. A rejected
	 * group has its members' group_ids cleared, which makes the
	 * builder treat them as singletons. The downstream
	 * cycle-detection fallback still catches the (now smaller)
	 * residue of cases where the runtime data is insufficient to
	 * decide soundness. */
	reconcile_filter_unsound_groups(state_dag);

	r = reconcile_build_contracted_from_dag(state_dag, period_ns,
			period_ns, &cg);
	if (r < 0)
		return -1;

	if (contracted_dag_has_cycle(cg)) {
		pw_log_warn("reconcile: fusion partition is non-convex "
			    "(contracted DAG has a cycle); falling back to "
			    "per-node deadline split. The soundness validator "
			    "will reject this partition once it lands.");
		contracted_dag_destroy(cg);
		return dag_foreach_node(state_dag, sched_cb, sched_data);
	}

	r = contracted_dag_to_dag(cg, state->cpu_utilization,
			state->n_cpus, state->relative_capacity, &macro_dag);
	if (r < 0) {
		contracted_dag_destroy(cg);
		errno = -r;
		return -1;
	}

	if (dag_recalculate(macro_dag) < 0) {
		int e = errno;
		pw_log_warn("reconcile: contracted-DAG recalculate failed "
			    "(%m); falling back to per-node deadline split.");
		dag_destroy(macro_dag);
		contracted_dag_destroy(cg);
		errno = e;
		return dag_foreach_node(state_dag, sched_cb, sched_data);
	}

	contracted_dag_apply_dag_schedule(cg, macro_dag);

	/* Feasibility classification on the macro-dag. Demotion to
	 * SOFT_DEGRADED is immediate on the first failure; promotion
	 * back to HARD requires N consecutive feasible passes
	 * (hysteresis). The chosen mode plus the reasons are stamped
	 * on state->feas so module-deadline can surface them. */
	{
		struct reconcile_feasibility f;
		bool density_ok, dbf_ok, feasible;
		double max_d = 0.0;
		uint32_t failing_cpu_d = 0;
		uint64_t failing_t = 0, failing_demand = 0;
		uint32_t failing_cpu_dbf = 0;

		memset(&f, 0, sizeof(f));
		density_ok = dag_density_feasible(macro_dag, &max_d,
				&failing_cpu_d);
		dbf_ok = dag_dbf_feasible(macro_dag, &failing_cpu_dbf,
				&failing_t, &failing_demand);
		feasible = density_ok || dbf_ok;

		f.density_passed = density_ok;
		f.max_density = max_d;
		f.density_failing_cpu = failing_cpu_d;
		f.dbf_passed = dbf_ok;
		f.dbf_failing_t = failing_t;
		f.dbf_failing_demand = failing_demand;
		f.dbf_failing_cpu = failing_cpu_dbf;

		if (feasible) {
			if (state->feas.mode == RECONCILE_MODE_SOFT_DEGRADED) {
				/* Promote only after hysteresis worth
				 * of consecutive passes. */
				uint32_t n = state->feas.consecutive_hard_passes + 1;
				if (n >= RECONCILE_HARD_RESTORE_HYSTERESIS) {
					f.mode = RECONCILE_MODE_HARD;
					f.consecutive_hard_passes = n;
					snprintf(f.reason, sizeof(f.reason),
						 "%s", "");
					pw_log_warn("reconcile: schedule "
						"feasible again "
						"(method=%s, max_density=%.3f); "
						"hard-real-time guarantees "
						"restored",
						density_ok ? "density" : "dbf",
						max_d);
				} else {
					f.mode = RECONCILE_MODE_SOFT_DEGRADED;
					f.consecutive_hard_passes = n;
					snprintf(f.reason, sizeof(f.reason),
						 "%s", state->feas.reason);
				}
			} else {
				f.mode = RECONCILE_MODE_HARD;
				f.consecutive_hard_passes =
					state->feas.consecutive_hard_passes + 1;
				if (f.consecutive_hard_passes >
				    RECONCILE_HARD_RESTORE_HYSTERESIS)
					f.consecutive_hard_passes =
						RECONCILE_HARD_RESTORE_HYSTERESIS;
			}
		} else {
			const char *reason = !density_ok && !dbf_ok
				? "edf_infeasible"
				: (!density_ok ? "density_above_one"
					      : "dbf_overload");
			f.mode = RECONCILE_MODE_SOFT_DEGRADED;
			f.consecutive_hard_passes = 0;
			snprintf(f.reason, sizeof(f.reason), "%s", reason);
			if (state->feas.mode != RECONCILE_MODE_SOFT_DEGRADED) {
				pw_log_warn("reconcile: schedule infeasible "
					"(reason=%s, max_density=%.3f, "
					"dbf_failing_t=%" PRIu64 "); "
					"hard-real-time guarantees dropped, "
					"continuing with kernel-valid "
					"parameters",
					reason, max_d, failing_t);
			}
		}

		state->feas = f;
	}

	/* Per-CPU soft-redistribution scaling. In SOFT_DEGRADED mode
	 * the contracted analysis already decided the schedule does
	 * not meet every deadline on every activation. To bound the
	 * damage on overloaded CPUs, scale every macro-node's runtime
	 * budget on a CPU c by min(1.0, 1.0 / density(c)); the
	 * resulting per-CPU sum of (R / D) is <= 1 and the kernel
	 * grants no more than that fraction of CPU time per period,
	 * which is the "least bad" deterministic redistribution the
	 * plan calls for. Tasks may miss their actual demand --
	 * xruns are possible -- but the system stays kernel-valid
	 * and other CPUs are unaffected. In HARD mode the scales
	 * are all 1.0 (no change). */
	double *cpu_scale = NULL;
	if (state->feas.mode == RECONCILE_MODE_SOFT_DEGRADED &&
			state->n_cpus > 0) {
		cpu_scale = calloc(state->n_cpus, sizeof(*cpu_scale));
		if (cpu_scale != NULL) {
			uint32_t cpu;
			for (cpu = 0; cpu < state->n_cpus; cpu++) {
				double d = dag_per_cpu_density(macro_dag, cpu);
				cpu_scale[cpu] = (d > 1.0) ? (1.0 / d) : 1.0;
			}
		}
	}

	/* Per-follower emission. For each real node in state_dag, look
	 * up its owning macro-node and emit sched_cb with the macro's
	 * (cumulative_deadline, local_deadline, cpu). Runtime: each
	 * follower reports its own wcet so sched_groups.sum_runtime
	 * aggregates to sum_of_members; the macro's overhead is folded
	 * into the leader's report so the final sum equals the macro-
	 * node's effective WCET. In SOFT_DEGRADED mode the runtime is
	 * scaled down by the per-CPU soft-redistribution factor
	 * computed above, with a 1ns floor to keep the kernel call
	 * valid. */
	spa_list_for_each(n, &state_dag->nodes, link) {
		contracted_node_t *cn;
		const struct contracted_member *leader;
		uint64_t runtime;
		uint32_t cpu;

		if (n->fictitious)
			continue;
		cn = contracted_owner(cg, n->id);
		if (cn == NULL)
			continue;
		leader = macro_leader(cn);

		runtime = n->wcet;
		if (leader != NULL && leader->id == n->id)
			runtime += cn->overhead_ns;

		cpu = (cn->cpu < 0) ? 0u : (uint32_t)cn->cpu;

		if (cpu_scale != NULL && cpu < state->n_cpus) {
			double s = cpu_scale[cpu];
			if (s > 0.0 && s < 1.0) {
				double scaled = (double)runtime * s;
				if (scaled < 1.0)
					scaled = 1.0;  /* runtime > 0 floor */
				runtime = (uint64_t)scaled;
			}
		}

		sched_cb(sched_data, n->id, n->tid, runtime,
				cn->cumulative_deadline_ns,
				cn->local_deadline_ns,
				period_ns, cpu);
	}

	free(cpu_scale);

	dag_destroy(macro_dag);
	contracted_dag_destroy(cg);
	/* state_dag's own deadlines are intentionally not recomputed
	 * (the contracted analysis is the authority now), but its
	 * dirty bit is cleared so the steady-state idempotency
	 * contract -- "a no-op reconcile pass leaves the cached DAG
	 * marked clean" -- still holds for instrumentation that
	 * watches state_dag->dirty. */
	state_dag->dirty = false;
	return 0;
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
	bool late_schedulable = false;

	/* The topology generation is structural-only (id+tid+edges) so
	 * a follower whose wcet was 0 at the first reconcile after it
	 * appeared (a fresh shim stream typically reports 0 for one or
	 * two cycles before the driver stamps its first prev_run_time)
	 * stays out of the DAG forever even after its sketch produces
	 * a meaningful budget. Detect that transition explicitly: any
	 * follower that is now schedulable but is missing from the
	 * persistent DAG forces a rebuild on this pass, exactly as a
	 * structural change would. */
	if (state->dag != NULL && !period_changed && !topology_changed) {
		for (i = 0; i < topo->n_followers; i++) {
			const reconcile_follower_t *f = &topo->followers[i];
			if (!follower_schedulable(f))
				continue;
			if (dag_find_node(state->dag, f->id) == NULL) {
				late_schedulable = true;
				break;
			}
		}
	}

	/* Cold start, period change, or post-failure rebuild. */
	if (state->dag == NULL || period_changed || topology_changed || late_schedulable) {
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

	/* Re-stamp co-location groups on every pass. Idempotent when
	 * the topology hasn't changed (each dag_set_node_group is a
	 * no-op when the value matches), so the steady-state cost is
	 * one pointer chase per follower. */
	if (reconcile_apply_tid_groups(state->dag, topo) < 0) {
		dag_destroy(state->dag);
		state->dag = NULL;
		state->dag_period = 0;
		state->consecutive_failures++;
		return -1;
	}

	/* Run the macro-node analysis on the contracted DAG derived
	 * from state->dag (topology + WCETs + group ids), then emit
	 * sched_cb per real follower with the macro-node's
	 * (cumulative_deadline, local_deadline, cpu). state->dag is
	 * kept as the topology / WCET cache; its own deadline split
	 * is no longer the authority and is intentionally not
	 * recalculated here. */
	if (reconcile_dispatch_contracted(state, state->dag,
				topo->period, sched_cb, sched_data) < 0) {
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

	if (reconcile_apply_tid_groups(dag, topo) < 0) {
		dag_destroy(dag);
		state->consecutive_failures++;
		return -1;
	}

	if (reconcile_dispatch_contracted(state, dag, topo->period,
				sched_cb, sched_data) < 0) {
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

/* Test-only accessor: returns the persistent dag_t* (or NULL if no
 * DAG is currently cached). Not declared in reconcile.h because
 * production callers should treat the DAG as opaque, but the unit
 * test for TID grouping needs to peek at per-node group_id and cpu
 * fields. The symbol is harmless in production: nothing in
 * module-deadline.c references it, so the linker drops it from
 * libpipewire-module-deadline.so. */
dag_t *reconcile_state_dag_for_test(reconcile_state_t *s)
{
	return s ? s->dag : NULL;
}
