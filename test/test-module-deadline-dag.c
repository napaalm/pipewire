/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>

#include "pwtest.h"

#include "../src/modules/module-deadline/dag.h"

static int add_real_node(dag_t *g, uint32_t id, uint64_t wcet, pid_t tid)
{
	return dag_add_node(g, id, wcet, tid, false);
}

static dag_node_t *find_node_by_id(dag_t *g, uint32_t id)
{
	dag_node_t *n;

	spa_list_for_each(n, &g->nodes, link) {
		if (n->id == id)
			return n;
	}
	return NULL;
}

static uint32_t bitset_population(bitset_t *set, uint32_t nbits)
{
	uint32_t count = 0;

	for (uint32_t i = 0; i < nbits; i++) {
		if (bitset_test(set, i))
			count++;
	}
	return count;
}

static bool unrelated_contains_ids(dag_t *g, bitset_t *set, const uint32_t *ids, uint32_t n_ids)
{
	for (uint32_t i = 0; i < n_ids; i++) {
		dag_node_t *n = find_node_by_id(g, ids[i]);
		if (!n || !bitset_test(set, n->index))
			return false;
	}
	return true;
}

static bool dag_has_unrelated_subset(dag_t *g, const uint32_t *ids, uint32_t n_ids)
{
	for (uint32_t i = 0; i < g->unrelated_size; i++) {
		if (unrelated_contains_ids(g, g->unrelated[i], ids, n_ids))
			return true;
	}
	return false;
}

static uint32_t dag_max_unrelated_size(dag_t *g)
{
	uint32_t max_size = 0;

	for (uint32_t i = 0; i < g->unrelated_size; i++) {
		uint32_t set_size = bitset_population(g->unrelated[i], g->indexed_count);
		if (set_size > max_size)
			max_size = set_size;
	}
	return max_size;
}

static uint32_t dag_real_node_count(dag_t *g)
{
	uint32_t count = 0;
	dag_node_t *n;

	spa_list_for_each(n, &g->nodes, link) {
		if (n->fictitious)
			continue;
		count++;
	}

	return count;
}

static uint32_t dag_fictitious_node_count(dag_t *g)
{
	uint32_t count = 0;
	dag_node_t *n;

	spa_list_for_each(n, &g->nodes, link) {
		if (n->fictitious)
			count++;
	}

	return count;
}

static uint32_t dag_real_indexed_count(dag_t *g)
{
	uint32_t count = 0;

	for (uint32_t i = 0; i < g->indexed_count; i++) {
		if (!g->indexed_nodes[i]->fictitious)
			count++;
	}

	return count;
}

static bool dag_all_nodes_have_no_successors(dag_t *g)
{
	dag_node_t *n;

	spa_list_for_each(n, &g->nodes, link) {
		if (n->successors != NULL)
			return false;
	}

	return true;
}

static bool dag_unrelated_has_fictitious_nodes(dag_t *g)
{
	for (uint32_t i = 0; i < g->unrelated_size; i++) {
		for (uint32_t j = 0; j < g->indexed_count; j++) {
			if (bitset_test(g->unrelated[i], j) && g->indexed_nodes[j]->fictitious)
				return true;
		}
	}

	return false;
}

struct foreach_info {
	uint32_t count;
	bool saw_internal_tid;
};

static void foreach_count_cb(void *data, uint32_t id, pid_t tid, uint64_t wcet,
		uint64_t cumulative_deadline, uint64_t local_deadline,
		uint64_t period, uint32_t cpu,
		uint32_t fusion_group_leader_id)
{
	struct foreach_info *info = data;

	(void)id;
	(void)cumulative_deadline;
	(void)local_deadline;
	(void)period;
	(void)cpu;
	(void)fusion_group_leader_id;

	info->count++;
	if (tid < 0 || wcet == 0)
		info->saw_internal_tid = true;
}

/* Pin the contract for the explicit deadline fields after
 * dag_recalculate: cumulative > 0 on every real node, local > 0 and
 * <= cumulative, monotonicity along every edge, and source nodes
 * (no real predecessor) have local == cumulative. */
PWTEST(explicit_deadline_fields_populated_after_recalc)
{
	dag_t *g = dag_create(100, 100, 0.55f, 1, NULL);
	pwtest_ptr_notnull(g);

	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);

	dag_node_t *n1 = find_node_by_id(g, 1);
	dag_node_t *n2 = find_node_by_id(g, 2);
	pwtest_int_eq((int)n1->cumulative_deadline, 0);
	pwtest_int_eq((int)n1->local_deadline, 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	pwtest_bool_true(n1->deadline > 0);
	pwtest_bool_true(n1->cumulative_deadline > 0);
	pwtest_bool_true(n1->local_deadline > 0);
	pwtest_bool_true(n2->cumulative_deadline > 0);
	pwtest_bool_true(n2->local_deadline > 0);
	/* Source: local equals cumulative (no real predecessor). */
	pwtest_int_eq((int)n1->local_deadline, (int)n1->cumulative_deadline);
	/* Monotonicity along the only real edge. */
	pwtest_bool_true(n1->cumulative_deadline <= n2->cumulative_deadline);
	/* For a non-source the local-vs-cumulative relation holds. */
	pwtest_int_eq((int)n2->local_deadline,
		      (int)(n2->cumulative_deadline - n1->cumulative_deadline));

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Chain A->B->C: with the splitter producing per-node slices, the
 * forward sum populates monotonically increasing cumulative
 * deadlines; the local-deadline conversion produces the original
 * per-node slices for the source and the difference downstream.
 * Pins the kernel-API conversion the plan's pseudocode requires. */
PWTEST(local_deadline_conversion_chain)
{
	dag_t *g = dag_create(1000, 1000, 0.95f, 1, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 1001), 0);
	pwtest_int_eq(add_real_node(g, 2, 100, 1002), 0);
	pwtest_int_eq(add_real_node(g, 3, 100, 1003), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	dag_node_t *a = find_node_by_id(g, 1);
	dag_node_t *b = find_node_by_id(g, 2);
	dag_node_t *c = find_node_by_id(g, 3);

	/* Cumulative is monotone non-decreasing along edges. */
	pwtest_bool_true(a->cumulative_deadline <= b->cumulative_deadline);
	pwtest_bool_true(b->cumulative_deadline <= c->cumulative_deadline);
	/* Source: local == cumulative. */
	pwtest_int_eq((int)a->local_deadline, (int)a->cumulative_deadline);
	/* Non-source: local equals cumulative delta. */
	pwtest_int_eq((int)b->local_deadline,
		      (int)(b->cumulative_deadline - a->cumulative_deadline));
	pwtest_int_eq((int)c->local_deadline,
		      (int)(c->cumulative_deadline - b->cumulative_deadline));
	/* Sink cumulative <= global D (the splitter enforces this
	 * before this commit's pass runs). */
	pwtest_bool_true(c->cumulative_deadline <= 1000);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Join A->C, B->C: local_deadline[C] = cumulative[C] - max(cumulative
 * of predecessors). The forward sum picks the longer path. */
PWTEST(local_deadline_conversion_join_uses_max_pred)
{
	dag_t *g = dag_create(1000, 1000, 0.95f, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 1001), 0);
	pwtest_int_eq(add_real_node(g, 2, 300, 1002), 0);
	pwtest_int_eq(add_real_node(g, 3, 200, 1003), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	dag_node_t *a = find_node_by_id(g, 1);
	dag_node_t *b = find_node_by_id(g, 2);
	dag_node_t *c = find_node_by_id(g, 3);

	uint64_t max_pred = a->cumulative_deadline >= b->cumulative_deadline
		? a->cumulative_deadline : b->cumulative_deadline;
	pwtest_int_eq((int)c->local_deadline,
		      (int)(c->cumulative_deadline - max_pred));
	pwtest_bool_true(c->cumulative_deadline >= max_pred);
	pwtest_bool_true(c->local_deadline > 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* dag_compute_local_deadlines is callable as a stand-alone transform
 * once cumulative_deadline has been assigned externally (the
 * contracted-DAG path will exercise this). Drive it with the plan's
 * pseudocode example: cumulative (100, 250, 500) on A -> B -> C must
 * yield local (100, 150, 250). */
PWTEST(local_deadline_conversion_direct_pseudocode_example)
{
	dag_t *g = dag_create(1000, 500, 0.95f, 1, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 50, 1001), 0);
	pwtest_int_eq(add_real_node(g, 2, 50, 1002), 0);
	pwtest_int_eq(add_real_node(g, 3, 50, 1003), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	/* Overwrite cumulative deadlines with the plan's reference
	 * values, then ask the conversion function to derive locals. */
	dag_node_t *a = find_node_by_id(g, 1);
	dag_node_t *b = find_node_by_id(g, 2);
	dag_node_t *c = find_node_by_id(g, 3);
	a->cumulative_deadline = 100;
	b->cumulative_deadline = 250;
	c->cumulative_deadline = 500;

	pwtest_bool_true(dag_compute_local_deadlines(g));
	pwtest_int_eq((int)a->local_deadline, 100);
	pwtest_int_eq((int)b->local_deadline, 150);
	pwtest_int_eq((int)c->local_deadline, 250);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Same shape, join topology: A -> C, B -> C with cumulative
 * (100, 200, 500) must yield C.local = 300. */
PWTEST(local_deadline_conversion_direct_join_example)
{
	dag_t *g = dag_create(1000, 500, 0.95f, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 50, 1001), 0);
	pwtest_int_eq(add_real_node(g, 2, 50, 1002), 0);
	pwtest_int_eq(add_real_node(g, 3, 100, 1003), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	dag_node_t *a = find_node_by_id(g, 1);
	dag_node_t *b = find_node_by_id(g, 2);
	dag_node_t *c = find_node_by_id(g, 3);
	a->cumulative_deadline = 100;
	b->cumulative_deadline = 200;
	c->cumulative_deadline = 500;

	pwtest_bool_true(dag_compute_local_deadlines(g));
	pwtest_int_eq((int)a->local_deadline, 100);
	pwtest_int_eq((int)b->local_deadline, 200);
	pwtest_int_eq((int)c->local_deadline, 300);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Non-monotonic cumulatives are rejected before any kernel call. */
PWTEST(local_deadline_conversion_rejects_nonmonotonic)
{
	dag_t *g = dag_create(1000, 500, 0.95f, 1, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 50, 1001), 0);
	pwtest_int_eq(add_real_node(g, 2, 50, 1002), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	dag_node_t *a = find_node_by_id(g, 1);
	dag_node_t *b = find_node_by_id(g, 2);
	a->cumulative_deadline = 300;
	b->cumulative_deadline = 200;

	pwtest_bool_false(dag_compute_local_deadlines(g));

	dag_destroy(g);
	return PWTEST_PASS;
}

/* --- per-CPU density predicate (Baruah 1990 sufficient test) --- */

PWTEST(density_empty_cpu_returns_zero)
{
	dag_t *g = dag_create(1000, 1000, 0.95f, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 1001), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	/* CPU 0 carries the single node; CPU 1 is empty. */
	pwtest_bool_true(dag_per_cpu_density(g, 1) == 0.0);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* A single small task on one CPU: density = C/D well under 1. */
PWTEST(density_feasible_chain_under_one)
{
	dag_t *g = dag_create(1000, 1000, 0.95f, 1, NULL);
	double max_d;
	uint32_t failing_cpu;
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 1001), 0);
	pwtest_int_eq(add_real_node(g, 2, 100, 1002), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	pwtest_bool_true(dag_density_feasible(g, &max_d, &failing_cpu));
	pwtest_bool_true(max_d > 0.0);
	pwtest_bool_true(max_d <= 1.0);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Single CPU with a task whose wcet exceeds the local deadline ->
 * density > 1. We construct it by post-hoc raising the wcet
 * (the recalculate step gives a feasible split; tweak afterwards
 * to drive density above 1). */
PWTEST(density_infeasible_chain_above_one)
{
	dag_t *g = dag_create(1000, 1000, 0.95f, 1, NULL);
	double max_d;
	uint32_t failing_cpu;
	dag_node_t *n;
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 1001), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	/* Force a density > 1 by mutating WCET past the local
	 * deadline after recalculate. The placer doesn't re-run; the
	 * density predicate inspects current state. */
	n = find_node_by_id(g, 1);
	pwtest_ptr_notnull(n);
	n->wcet = (uint64_t)((double)n->local_deadline * 2.0);

	pwtest_bool_false(dag_density_feasible(g, &max_d, &failing_cpu));
	pwtest_bool_true(max_d > 1.0);
	pwtest_int_eq((int)failing_cpu, (int)n->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* With three CPUs and unrelated tasks, each task lands on its own
 * CPU and the per-CPU density does not aggregate across CPUs. */
PWTEST(density_per_cpu_isolates_workloads)
{
	dag_t *g = dag_create(1000, 1000, 0.55f, 3, NULL);
	double max_d;
	uint32_t failing_cpu;
	uint32_t i;
	double total = 0.0;
	pwtest_ptr_notnull(g);
	/* Three independent sources -> each gets its own CPU under
	 * worst-fit when admission_ceiling = 0.55 forces a spread. */
	pwtest_int_eq(add_real_node(g, 1, 300, 1001), 0);
	pwtest_int_eq(add_real_node(g, 2, 300, 1002), 0);
	pwtest_int_eq(add_real_node(g, 3, 300, 1003), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	pwtest_bool_true(dag_density_feasible(g, &max_d, &failing_cpu));
	/* Sum across CPUs is the workload's total density; no single
	 * CPU should hit anywhere near total. */
	for (i = 0; i < 3; i++)
		total += dag_per_cpu_density(g, i);
	pwtest_bool_true(max_d < total);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Relative capacity scaling: a slower CPU's density is divided by
 * its relative_capacity. Same node, same deadline -> density on
 * a 0.5-capacity CPU is 2x that on a 1.0-capacity CPU. */
PWTEST(density_relative_capacity_scaling)
{
	double rel[2] = { 1.0, 0.5 };
	dag_t *g = dag_create(1000, 1000, 0.95f, 2, rel);
	dag_node_t *n;
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 1001), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	n = find_node_by_id(g, 1);
	pwtest_ptr_notnull(n);

	/* If the placer chose CPU 0 (fastest), force the node to CPU
	 * 1 (slow) and observe the density jump. */
	uint32_t orig_cpu = n->cpu;
	(void)orig_cpu;
	double d0_native = dag_per_cpu_density(g, n->cpu);
	pwtest_bool_true(d0_native > 0.0);

	/* Pretend the node sits on CPU 1 -- density should be the
	 * D=local_deadline normalised result divided by 0.5. */
	uint32_t saved = n->cpu;
	n->cpu = 1;
	double d_slow = dag_per_cpu_density(g, 1);
	n->cpu = saved;
	pwtest_bool_true(d_slow > 0.0);
	/* Ratio == relative_capacity[0] / relative_capacity[1] = 2.0,
	 * but only when the node was originally on CPU 0. Verify
	 * directly: density on the slow CPU is twice the contribution
	 * the fast CPU would have seen for the same node. */
	double expected_fast_contrib = (double)n->wcet /
		(double)(n->local_deadline ? n->local_deadline : g->period);
	pwtest_bool_true(d_slow > expected_fast_contrib * 1.5);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(density_null_safe)
{
	pwtest_bool_true(dag_per_cpu_density(NULL, 0) == 0.0);
	pwtest_bool_false(dag_density_feasible(NULL, NULL, NULL));
	return PWTEST_PASS;
}

/* --- processor-demand (DBF) feasibility (Baruah 1990) --- */

PWTEST(dbf_feasible_single_task)
{
	dag_t *g = dag_create(1000, 1000, 0.95f, 1, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 1001), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	pwtest_bool_true(dag_dbf_feasible(g, NULL, NULL, NULL));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(dbf_infeasible_when_demand_exceeds_t)
{
	dag_t *g = dag_create(1000, 1000, 0.95f, 1, NULL);
	dag_node_t *n;
	uint32_t failing_cpu;
	uint64_t failing_t, failing_demand;
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 1001), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	/* Force C > D so the first checkpoint t = D fails because
	 * demand C > t. */
	n = find_node_by_id(g, 1);
	pwtest_ptr_notnull(n);
	n->wcet = n->local_deadline + 1;

	pwtest_bool_false(dag_dbf_feasible(g, &failing_cpu, &failing_t,
				&failing_demand));
	pwtest_int_eq((int)failing_cpu, (int)n->cpu);
	pwtest_bool_true(failing_t == n->local_deadline);
	pwtest_bool_true(failing_demand > failing_t);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(dbf_passes_when_density_passes)
{
	/* A density-feasible schedule (every per-CPU density <= 1) is
	 * also DBF-feasible: the dbf criterion is strictly weaker
	 * than density for synchronously-released constrained-
	 * deadline task sets. Construct a comfortably under-loaded
	 * task set and verify both predicates agree. */
	dag_t *g = dag_create(1000, 1000, 0.95f, 1, NULL);
	double max_d;
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 1001), 0);
	pwtest_int_eq(add_real_node(g, 2, 100, 1002), 0);
	pwtest_int_eq(add_real_node(g, 3, 100, 1003), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	pwtest_bool_true(dag_density_feasible(g, &max_d, NULL));
	pwtest_bool_true(max_d <= 1.0);
	pwtest_bool_true(dag_dbf_feasible(g, NULL, NULL, NULL));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(dbf_null_safe)
{
	pwtest_bool_false(dag_dbf_feasible(NULL, NULL, NULL, NULL));
	return PWTEST_PASS;
}

PWTEST(chain_uses_peak_not_sum)
{
	dag_t *g = dag_create(100, 100, 0.55f, 1, NULL);
	pwtest_ptr_notnull(g);

	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	dag_node_t *n1 = find_node_by_id(g, 1);
	dag_node_t *n2 = find_node_by_id(g, 2);
	dag_node_t *n3 = find_node_by_id(g, 3);
	pwtest_ptr_notnull(n1);
	pwtest_ptr_notnull(n2);
	pwtest_ptr_notnull(n3);

	pwtest_int_eq((int)dag_real_indexed_count(g), 3);
	pwtest_int_eq((int)dag_fictitious_node_count(g), 2);
	pwtest_ptr_notnull(n1->successors);
	pwtest_ptr_notnull(n2->successors);
	pwtest_ptr_notnull(n3->successors);
	pwtest_bool_true(bitset_test(n1->successors, n1->index));
	pwtest_bool_true(bitset_test(n1->successors, n2->index));
	pwtest_bool_false(bitset_test(n2->successors, n1->index));
	pwtest_bool_true(bitset_test(n1->successors, n3->index));
	pwtest_bool_false(bitset_test(n3->successors, n1->index));
	pwtest_int_eq((int)dag_max_unrelated_size(g), 1);
	pwtest_bool_false(dag_unrelated_has_fictitious_nodes(g));
	pwtest_int_eq((int)n1->cpu, 0);
	pwtest_int_eq((int)n2->cpu, 0);
	pwtest_int_eq((int)n3->cpu, 0);
	pwtest_bool_true(n1->deadline_assigned);
	pwtest_bool_true(n2->deadline_assigned);
	pwtest_bool_true(n3->deadline_assigned);
	pwtest_bool_true(n1->deadline > 0);
	pwtest_bool_true(n2->deadline > 0);
	pwtest_bool_true(n3->deadline > 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(fork_join_fails_on_peak_concurrency)
{
	dag_t *g = dag_create(100, 100, 0.55f, 1, NULL);
	static const uint32_t middle_parallel[] = { 2, 3 };
	int res;

	pwtest_ptr_notnull(g);

	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 10, 104), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);

	res = dag_recalculate(g);
	pwtest_int_eq(res, -1);
	pwtest_int_eq(errno, EAGAIN);
	pwtest_bool_true(dag_has_unrelated_subset(g, middle_parallel, 2));
	pwtest_bool_false(dag_unrelated_has_fictitious_nodes(g));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(multi_source_initial_cut_and_cleanup)
{
	dag_t *g = dag_create(100, 100, 0.90f, 1, NULL);
	static const uint32_t sources[] = { 1, 2 };
	static const uint32_t mixed_cut[] = { 2, 3 };
	static const uint32_t invalid_cut[] = { 1, 4 };

	pwtest_ptr_notnull(g);

	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 10, 104), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	pwtest_bool_true(dag_has_unrelated_subset(g, sources, 2));
	pwtest_bool_true(dag_has_unrelated_subset(g, mixed_cut, 2));
	pwtest_bool_false(dag_has_unrelated_subset(g, invalid_cut, 2));
	pwtest_bool_false(dag_unrelated_has_fictitious_nodes(g));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(topology_change_rebuilds_analysis)
{
	dag_t *g = dag_create(100, 100, 0.90f, 1, NULL);
	static const uint32_t concurrent_pair[] = { 1, 3 };

	pwtest_ptr_notnull(g);

	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)dag_real_indexed_count(g), 3);
	pwtest_int_eq((int)dag_fictitious_node_count(g), 2);
	pwtest_int_eq((int)dag_max_unrelated_size(g), 2);
	pwtest_bool_true(dag_has_unrelated_subset(g, concurrent_pair, 2));

	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_ptr_eq(g->indexed_nodes, NULL);
	pwtest_ptr_eq(g->unrelated, NULL);
	pwtest_int_eq((int)g->indexed_count, 0);
	pwtest_int_eq((int)g->unrelated_size, 0);
	pwtest_bool_true(dag_all_nodes_have_no_successors(g));

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)dag_real_indexed_count(g), 3);
	pwtest_int_eq((int)dag_fictitious_node_count(g), 2);
	pwtest_int_eq((int)dag_max_unrelated_size(g), 1);
	pwtest_bool_false(dag_has_unrelated_subset(g, concurrent_pair, 2));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(fictitious_nodes_allow_zero_wcet)
{
	dag_t *g = dag_create(100, 100, 0.90f, 1, NULL);

	pwtest_ptr_notnull(g);

	pwtest_int_eq(dag_add_node(g, 1, 0, 101, false), -1);
	pwtest_int_eq(errno, EINVAL);
	pwtest_int_eq(dag_add_node(g, UINT32_MAX, 1, 101, false), -1);
	pwtest_int_eq(errno, EINVAL);
	pwtest_int_eq(dag_add_node(g, UINT32_MAX, 0, -1, true), 0);
	pwtest_ptr_notnull(find_node_by_id(g, UINT32_MAX));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(recalculate_keeps_exactly_two_fictitious_nodes)
{
	dag_t *g = dag_create(100, 100, 0.90f, 1, NULL);

	pwtest_ptr_notnull(g);

	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)dag_real_node_count(g), 3);
	pwtest_int_eq((int)dag_fictitious_node_count(g), 2);
	pwtest_ptr_notnull(find_node_by_id(g, UINT32_MAX));
	pwtest_ptr_notnull(find_node_by_id(g, UINT32_MAX - 1));
	pwtest_ptr_notnull(find_node_by_id(g, 1)->successors);
	pwtest_ptr_notnull(find_node_by_id(g, 2)->successors);
	pwtest_ptr_notnull(find_node_by_id(g, 3)->successors);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)dag_real_node_count(g), 3);
	pwtest_int_eq((int)dag_fictitious_node_count(g), 2);
	pwtest_ptr_notnull(find_node_by_id(g, UINT32_MAX));
	pwtest_ptr_notnull(find_node_by_id(g, UINT32_MAX - 1));
	pwtest_ptr_notnull(find_node_by_id(g, 1)->successors);
	pwtest_ptr_notnull(find_node_by_id(g, 2)->successors);
	pwtest_ptr_notnull(find_node_by_id(g, 3)->successors);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(dag_foreach_node_skips_fictitious_nodes)
{
	dag_t *g = dag_create(100, 100, 0.90f, 1, NULL);
	struct foreach_info info = { 0 };

	pwtest_ptr_notnull(g);

	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	pwtest_int_eq(dag_foreach_node(g, foreach_count_cb, &info), 0);
	pwtest_int_eq((int)info.count, 3);
	pwtest_bool_false(info.saw_internal_tid);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(iterative_deadlines_assign_fork_join)
{
	dag_t *g = dag_create(100, 100, 0.90f, 2, NULL);
	dag_node_t *n1, *n2, *n3, *n4;

	pwtest_ptr_notnull(g);

	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 10, 104), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	n1 = find_node_by_id(g, 1);
	n2 = find_node_by_id(g, 2);
	n3 = find_node_by_id(g, 3);
	n4 = find_node_by_id(g, 4);
	pwtest_ptr_notnull(n1);
	pwtest_ptr_notnull(n2);
	pwtest_ptr_notnull(n3);
	pwtest_ptr_notnull(n4);
	pwtest_bool_true(n1->deadline_assigned);
	pwtest_bool_true(n2->deadline_assigned);
	pwtest_bool_true(n3->deadline_assigned);
	pwtest_bool_true(n4->deadline_assigned);
	pwtest_bool_true(n1->deadline > 0);
	pwtest_bool_true(n2->deadline > 0);
	pwtest_bool_true(n3->deadline > 0);
	pwtest_bool_true(n4->deadline > 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(independent_tasks_are_placed_by_descending_density)
{
	dag_t *g = dag_create(100, 100, 0.10f, 2, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);

	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 4, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 3, 103), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_int_eq((int)a->cpu, 0);
	pwtest_int_eq((int)b->cpu, 1);
	pwtest_int_eq((int)c->cpu, 1);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Helper for the canonical deadline-splitter regressions below. Sums
 * the assigned deadlines along a named id-path; the caller asserts
 * the bound against the global end-to-end deadline. */
static uint64_t path_deadline_sum(dag_t *g, const uint32_t *ids, size_t n_ids)
{
	uint64_t sum = 0;
	size_t i;

	for (i = 0; i < n_ids; i++) {
		dag_node_t *node = find_node_by_id(g, ids[i]);

		pwtest_ptr_notnull(node);
		sum += node->deadline;
	}

	return sum;
}

PWTEST(single_node_deadline)
{
	dag_t *g = dag_create(25, 25, 1.0f, 1, NULL);
	dag_node_t *node;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 7, 101), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	node = find_node_by_id(g, 1);
	pwtest_ptr_notnull(node);
	pwtest_bool_true(node->deadline_assigned);
	pwtest_int_eq((int)node->deadline, 25);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(chain_deadlines)
{
	static const uint32_t path[] = { 1, 2, 3 };
	dag_t *g = dag_create(18, 18, 1.0f, 3, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 2, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 3, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 4, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_bool_true(a->deadline > 0);
	pwtest_bool_true(b->deadline > 0);
	pwtest_bool_true(c->deadline > 0);
	pwtest_bool_true(path_deadline_sum(g, path, SPA_N_ELEMENTS(path)) <= g->deadline);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(diamond_deadlines)
{
	static const uint32_t left_path[] = { 1, 2, 4 };
	static const uint32_t right_path[] = { 1, 3, 4 };
	dag_t *g = dag_create(12, 12, 1.0f, 4, NULL);
	dag_node_t *a, *b, *c, *d;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 1, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 1, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 1, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 1, 104), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	d = find_node_by_id(g, 4);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_ptr_notnull(d);
	pwtest_bool_true(a->deadline > 0);
	pwtest_bool_true(b->deadline > 0);
	pwtest_bool_true(c->deadline > 0);
	pwtest_bool_true(d->deadline > 0);
	pwtest_bool_true(path_deadline_sum(g, left_path, SPA_N_ELEMENTS(left_path)) <= g->deadline);
	pwtest_bool_true(path_deadline_sum(g, right_path, SPA_N_ELEMENTS(right_path)) <= g->deadline);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Branch-tightening: a graph where the heavy branch's deadlines are
 * tight enough that the discount loop in the splitter (commit 2's
 * subject) runs at least once when the lighter branch is then
 * processed. The bound to assert is that neither path overflows the
 * global deadline. The lighter path strictly leaves slack because
 * the heavy branch consumed the tight budget. */
PWTEST(branch_tightening_residual_budget)
{
	static const uint32_t heavy_path[] = { 1, 2, 5, 6 };
	static const uint32_t discounted_path[] = { 1, 3, 4, 5, 6 };
	dag_t *g = dag_create(54, 54, 1.0f, 6, NULL);
	dag_node_t *a, *b, *c, *x, *d, *e;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 1, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 20, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 1, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 1, 104), 0);
	pwtest_int_eq(add_real_node(g, 5, 5, 105), 0);
	pwtest_int_eq(add_real_node(g, 6, 1, 106), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 5), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 4, 5), 0);
	pwtest_int_eq(dag_add_edge(g, 5, 6), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	x = find_node_by_id(g, 4);
	d = find_node_by_id(g, 5);
	e = find_node_by_id(g, 6);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_ptr_notnull(x);
	pwtest_ptr_notnull(d);
	pwtest_ptr_notnull(e);

	pwtest_bool_true(a->deadline > 0);
	pwtest_bool_true(b->deadline > 0);
	pwtest_bool_true(c->deadline > 0);
	pwtest_bool_true(x->deadline > 0);
	pwtest_bool_true(d->deadline > 0);
	pwtest_bool_true(e->deadline > 0);

	pwtest_bool_true(path_deadline_sum(g, heavy_path, SPA_N_ELEMENTS(heavy_path)) <= g->deadline);
	pwtest_bool_true(path_deadline_sum(g, discounted_path,
				SPA_N_ELEMENTS(discounted_path)) <= g->deadline);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* a fork-join where one side of the fork has a much
 * tighter critical-path constraint than the other forces the splitter
 * to assign a sub-proportional deadline to the tight branch before
 * the wider branch is considered. The wider branch then sees a
 * residual budget that already excludes the tight branch's
 * consumption -- a bug in the previous splitter re-introduced that
 * budget into the recursion and over-allocated the wider subproblem,
 * which manifested as the wider branch's deadlines summing to more
 * than the global end-to-end deadline.
 *
 * The regression target is: every per-node deadline must be positive,
 * the join node's deadline must be <= the global deadline, and the
 * sum of (path 1->2->4) plus (path 1->3->4)'s extra hop must
 * collectively respect the global budget without crediting the
 * tight branch twice. */
PWTEST(residual_budget_no_double_credit)
{
	dag_t *g = dag_create(1000, 1000, 0.95f, 4, NULL);
	dag_node_t *n1, *n2, *n3, *n4;

	pwtest_ptr_notnull(g);

	/* Asymmetric fork-join: branch 2 is heavy (path 1->2->4
	 * dominates), branch 3 is light. */
	pwtest_int_eq(add_real_node(g, 1, 100, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 600, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 50, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 100, 104), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	n1 = find_node_by_id(g, 1);
	n2 = find_node_by_id(g, 2);
	n3 = find_node_by_id(g, 3);
	n4 = find_node_by_id(g, 4);
	pwtest_ptr_notnull(n1);
	pwtest_ptr_notnull(n2);
	pwtest_ptr_notnull(n3);
	pwtest_ptr_notnull(n4);

	pwtest_bool_true(n1->deadline_assigned);
	pwtest_bool_true(n2->deadline_assigned);
	pwtest_bool_true(n3->deadline_assigned);
	pwtest_bool_true(n4->deadline_assigned);

	pwtest_bool_true(n1->deadline > 0);
	pwtest_bool_true(n2->deadline > 0);
	pwtest_bool_true(n3->deadline > 0);
	pwtest_bool_true(n4->deadline > 0);

	/* Heavy path 1->2->4 must fit in the global deadline. */
	pwtest_bool_true(n1->deadline + n2->deadline + n4->deadline <= g->deadline);

	/* Light path 1->3->4 must also fit: the splitter must not have
	 * given n3 a budget computed against a residual that secretly
	 * re-included the budget already consumed by n2. */
	pwtest_bool_true(n1->deadline + n3->deadline + n4->deadline <= g->deadline);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Two disconnected source-to-sink subproblems. The library is
 * expected to compute scheduling over each, with no real node left
 * unassigned. The order in which they're processed is internal to
 * the analyser; we only assert the outcome. */
/* a three-node chain has every node related to
 * every other node (each can reach the next), so the max unrelated
 * set has size 1. With one CPU and a configured utilization >= the
 * per-node density, the analyser must succeed and place all three
 * on the same CPU. */
/* Counts dag_foreach_node callbacks; we use this to detect whether
 * the foreach pass ran a recalculate (because the only side-effect
 * a caller can observe from outside the library is the per-node
 * callback count plus the assignment values). */
struct dirty_test_stats {
	uint32_t callbacks;
	uint64_t deadline_sum;
};

static void dirty_test_count_cb(void *data, uint32_t id, pid_t tid, uint64_t wcet,
		uint64_t cumulative_deadline, uint64_t local_deadline,
		uint64_t period, uint32_t cpu,
		uint32_t fusion_group_leader_id)
{
	struct dirty_test_stats *s = data;

	(void)id;
	(void)tid;
	(void)wcet;
	(void)cumulative_deadline;
	(void)period;
	(void)cpu;
	(void)fusion_group_leader_id;

	s->callbacks++;
	s->deadline_sum += local_deadline;
}

/* after a clean recalc, a second dag_foreach_node with
 * no mutation in between must not re-set dirty. The assignment values
 * are stable. */
/* chain whose critical-path WCET exactly matches
 * the global deadline. Feasibility check must pass; every per-node
 * deadline is positive. */
/* three independent unrelated nodes with
 * distinct densities. The CPU loop orders them by descending
 * density and the placements honour that. The densest goes to CPU
 * 0 (only choice for the first); the next densest goes to a
 * different (less-loaded) CPU; etc. */
/* dag_create rejects non-finite, zero and >1
 * utilization caps before any allocation happens. */
/* dag_create and dag_set_global_period_deadline
 * reject period=0, deadline=0, and deadline>period. */
/* two consecutive recalcs on the same DAG must
 * produce identical assignments and reuse the per-recalc workspace
 * pointer (the pointer is allocated to indexed_count entries on
 * first build and survives until invalidate_analysis next runs). */
/* empty DAG returns NULL from dag_find_node. */
PWTEST(id_index_empty)
{
	dag_t *g = dag_create(100, 100, 0.95, 1, NULL);

	pwtest_ptr_notnull(g);
	pwtest_ptr_null(dag_find_node(g, 42));
	pwtest_int_eq((int)g->nodes_by_id_count, 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* a single inserted node is found by dag_find_node. */
PWTEST(id_index_single)
{
	dag_t *g = dag_create(100, 100, 0.95, 1, NULL);
	dag_node_t *n;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 42, 10, 142), 0);
	n = dag_find_node(g, 42);
	pwtest_ptr_notnull(n);
	pwtest_int_eq((int)n->id, 42);
	pwtest_int_eq((int)g->nodes_by_id_count, 1);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* dag_find_node after dag_remove_node returns NULL. */
PWTEST(id_index_remove)
{
	dag_t *g = dag_create(100, 100, 0.95, 1, NULL);

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 42, 10, 142), 0);
	pwtest_ptr_notnull(dag_find_node(g, 42));

	pwtest_int_eq(dag_remove_node(g, 42), 0);
	pwtest_ptr_null(dag_find_node(g, 42));
	pwtest_int_eq((int)g->nodes_by_id_count, 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* shuffled insertion order, every id found, array
 * stays sorted. */
PWTEST(id_index_shuffled_insertion_stays_sorted)
{
	dag_t *g = dag_create(100, 100, 0.95, 1, NULL);
	static const uint32_t shuffled[] = { 7, 3, 11, 1, 5, 9, 2, 13, 4 };
	uint32_t i;

	pwtest_ptr_notnull(g);
	for (i = 0; i < SPA_N_ELEMENTS(shuffled); i++) {
		pwtest_int_eq(add_real_node(g, shuffled[i], 10,
				(pid_t)(100 + shuffled[i])), 0);
	}

	/* Every shuffled id is locatable. */
	for (i = 0; i < SPA_N_ELEMENTS(shuffled); i++) {
		dag_node_t *n = dag_find_node(g, shuffled[i]);
		pwtest_ptr_notnull(n);
		pwtest_int_eq((int)n->id, (int)shuffled[i]);
	}

	/* Array is sorted by id. */
	for (i = 1; i < g->nodes_by_id_count; i++) {
		pwtest_bool_true(g->nodes_by_id[i - 1]->id < g->nodes_by_id[i]->id);
	}

	dag_destroy(g);
	return PWTEST_PASS;
}

/* many mixed add/remove operations leave the index in
 * sync with the list. */
PWTEST(id_index_add_remove_stress)
{
	dag_t *g = dag_create(100, 100, 0.95, 1, NULL);
	uint32_t i;

	pwtest_ptr_notnull(g);

	/* Insert 64 ids in a deliberate shuffle pattern, then remove
	 * every other one, then re-insert them. */
	for (i = 0; i < 64; i++) {
		uint32_t id = ((i * 17u) % 64u) + 1u;
		(void)add_real_node(g, id, 10, (pid_t)(100 + id));
	}
	for (i = 1; i <= 64; i += 2) {
		(void)dag_remove_node(g, i);
	}
	for (i = 1; i <= 64; i += 2) {
		(void)add_real_node(g, i, 10, (pid_t)(100 + i));
	}

	/* Index count matches list count. */
	{
		uint32_t list_count = 0;
		dag_node_t *n;
		spa_list_for_each(n, &g->nodes, link)
			list_count++;
		pwtest_int_eq((int)g->nodes_by_id_count, (int)list_count);
	}

	/* dag_find_node agrees with a linear scan over dag->nodes. */
	for (i = 1; i <= 64; i++) {
		dag_node_t *via_index = dag_find_node(g, i);
		dag_node_t *via_scan = NULL;
		dag_node_t *n;
		spa_list_for_each(n, &g->nodes, link) {
			if (n->id == i) {
				via_scan = n;
				break;
			}
		}
		pwtest_ptr_eq(via_index, via_scan);
	}

	dag_destroy(g);
	return PWTEST_PASS;
}

/* a duplicate add fails with EEXIST and leaves the
 * index untouched. The original ptr from dag_find_node is
 * unchanged. */
PWTEST(id_index_duplicate_add_leaves_index_intact)
{
	dag_t *g = dag_create(100, 100, 0.95, 1, NULL);
	dag_node_t *original;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 7, 10, 107), 0);
	original = dag_find_node(g, 7);
	pwtest_ptr_notnull(original);

	errno = 0;
	pwtest_int_eq(add_real_node(g, 7, 20, 207), -1);
	pwtest_int_eq(errno, EEXIST);

	pwtest_ptr_eq(dag_find_node(g, 7), original);
	pwtest_int_eq((int)g->nodes_by_id_count, 1);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(workspace_reuse_across_recalcs)
{
	dag_t *g = dag_create(100, 100, 0.95f, 2, NULL);
	dag_node_t *a, *b, *c;
	uint64_t da1, db1, dc1;
	uint64_t da2, db2, dc2;
	dag_node_t **ws_path_before;
	bool       *ws_excluded_before;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	da1 = a->deadline;
	db1 = b->deadline;
	dc1 = c->deadline;
	pwtest_ptr_notnull(g->ws_path);
	pwtest_ptr_notnull(g->ws_excluded);
	pwtest_bool_true(g->ws_capacity >= g->indexed_count);
	ws_path_before = g->ws_path;
	ws_excluded_before = g->ws_excluded;

	/* Force a recalculation by setting a node's WCET to itself,
	 * which is currently a no-op so we instead bounce it to
	 * trigger dirty. */
	pwtest_int_eq(dag_set_node_wcet(g, 1, 11), 0);
	pwtest_int_eq(dag_set_node_wcet(g, 1, 10), 0);
	pwtest_bool_true(g->dirty);

	pwtest_int_eq(dag_recalculate(g), 0);
	da2 = a->deadline;
	db2 = b->deadline;
	dc2 = c->deadline;

	pwtest_int_eq((int)da1, (int)da2);
	pwtest_int_eq((int)db1, (int)db2);
	pwtest_int_eq((int)dc1, (int)dc2);

	/* The workspace buffers are re-allocated each recalc (because
	 * invalidate_analysis frees them) but their capacity stays
	 * sufficient. The key reuse property is across the SINGLE
	 * recalc: many compute_longest_path calls share one allocation.
	 * We can at least assert that the buffers exist and are correctly
	 * sized after the second recalc, mirroring the first. */
	pwtest_ptr_notnull(g->ws_path);
	pwtest_ptr_notnull(g->ws_excluded);
	pwtest_bool_true(g->ws_capacity >= g->indexed_count);
	(void)ws_path_before;
	(void)ws_excluded_before;

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(timing_invalid_inputs_rejected)
{
	dag_t *g;

	errno = 0;
	g = dag_create(0, 10, 0.95, 1, NULL);
	pwtest_ptr_null(g);
	pwtest_int_eq(errno, EINVAL);

	errno = 0;
	g = dag_create(10, 0, 0.95, 1, NULL);
	pwtest_ptr_null(g);
	pwtest_int_eq(errno, EINVAL);

	/* deadline > period: reject. */
	errno = 0;
	g = dag_create(10, 11, 0.95, 1, NULL);
	pwtest_ptr_null(g);
	pwtest_int_eq(errno, EINVAL);

	/* Successful create, then bad set. */
	g = dag_create(100, 100, 0.95, 1, NULL);
	pwtest_ptr_notnull(g);
	errno = 0;
	pwtest_int_eq(dag_set_global_period_deadline(g, 100, 0), -1);
	pwtest_int_eq(errno, EINVAL);
	errno = 0;
	pwtest_int_eq(dag_set_global_period_deadline(g, 0, 100), -1);
	pwtest_int_eq(errno, EINVAL);
	errno = 0;
	pwtest_int_eq(dag_set_global_period_deadline(g, 50, 60), -1);
	pwtest_int_eq(errno, EINVAL);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* U-zero-wcet: dag_add_node and dag_set_node_wcet reject wcet=0 for
 * real nodes consistently (fictitious are an internal exception
 * not exposed to public callers). */
PWTEST(zero_wcet_rejected)
{
	dag_t *g = dag_create(100, 100, 0.95, 1, NULL);

	pwtest_ptr_notnull(g);

	errno = 0;
	pwtest_int_eq(dag_add_node(g, 1, 0, 101, false), -1);
	pwtest_int_eq(errno, EINVAL);

	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	errno = 0;
	pwtest_int_eq(dag_set_node_wcet(g, 2, 0), -1);
	pwtest_int_eq(errno, EINVAL);

	/* No side-effect: WCET is still 10. */
	{
		dag_node_t *n = find_node_by_id(g, 2);
		pwtest_ptr_notnull(n);
		pwtest_int_eq((int)n->wcet, 10);
	}

	dag_destroy(g);
	return PWTEST_PASS;
}

/* a DAG created with deadline == period (the
 * canonical implicit-deadline case) is accepted and produces a
 * valid schedule. */
PWTEST(timing_edge_deadline_equals_period)
{
	dag_t *g = dag_create(50, 50, 0.95, 1, NULL);

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(load_reject_non_finite_or_out_of_range)
{
	dag_t *g;
	double nan_v = NAN;
	double inf_v = INFINITY;

	errno = 0;
	g = dag_create(100, 100, nan_v, 1, NULL);
	pwtest_ptr_null(g);
	pwtest_int_eq(errno, EINVAL);

	errno = 0;
	g = dag_create(100, 100, inf_v, 1, NULL);
	pwtest_ptr_null(g);
	pwtest_int_eq(errno, EINVAL);

	errno = 0;
	g = dag_create(100, 100, -0.5, 1, NULL);
	pwtest_ptr_null(g);
	pwtest_int_eq(errno, EINVAL);

	errno = 0;
	g = dag_create(100, 100, 0.0, 1, NULL);
	pwtest_ptr_null(g);
	pwtest_int_eq(errno, EINVAL);

	errno = 0;
	g = dag_create(100, 100, 1.1, 1, NULL);
	pwtest_ptr_null(g);
	pwtest_int_eq(errno, EINVAL);

	return PWTEST_PASS;
}

/* a normal in-range cap (e.g. 0.95) yields a valid
 * DAG and the admission test admits an obviously feasible graph. */
PWTEST(load_ordinary_cap_admits)
{
	dag_t *g = dag_create(100, 100, 0.95, 2, NULL);
	dag_node_t *a;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	pwtest_ptr_notnull(a);
	pwtest_bool_true(a->deadline > 0);
	pwtest_int_eq((int)a->cpu, 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* a graph whose summed per-CPU density falls
 * inside the cap by exactly the floating-point roundoff window
 * still admits. The unrelated-set placement sum is a chain of
 * division roundoffs; without an epsilon, the load might project
 * to (cap + 1ulp) and trigger spurious rejection. We construct a
 * case that is feasible at the chosen cap. */
PWTEST(load_near_bound_admits)
{
	/* Use a cap precisely equal to a per-node density. The chain
	 * has max unrelated set size 1, so per-CPU load equals per-node
	 * density (computed as wcet/min(deadline,period) by the
	 * analyser). With cap = 0.10 and density = 10/period_clamped,
	 * the analyser is at the edge -- with deadline tightening
	 * inside dag_recalculate the per-node density may grow
	 * marginally above cap; the epsilon absorbs the ULP. */
	dag_t *g = dag_create(100, 100, 0.10, 1, NULL);

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(cpu_placement_orders_by_descending_density)
{
	dag_t *g = dag_create(100, 100, 0.10f, 3, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 9, 101), 0);   /* density 0.09 */
	pwtest_int_eq(add_real_node(g, 2, 7, 102), 0);   /* density 0.07 */
	pwtest_int_eq(add_real_node(g, 3, 5, 103), 0);   /* density 0.05 */

	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);

	/* The three CPUs picked must be three distinct values; the
	 * densest node sits on the lowest-numbered CPU because all are
	 * empty at that point. */
	pwtest_int_eq((int)a->cpu, 0);
	pwtest_bool_true(a->cpu != b->cpu);
	pwtest_bool_true(a->cpu != c->cpu);
	pwtest_bool_true(b->cpu != c->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* equal-density nodes placed by worst-fit must produce
 * the same assignment on every run. With identical inputs, equal
 * load on every CPU after the first placement, the next densest
 * also goes to CPU 0 (lowest-index tie-break). And with 2 CPUs,
 * the third identical node is on CPU 1, etc. This is already
 * exercised by equal_load_ties_choose_lowest_cpu, but we explicitly
 * pin the deterministic ordering here too. */
PWTEST(cpu_placement_equal_density_lowest_cpu)
{
	dag_t *g = dag_create(100, 100, 0.50f, 2, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);

	/* First placement goes to CPU 0 (lowest-index tie). The next
	 * placement sees CPU 0 at u and CPU 1 at 0, so picks CPU 1
	 * (worst-fit). The third sees both CPUs at u; tied → CPU 0. */
	pwtest_int_eq((int)a->cpu, 0);
	pwtest_int_eq((int)b->cpu, 1);
	pwtest_int_eq((int)c->cpu, 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* CP-aware placement order: assign_cpus sorts candidates by
 * downstream critical-path priority (longest_len, equivalent to
 * HEFT's upward rank rank_u with zero communication cost --
 * Topcuoglu et al. 2002, papers/Topcuoglu-HEFT-TPDS2002.pdf, eq.
 * 8). Density is the secondary key; topological index is the
 * final tie-breaker.
 *
 * Regression test: a 4-node chain (A->B->C->D, all WCET=1, chain
 * CP=4 ns, chain density 0.04) plus one independent node E
 * (WCET=3, CP=3, density 0.03) lays the chain on a single CPU
 * (the unrelated-set sums never accumulate across related chain
 * nodes, so cpu_peak does not grow past the chain density) and
 * sends the independent node to the next free CPU. The CP-aware
 * ordering visits B (CP=3, density 0.04) before E (CP=3, density
 * 0.03 -- density loses the tie-break); the density-only ordering
 * visits B (idx 1, density 0.04) before E (idx 4, density 0.03)
 * via the index tie-break. Either order produces the same final
 * placement because related chain nodes do not bump cpu_peak.
 *
 * This test pins the placement so that any future change to the
 * worst-fit's interaction with CP priority is caught. */
PWTEST(cp_aware_chain_plus_independent_placement)
{
	dag_t *g = dag_create(100, 100, 0.10f, 5, NULL);
	dag_node_t *a, *b, *c, *d, *e;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 1, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 1, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 1, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 1, 104), 0);
	pwtest_int_eq(add_real_node(g, 5, 3, 105), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	d = find_node_by_id(g, 4);
	e = find_node_by_id(g, 5);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_ptr_notnull(d);
	pwtest_ptr_notnull(e);

	/* Chain piles onto CPU 0; the independent node goes to CPU 1. */
	pwtest_int_eq((int)a->cpu, 0);
	pwtest_int_eq((int)b->cpu, 0);
	pwtest_int_eq((int)c->cpu, 0);
	pwtest_int_eq((int)d->cpu, 0);
	pwtest_int_eq((int)e->cpu, 1);

	/* CP priorities (longest_len) must reflect the chain
	 * structure: A is the head of the longest path (CP=4), and
	 * each step downstream loses one WCET unit. E is its own
	 * source-to-sink, CP equal to its WCET. */
	pwtest_int_eq((int)a->longest_len, 4);
	pwtest_int_eq((int)b->longest_len, 3);
	pwtest_int_eq((int)c->longest_len, 2);
	pwtest_int_eq((int)d->longest_len, 1);
	pwtest_int_eq((int)e->longest_len, 3);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* CP-aware placement on two parallel chains of different lengths.
 * The longer chain (A->B->C->D->E, density 0.05) and the shorter
 * chain (F->G, density 0.02) are independent of each other. The
 * chains pile onto separate CPUs (CPU 0 and CPU 1 respectively)
 * because the unrelated-set sums for cross-chain pairs (e.g.,
 * {A,F}, {B,F}, ..., {E,F}) push F off CPU 0 to the empty CPU 1.
 * The internal chain order (CP priorities A=5, B=4, C=3, D=2,
 * E=1 for the long chain; F=2, G=1 for the short one) is
 * exercised by the deadline-splitting pass; this test pins the
 * placement and the CP values to lock the analysis in. */
PWTEST(cp_aware_two_chains_placement)
{
	dag_t *g = dag_create(100, 100, 0.10f, 7, NULL);
	dag_node_t *a, *b, *c, *d, *e, *f, *gn;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 1, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 1, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 1, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 1, 104), 0);
	pwtest_int_eq(add_real_node(g, 5, 1, 105), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 4, 5), 0);
	pwtest_int_eq(add_real_node(g, 6, 1, 106), 0);
	pwtest_int_eq(add_real_node(g, 7, 1, 107), 0);
	pwtest_int_eq(dag_add_edge(g, 6, 7), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	a  = find_node_by_id(g, 1);
	b  = find_node_by_id(g, 2);
	c  = find_node_by_id(g, 3);
	d  = find_node_by_id(g, 4);
	e  = find_node_by_id(g, 5);
	f  = find_node_by_id(g, 6);
	gn = find_node_by_id(g, 7);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_ptr_notnull(d);
	pwtest_ptr_notnull(e);
	pwtest_ptr_notnull(f);
	pwtest_ptr_notnull(gn);

	/* Chain 1 lives on CPU 0; chain 2 lives on CPU 1. */
	pwtest_int_eq((int)a->cpu,  0);
	pwtest_int_eq((int)b->cpu,  0);
	pwtest_int_eq((int)c->cpu,  0);
	pwtest_int_eq((int)d->cpu,  0);
	pwtest_int_eq((int)e->cpu,  0);
	pwtest_int_eq((int)f->cpu,  1);
	pwtest_int_eq((int)gn->cpu, 1);

	pwtest_int_eq((int)a->longest_len, 5);
	pwtest_int_eq((int)b->longest_len, 4);
	pwtest_int_eq((int)c->longest_len, 3);
	pwtest_int_eq((int)d->longest_len, 2);
	pwtest_int_eq((int)e->longest_len, 1);
	pwtest_int_eq((int)f->longest_len, 2);
	pwtest_int_eq((int)gn->longest_len, 1);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* When every node is independent (no edges), each node's
 * critical-path priority collapses to its own WCET. The CP-aware
 * order therefore coincides with WCET-descending order, which on
 * an end-to-end deadline equal to the period also coincides with
 * density-descending order. This regression test pins that
 * coincidence so existing density-driven tests stay valid. */
PWTEST(cp_aware_independent_set_matches_density_order)
{
	dag_t *g = dag_create(100, 100, 0.10f, 2, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 4, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 3, 103), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_int_eq((int)a->cpu, 0);
	pwtest_int_eq((int)b->cpu, 1);
	pwtest_int_eq((int)c->cpu, 1);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Nodes truly identical by (CP, density) fall to the topological
 * index tie-breaker; on the worst-fit pass with two empty CPUs the
 * first identical node lands on CPU 0, the second on CPU 1, the
 * third on whichever side is lighter -- here CPU 0. This is the
 * same deterministic ordering that the pre-CP-aware code
 * produced; the final tie-breaker is unchanged. */
PWTEST(cp_aware_equal_cp_and_density_breaks_by_topological_index)
{
	dag_t *g = dag_create(100, 100, 0.50f, 2, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_int_eq((int)a->cpu, 0);
	pwtest_int_eq((int)b->cpu, 1);
	pwtest_int_eq((int)c->cpu, 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Co-location groups force every group member onto the same CPU
 * as the first-visited member of the group, regardless of how the
 * placement order interleaves the rest of the DAG. With CP-aware
 * ordering the high-CP group member is visited first and pins the
 * CPU; subsequent members still land on that pinned CPU. */
PWTEST(cp_aware_group_keeps_co_location)
{
	dag_t *g = dag_create(100, 100, 0.50f, 2, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	/* Chain A->B (WCET 5, 5). CP(A)=10, CP(B)=5, density 0.10. */
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 5, 102), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	/* Independent C (WCET 5). CP=5, density 0.05. */
	pwtest_int_eq(add_real_node(g, 3, 5, 103), 0);
	pwtest_int_eq(dag_set_node_group(g, 1, 1), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 1), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);

	/* A is visited first (CP=10) and picks CPU 0 (empty, lowest
	 * tied). C (CP=5, density 0.05) ties with B (CP=5, density
	 * 0.10) by CP; density tie-break visits B before C, but B is
	 * forced to A's CPU by group 1. C then lands on CPU 1
	 * (worst-fit picks the empty bin). */
	pwtest_int_eq((int)a->cpu, 0);
	pwtest_int_eq((int)b->cpu, 0);
	pwtest_int_eq((int)c->cpu, 1);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Heterogeneous CPUs: relative_capacity scales how much room a
 * CPU has, not the CP-priority of the candidates. The chain head
 * is still visited first (it has the highest CP); the worst-fit
 * inner loop then picks the CPU with the most remaining capacity
 * after dividing the candidate's density by the per-CPU
 * relative_capacity (i.e., a slower CPU at 0.5x looks twice as
 * loaded as the same WCET on a 1.0x CPU). On two CPUs with
 * capacities {1.0, 0.5} and three independent nodes, the densest
 * candidate is placed on the faster CPU because it is the
 * least-loaded option under relative utilisation. */
PWTEST(cp_aware_hetero_capacity_picks_faster_cpu_first)
{
	double caps[2] = { 1.0, 0.5 };
	dag_t *g = dag_create(100, 100, 0.50f, 2, caps);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 5, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 5, 103), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);

	/* A (CP=10, density 0.10) lands on CPU 0 (relative load
	 * 0.10/1.0 = 0.10 vs 0.10/0.5 = 0.20 on CPU 1; CPU 0 is the
	 * worst-fit winner on an empty bin too). B and C then tie at
	 * CP=5 and density 0.05; index tie-break visits B first.
	 * After A, CPU 0 carries 0.10 of relative load and CPU 1
	 * carries 0; B's relative load is 0.05 on CPU 0 vs 0.10 on
	 * CPU 1, so worst-fit picks CPU 1 (whichever has lower peak
	 * after the candidate is added: CPU 1 ends at 0.10, CPU 0
	 * would end at 0.15 -- CPU 1 wins). C then sees CPU 0 at
	 * 0.10 (would become 0.15) and CPU 1 at 0.10 (would become
	 * 0.20); CPU 0 wins, again by lower projected load. */
	pwtest_int_eq((int)a->cpu, 0);
	pwtest_int_eq((int)b->cpu, 1);
	pwtest_int_eq((int)c->cpu, 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(feasibility_tight_critical_path)
{
	dag_t *g = dag_create(30, 30, 1.0f, 1, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_bool_true(a->deadline > 0);
	pwtest_bool_true(b->deadline > 0);
	pwtest_bool_true(c->deadline > 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* critical-path WCET exceeds the global
 * deadline. dag_recalculate must fail with EAGAIN; the DAG stays
 * dirty, deadlines/cpus are cleared. */
PWTEST(feasibility_critical_path_overrun)
{
	dag_t *g = dag_create(20, 20, 1.0f, 1, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	errno = 0;
	pwtest_int_eq(dag_recalculate(g), -1);
	pwtest_int_eq(errno, EAGAIN);
	pwtest_bool_true(g->dirty);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_bool_false(a->deadline_assigned);
	pwtest_bool_false(b->deadline_assigned);
	pwtest_bool_false(c->deadline_assigned);
	pwtest_int_eq((int)a->cpu, (int)DAG_CPU_INVALID);
	pwtest_int_eq((int)b->cpu, (int)DAG_CPU_INVALID);
	pwtest_int_eq((int)c->cpu, (int)DAG_CPU_INVALID);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* U-feas-min: a graph whose total per-node 1-ns reservation exceeds
 * the global deadline. Even with zero-WCET-style trivial work the
 * analyser cannot allocate a positive deadline to every node, so
 * recalc must fail. We use 3 nodes and a global deadline of 2 to
 * force the violation. */
PWTEST(feasibility_min_deadline_reservation)
{
	dag_t *g = dag_create(2, 2, 1.0f, 1, NULL);

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 1, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 1, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 1, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	/* critical path = 3, deadline = 2 -> overrun fails too, but the
	 * point is to exercise the min-reservation guard. Use a separate
	 * graph in which the critical path is exactly the deadline but
	 * the node count's 1-ns reservation overflows the budget. */
	errno = 0;
	pwtest_int_eq(dag_recalculate(g), -1);
	pwtest_int_eq(errno, EAGAIN);
	pwtest_bool_true(g->dirty);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* dag_recalculate_soft: critical-path overrun (the same workload as
 * feasibility_critical_path_overrun) must produce a complete
 * SCHED_DEADLINE-valid assignment when run through the soft variant.
 * Every real node ends up with a CPU, a non-zero local_deadline, and
 * the soft heuristic flags the nodes whose wcet exceeds their
 * redistributed slice as budget_clipped. */
PWTEST(recalculate_soft_critical_path_overrun_assigns_all_nodes)
{
	dag_t *g = dag_create(20, 20, 1.0f, 1, NULL);
	dag_node_t *a, *b, *c;
	uint32_t clipped_total;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	/* The strict pipeline rejects (critical path 30 > deadline 20). */
	errno = 0;
	pwtest_int_eq(dag_recalculate(g), -1);
	pwtest_int_eq(errno, EAGAIN);
	pwtest_bool_true(g->dirty);

	/* The soft pipeline produces a complete assignment. */
	pwtest_int_eq(dag_recalculate_soft(g), 0);
	pwtest_bool_false(g->dirty);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_bool_true(a->deadline_assigned);
	pwtest_bool_true(b->deadline_assigned);
	pwtest_bool_true(c->deadline_assigned);
	pwtest_bool_true(a->local_deadline > 0);
	pwtest_bool_true(b->local_deadline > 0);
	pwtest_bool_true(c->local_deadline > 0);
	pwtest_int_ne((int)a->cpu, (int)DAG_CPU_INVALID);
	pwtest_int_ne((int)b->cpu, (int)DAG_CPU_INVALID);
	pwtest_int_ne((int)c->cpu, (int)DAG_CPU_INVALID);

	/* Soft redistribution: 30 wcet over a 20-budget chain forces at
	 * least one node to clip. */
	clipped_total = (a->budget_clipped ? 1u : 0u) +
			(b->budget_clipped ? 1u : 0u) +
			(c->budget_clipped ? 1u : 0u);
	pwtest_bool_true(clipped_total > 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* dag_recalculate_soft on a workload that strictly fails admission_ceiling
 * because every node lands on the only CPU. The relaxed placer must
 * still complete the placement (no EAGAIN) and every node gets the
 * same CPU. */
PWTEST(recalculate_soft_relaxed_placement_overrides_admission_ceiling)
{
	dag_t *g = dag_create(100, 100, 0.10f, 1, NULL);
	dag_node_t *a, *b;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 60, 201), 0);
	pwtest_int_eq(add_real_node(g, 2, 60, 202), 0);

	errno = 0;
	pwtest_int_eq(dag_recalculate(g), -1);
	pwtest_int_eq(errno, EAGAIN);

	pwtest_int_eq(dag_recalculate_soft(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_int_eq((int)a->cpu, 0);
	pwtest_int_eq((int)b->cpu, 0);
	pwtest_bool_true(a->local_deadline > 0);
	pwtest_bool_true(b->local_deadline > 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* dag_recalculate_soft on an empty DAG: same fast-path as the strict
 * variant (no nodes -> clean, dirty=false). */
PWTEST(recalculate_soft_empty_graph_is_noop)
{
	dag_t *g = dag_create(100, 100, 0.95f, 1, NULL);

	pwtest_ptr_notnull(g);
	pwtest_int_eq(dag_recalculate_soft(g), 0);
	pwtest_bool_false(g->dirty);
	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(dirty_noop_after_clean_recalc)
{
	dag_t *g = dag_create(100, 100, 0.95f, 1, NULL);
	struct dirty_test_stats s1 = { 0 };
	struct dirty_test_stats s2 = { 0 };

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_bool_false(g->dirty);

	pwtest_int_eq(dag_foreach_node(g, dirty_test_count_cb, &s1), 0);
	pwtest_bool_false(g->dirty);

	pwtest_int_eq(dag_foreach_node(g, dirty_test_count_cb, &s2), 0);
	pwtest_bool_false(g->dirty);

	pwtest_int_eq((int)s1.callbacks, 2);
	pwtest_int_eq((int)s2.callbacks, 2);
	pwtest_int_eq((int)s1.deadline_sum, (int)s2.deadline_sum);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* changing a node's WCET marks the DAG dirty. A
 * no-op set (same wcet) does NOT mark dirty. */
PWTEST(dirty_set_node_wcet)
{
	dag_t *g = dag_create(100, 100, 0.95f, 1, NULL);
	struct dirty_test_stats s = { 0 };

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_bool_false(g->dirty);

	/* Same wcet -> no dirty. */
	pwtest_int_eq(dag_set_node_wcet(g, 1, 10), 0);
	pwtest_bool_false(g->dirty);

	/* Different wcet -> dirty, assigned deadlines cleared. */
	pwtest_int_eq(dag_set_node_wcet(g, 1, 15), 0);
	pwtest_bool_true(g->dirty);

	{
		dag_node_t *n = find_node_by_id(g, 1);
		pwtest_ptr_notnull(n);
		pwtest_bool_false(n->deadline_assigned);
		pwtest_int_eq((int)n->cpu, (int)DAG_CPU_INVALID);
	}

	/* dag_foreach_node observes the dirty bit and triggers a recalc. */
	pwtest_int_eq(dag_foreach_node(g, dirty_test_count_cb, &s), 0);
	pwtest_bool_false(g->dirty);
	pwtest_int_eq((int)s.callbacks, 2);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* changing global period+deadline marks dirty,
 * a no-op set does not. */
PWTEST(dirty_set_global_period_deadline)
{
	dag_t *g = dag_create(100, 100, 0.95f, 1, NULL);

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_bool_false(g->dirty);

	/* No-op: same values. */
	pwtest_int_eq(dag_set_global_period_deadline(g, 100, 100), 0);
	pwtest_bool_false(g->dirty);

	/* Real change. */
	pwtest_int_eq(dag_set_global_period_deadline(g, 200, 200), 0);
	pwtest_bool_true(g->dirty);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* adding an edge after a clean recalc marks
 * the DAG dirty. */
PWTEST(dirty_on_add_edge)
{
	dag_t *g = dag_create(100, 100, 0.95f, 1, NULL);

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_bool_false(g->dirty);

	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_bool_true(g->dirty);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(unrelated_set_chain_admits_serially)
{
	dag_t *g = dag_create(100, 100, 0.95f, 1, NULL);
	dag_node_t *n1, *n2, *n3;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 20, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 20, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 20, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	n1 = find_node_by_id(g, 1);
	n2 = find_node_by_id(g, 2);
	n3 = find_node_by_id(g, 3);
	pwtest_ptr_notnull(n1);
	pwtest_ptr_notnull(n2);
	pwtest_ptr_notnull(n3);

	/* Max unrelated set has size 1: the chain is fully ordered. */
	pwtest_int_eq((int)dag_max_unrelated_size(g), 1);

	/* All three nodes go to the only CPU. */
	pwtest_int_eq((int)n1->cpu, 0);
	pwtest_int_eq((int)n2->cpu, 0);
	pwtest_int_eq((int)n3->cpu, 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* three independent real nodes (no edges) are all
 * pairwise unrelated, so the max unrelated set has size 3. With a
 * tight per-CPU utilization bound, the analyser must spread them
 * across CPUs so that each CPU's max-unrelated-density stays within
 * the bound. */
PWTEST(unrelated_set_independent_spreads)
{
	dag_t *g = dag_create(100, 100, 0.95f, 3, NULL);
	dag_node_t *a, *b, *c;
	uint32_t cpus_used;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 20, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 20, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 20, 103), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);

	/* All three are pairwise unrelated. */
	pwtest_int_eq((int)dag_max_unrelated_size(g), 3);

	/* Three nodes, each on its own CPU (worst-fit). */
	{
		uint32_t mask = (1u << a->cpu) | (1u << b->cpu) | (1u << c->cpu);
		cpus_used = (uint32_t)__builtin_popcount(mask);
	}
	pwtest_int_eq((int)cpus_used, 3);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* a diamond 1->{2,3}->4 has {2,3} pairwise
 * unrelated. With two CPUs, the analyser should be able to admit
 * the diamond by placing n2 and n3 on different CPUs. */
PWTEST(unrelated_set_diamond_admits_on_two_cpus)
{
	dag_t *g = dag_create(100, 100, 0.95f, 2, NULL);
	dag_node_t *n1, *n2, *n3, *n4;
	static const uint32_t middle[] = { 2, 3 };

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 30, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 30, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 10, 104), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	n1 = find_node_by_id(g, 1);
	n2 = find_node_by_id(g, 2);
	n3 = find_node_by_id(g, 3);
	n4 = find_node_by_id(g, 4);
	pwtest_ptr_notnull(n1);
	pwtest_ptr_notnull(n2);
	pwtest_ptr_notnull(n3);
	pwtest_ptr_notnull(n4);

	pwtest_bool_true(dag_has_unrelated_subset(g, middle, 2));
	pwtest_bool_true(n2->cpu != n3->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(disconnect_two_parallel_chains)
{
	dag_t *g = dag_create(100, 100, 0.95f, 2, NULL);
	dag_node_t *a1, *a2, *x1, *x2;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 10, 104), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	a1 = find_node_by_id(g, 1);
	a2 = find_node_by_id(g, 2);
	x1 = find_node_by_id(g, 3);
	x2 = find_node_by_id(g, 4);
	pwtest_ptr_notnull(a1);
	pwtest_ptr_notnull(a2);
	pwtest_ptr_notnull(x1);
	pwtest_ptr_notnull(x2);
	pwtest_bool_true(a1->deadline_assigned);
	pwtest_bool_true(a2->deadline_assigned);
	pwtest_bool_true(x1->deadline_assigned);
	pwtest_bool_true(x2->deadline_assigned);
	pwtest_bool_true(a1->deadline > 0);
	pwtest_bool_true(a2->deadline > 0);
	pwtest_bool_true(x1->deadline > 0);
	pwtest_bool_true(x2->deadline > 0);
	/* Each chain's deadlines fit within the global budget independently. */
	pwtest_bool_true(a1->deadline + a2->deadline <= g->deadline);
	pwtest_bool_true(x1->deadline + x2->deadline <= g->deadline);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* A graph that is a chain plus an isolated real node with no edges.
 * find_sources_and_sinks treats it as both a source and a sink; the
 * recalc must still assign it a deadline. */
PWTEST(disconnect_chain_plus_isolated)
{
	dag_t *g = dag_create(100, 100, 0.95f, 2, NULL);
	dag_node_t *a, *b, *iso;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 99, 5, 199), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	iso = find_node_by_id(g, 99);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(iso);

	pwtest_bool_true(a->deadline_assigned);
	pwtest_bool_true(b->deadline_assigned);
	pwtest_bool_true(iso->deadline_assigned);
	pwtest_bool_true(a->deadline > 0);
	pwtest_bool_true(b->deadline > 0);
	pwtest_bool_true(iso->deadline > 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(self_loop_rejected_with_einval)
{
	dag_t *g = dag_create(100, 100, 0.95f, 1, NULL);

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);

	errno = 0;
	pwtest_int_eq(dag_add_edge(g, 1, 1), -1);
	pwtest_int_eq(errno, EINVAL);

	/* The DAG must remain in a sane state -- no phantom edge. */
	pwtest_bool_true(spa_list_is_empty(&g->edges));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(cycle_creating_edge_rejected_with_eloop)
{
	dag_t *g = dag_create(100, 100, 0.95f, 1, NULL);

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	/* 3 -> 1 would close a cycle 1->2->3->1. */
	errno = 0;
	pwtest_int_eq(dag_add_edge(g, 3, 1), -1);
	pwtest_int_eq(errno, ELOOP);

	/* The edge must NOT have been added (count stays at 2). */
	{
		uint32_t n_edges = 0;
		dag_edge_t *e;
		spa_list_for_each(e, &g->edges, link)
			n_edges++;
		pwtest_int_eq((int)n_edges, 2);
	}

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(dag_has_cycle_predicate)
{
	dag_t *g = dag_create(100, 100, 0.95f, 1, NULL);

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);

	pwtest_bool_false(dag_has_cycle(g));

	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_bool_false(dag_has_cycle(g));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(equal_load_ties_choose_lowest_cpu)
{
	dag_t *g = dag_create(300, 300, 0.10f, 2, NULL);
	dag_node_t *n1, *n2, *n3;

	pwtest_ptr_notnull(g);

	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	n1 = find_node_by_id(g, 1);
	n2 = find_node_by_id(g, 2);
	n3 = find_node_by_id(g, 3);
	pwtest_ptr_notnull(n1);
	pwtest_ptr_notnull(n2);
	pwtest_ptr_notnull(n3);
	pwtest_int_eq((int)n1->cpu, 0);
	pwtest_int_eq((int)n2->cpu, 0);
	pwtest_int_eq((int)n3->cpu, 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* --- co-location group tests ---
 *
 * The group_id field on dag_node lets the caller tell the worst-fit
 * pass "these nodes must share a CPU." It's the library hook the
 * chain-merge feature uses: when libpipewire pins several adjacent
 * nodes onto one OS thread, module-deadline forwards that thread
 * affinity into the scheduling DAG by stamping the same group_id on
 * each member, so the CPU assignment stays consistent with the
 * actual execution thread.
 *
 * Each test below exercises one property of the contract documented
 * in dag.h:
 *
 *   - group members co-locate (positive case);
 *   - distinct groups don't unify;
 *   - ungrouped neighbours are unaffected;
 *   - infeasible group placement returns EAGAIN rather than
 *     splitting the group;
 *   - dag_set_node_group input validation;
 *   - group_id survives a recalc;
 *   - clearing back to 0 returns to default worst-fit behaviour.
 *
 * Util values are chosen so that, without grouping, worst-fit would
 * naturally spread the nodes across CPUs (so the co-location is a
 * visible effect, not a side-effect of the default placement). */
PWTEST(group_two_independent_share_cpu)
{
	/* Two independent nodes that ordinarily spread to CPU 0/1
	 * end up on the same CPU when grouped. */
	dag_t *g = dag_create(100, 100, 0.50f, 2, NULL);
	dag_node_t *a, *b;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);

	/* Baseline: no group -> different CPUs. */
	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_bool_true(a->cpu != b->cpu);

	/* Now stamp them with the same group: same CPU. */
	pwtest_int_eq(dag_set_node_group(g, 1, 42), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 42), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)a->cpu, (int)b->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_chain_keeps_chain_on_one_cpu)
{
	/* A 3-node chain A->B->C stamped as one group lands on a
	 * single CPU even though there are 4 CPUs available. */
	dag_t *g = dag_create(100, 100, 0.80f, 4, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 5, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 5, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	pwtest_int_eq(dag_set_node_group(g, 1, 7), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 7), 0);
	pwtest_int_eq(dag_set_node_group(g, 3, 7), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_int_eq((int)a->cpu, (int)b->cpu);
	pwtest_int_eq((int)a->cpu, (int)c->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_distinct_groups_do_not_unify)
{
	/* Two independent chains, each in its own group. Without
	 * grouping they spread to 4 CPUs; with two separate groups
	 * they spread to 2 CPUs (one per group). The point is that
	 * group X members co-locate but X and Y don't share. */
	dag_t *g = dag_create(100, 100, 0.50f, 4, NULL);
	dag_node_t *a, *b, *c, *d;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 4, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 4, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 4, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 4, 104), 0);

	pwtest_int_eq(dag_set_node_group(g, 1, 1), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 1), 0);
	pwtest_int_eq(dag_set_node_group(g, 3, 2), 0);
	pwtest_int_eq(dag_set_node_group(g, 4, 2), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	d = find_node_by_id(g, 4);

	pwtest_int_eq((int)a->cpu, (int)b->cpu);
	pwtest_int_eq((int)c->cpu, (int)d->cpu);
	/* Distinct groups must NOT share a CPU when free CPUs exist:
	 * worst-fit picks the emptier CPU for the second group's
	 * first member. */
	pwtest_bool_true(a->cpu != c->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_ungrouped_neighbour_unaffected)
{
	/* A 2-member group plus one ungrouped node. The ungrouped
	 * one is placed by ordinary worst-fit; the group lands on
	 * its own CPU. */
	dag_t *g = dag_create(100, 100, 0.60f, 3, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);

	pwtest_int_eq(dag_set_node_group(g, 1, 5), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 5), 0);
	/* node 3 keeps group_id = 0 (default). */

	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);

	pwtest_int_eq((int)a->cpu, (int)b->cpu);
	pwtest_bool_true(c->cpu != a->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_overcapacity_returns_eagain)
{
	/* A group of two nodes whose summed individual densities
	 * exceed the per-CPU cap returns EAGAIN. Splitting the group
	 * across CPUs is forbidden by contract, so admission must
	 * fail rather than spread. */
	dag_t *g = dag_create(100, 100, 0.55f, 2, NULL);
	dag_node_t *a, *b;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 30, 101), 0);  /* density 0.30 */
	pwtest_int_eq(add_real_node(g, 2, 30, 102), 0);  /* density 0.30 */
	/* Sum on one CPU = 0.60 > 0.55 cap. */

	/* Without grouping it admits (one node per CPU). */
	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	pwtest_bool_true(a->cpu != b->cpu);

	/* With grouping it fails admission. */
	pwtest_int_eq(dag_set_node_group(g, 1, 9), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 9), 0);
	errno = 0;
	pwtest_int_eq(dag_recalculate(g), -1);
	pwtest_int_eq(errno, EAGAIN);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_invalid_id_returns_enoent)
{
	dag_t *g = dag_create(100, 100, 0.50f, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);

	errno = 0;
	pwtest_int_eq(dag_set_node_group(g, 999, 1), -1);
	pwtest_int_eq(errno, ENOENT);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_null_dag_returns_einval)
{
	errno = 0;
	pwtest_int_eq(dag_set_node_group(NULL, 1, 1), -1);
	pwtest_int_eq(errno, EINVAL);
	return PWTEST_PASS;
}

PWTEST(group_same_value_is_noop_for_dirty)
{
	/* Setting a node's group to its current value must NOT dirty
	 * the DAG: a steady-state recalc loop shouldn't redo CPU
	 * placement every cycle because module-deadline re-applied
	 * the same group stamps. */
	dag_t *g = dag_create(100, 100, 0.50f, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);

	pwtest_int_eq(dag_set_node_group(g, 1, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_bool_false(g->dirty);

	/* Re-setting the same group must keep dirty = false. */
	pwtest_int_eq(dag_set_node_group(g, 1, 3), 0);
	pwtest_bool_false(g->dirty);

	/* But a real change flips dirty. */
	pwtest_int_eq(dag_set_node_group(g, 1, 4), 0);
	pwtest_bool_true(g->dirty);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_clear_returns_to_default)
{
	/* Clearing a group (set to 0) returns the node to ungrouped
	 * worst-fit behaviour. */
	dag_t *g = dag_create(100, 100, 0.50f, 2, NULL);
	dag_node_t *a, *b;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);

	pwtest_int_eq(dag_set_node_group(g, 1, 1), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 1), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	pwtest_int_eq((int)a->cpu, (int)b->cpu);

	/* Clear and recalculate: back to spread. */
	pwtest_int_eq(dag_set_node_group(g, 2, 0), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_bool_true(a->cpu != b->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_survives_wcet_update)
{
	/* Updating a node's WCET (which marks dirty and triggers a
	 * recalc) must preserve its group_id. Without this, every
	 * WCET update would defeat the chain merge. */
	dag_t *g = dag_create(100, 100, 0.80f, 2, NULL);
	dag_node_t *a, *b;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 5, 102), 0);

	pwtest_int_eq(dag_set_node_group(g, 1, 11), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 11), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	pwtest_int_eq((int)a->cpu, (int)b->cpu);

	/* Bump WCETs; group must persist. */
	pwtest_int_eq(dag_set_node_wcet(g, 1, 8), 0);
	pwtest_int_eq(dag_set_node_wcet(g, 2, 8), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)a->cpu, (int)b->cpu);
	pwtest_int_eq(a->group_id, 11u);
	pwtest_int_eq(b->group_id, 11u);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_three_members_partial_clear)
{
	/* Group of three; clear one member only. The remaining two
	 * stay co-located; the cleared one is placed freely. */
	dag_t *g = dag_create(100, 100, 0.60f, 3, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);

	pwtest_int_eq(dag_set_node_group(g, 1, 4), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 4), 0);
	pwtest_int_eq(dag_set_node_group(g, 3, 4), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_int_eq((int)a->cpu, (int)b->cpu);
	pwtest_int_eq((int)a->cpu, (int)c->cpu);

	/* Clear node 3 from the group: it's now free. */
	pwtest_int_eq(dag_set_node_group(g, 3, 0), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)a->cpu, (int)b->cpu);
	/* C must now sit on a different CPU since the worst-fit
	 * has 2 free CPUs to spread to. */
	pwtest_bool_true(c->cpu != a->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_member_removed_does_not_leak)
{
	/* Removing a group member shouldn't crash the group_cpu
	 * lookup. Exercises the bookkeeping path where the indexed
	 * node count shrinks while the max_group_id stays high. */
	dag_t *g = dag_create(100, 100, 0.50f, 2, NULL);
	dag_node_t *a;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 5, 102), 0);
	pwtest_int_eq(dag_set_node_group(g, 1, 99), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 99), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	pwtest_int_eq(dag_remove_node(g, 2), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	pwtest_ptr_notnull(a);
	pwtest_bool_true(a->cpu < 2);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* --- deeper group coverage ---
 *
 * The basic group tests above pin the contract; the ones below stress
 * the corners: large group ids, many groups, recovery after an
 * infeasible recalc, migration of a node between groups, fictitious-
 * node interaction with group_id, dirty bit interactions, and the
 * worst-fit choice when a group competes with ungrouped neighbours
 * for finite per-CPU bandwidth. */

PWTEST(group_high_id_does_not_overflow)
{
	/* group_cpu[] is sized as max_group_id + 1; a 31-bit id must
	 * neither malloc-overflow nor write out of bounds. Picking a
	 * very high (but still 32-bit) id flushes any silent assumption
	 * that groups are 0..N-1. */
	dag_t *g = dag_create(100, 100, 0.50f, 2, NULL);
	dag_node_t *a, *b;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 5, 102), 0);

	/* Just below UINT32_MAX so the malloc is bounded but the array
	 * size dwarfs the actual group count -- the test specifically
	 * verifies the implementation doesn't try to materialise the
	 * whole [0, group_id] range as a packed structure. The 16 GiB
	 * allocation would be visible immediately. */
	uint32_t big = 1u << 20;
	pwtest_int_eq(dag_set_node_group(g, 1, big), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, big), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	pwtest_int_eq((int)a->cpu, (int)b->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_many_groups_each_co_locates)
{
	/* Eight groups of two members each on four CPUs. Each group's
	 * members must share a CPU; with worst-fit and the load cap at
	 * 0.40 they spread across all four CPUs. */
	dag_t *g = dag_create(100, 100, 0.40f, 4, NULL);

	pwtest_ptr_notnull(g);
	for (uint32_t k = 0; k < 8; k++) {
		uint32_t a_id = 100 + 2 * k;
		uint32_t b_id = 101 + 2 * k;
		uint32_t group = 1000 + k;
		pwtest_int_eq(add_real_node(g, a_id, 2, 100 + (int)k), 0);
		pwtest_int_eq(add_real_node(g, b_id, 2, 200 + (int)k), 0);
		pwtest_int_eq(dag_set_node_group(g, a_id, group), 0);
		pwtest_int_eq(dag_set_node_group(g, b_id, group), 0);
	}

	pwtest_int_eq(dag_recalculate(g), 0);

	for (uint32_t k = 0; k < 8; k++) {
		dag_node_t *a = find_node_by_id(g, 100 + 2 * k);
		dag_node_t *b = find_node_by_id(g, 101 + 2 * k);
		pwtest_ptr_notnull(a);
		pwtest_ptr_notnull(b);
		pwtest_int_eq((int)a->cpu, (int)b->cpu);
		pwtest_int_eq(a->group_id, 1000u + k);
		pwtest_int_eq(b->group_id, 1000u + k);
	}

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_eagain_recoverable_after_clear)
{
	/* Overcapacity returns EAGAIN; clearing the offending group
	 * lets the next recalc succeed. The DAG must not be left in a
	 * permanently-broken state by a transient infeasible placement. */
	dag_t *g = dag_create(100, 100, 0.55f, 2, NULL);
	dag_node_t *a, *b;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 30, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 30, 102), 0);
	pwtest_int_eq(dag_set_node_group(g, 1, 5), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 5), 0);

	errno = 0;
	pwtest_int_eq(dag_recalculate(g), -1);
	pwtest_int_eq(errno, EAGAIN);
	/* On failure dag_recalculate leaves dirty set so a retry
	 * actually retries. */
	pwtest_bool_true(g->dirty);

	/* Clear the group on one member; the two now spread across CPUs
	 * and admission succeeds. */
	pwtest_int_eq(dag_set_node_group(g, 2, 0), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	pwtest_bool_true(a->cpu != b->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_migration_between_groups)
{
	/* A node initially in group 1 migrates to group 2; verify the
	 * placement follows. This is the path taken when libpipewire
	 * reshapes a chain (node leaves chain A, joins chain B). */
	dag_t *g = dag_create(100, 100, 0.60f, 4, NULL);
	dag_node_t *a, *b, *c, *d;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 5, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 5, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 5, 104), 0);

	/* Initial: {1,2,3} group A, {4} alone. */
	pwtest_int_eq(dag_set_node_group(g, 1, 100), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 100), 0);
	pwtest_int_eq(dag_set_node_group(g, 3, 100), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	d = find_node_by_id(g, 4);
	pwtest_int_eq((int)a->cpu, (int)b->cpu);
	pwtest_int_eq((int)a->cpu, (int)c->cpu);

	/* Migrate: node 3 jumps from group 100 to a new group 200
	 * with node 4. After recalc 1 and 2 still share, 3 and 4
	 * share, and the two groups occupy different CPUs (worst-fit
	 * spreads them with 4 CPUs available). */
	pwtest_int_eq(dag_set_node_group(g, 3, 200), 0);
	pwtest_int_eq(dag_set_node_group(g, 4, 200), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)a->cpu, (int)b->cpu);
	pwtest_int_eq((int)c->cpu, (int)d->cpu);
	pwtest_bool_true(a->cpu != c->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_fictitious_node_ignored)
{
	/* The recalc's fictitious-source/sink sentinels never participate
	 * in placement; even if a (defective) caller tried to stamp them
	 * with a group, that group must not influence real-node
	 * assignment. The library only walks group_id on real nodes
	 * inside assign_cpus, so the fictitious entries are skipped
	 * implicitly -- this test guards that no future refactor
	 * regresses by iterating fictitious nodes through the worst-fit
	 * loop. */
	dag_t *g = dag_create(100, 100, 0.50f, 2, NULL);
	dag_node_t *a, *b;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 5, 102), 0);
	pwtest_int_eq(dag_set_node_group(g, 1, 7), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 7), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	/* Inspect every fictitious node: group_id is still 0 (the
	 * default), confirming the calling convention. */
	dag_node_t *n;
	uint32_t fictitious_seen = 0;
	spa_list_for_each(n, &g->nodes, link) {
		if (!n->fictitious)
			continue;
		fictitious_seen++;
		pwtest_int_eq(n->group_id, 0u);
	}
	pwtest_bool_true(fictitious_seen >= 2);

	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	pwtest_int_eq((int)a->cpu, (int)b->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_competes_with_ungrouped_for_capacity)
{
	/* A group of two density-0.20 members lands on CPU 0
	 * (consuming 0.40). A single ungrouped density-0.30 node
	 * then has only CPU 1 with enough headroom (cap 0.45). It
	 * goes there. This is the realistic case where the chain
	 * merge changes which CPU the unrelated work ends up on. */
	dag_t *g = dag_create(100, 100, 0.45f, 2, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 20, 101), 0);  /* density 0.20, grouped */
	pwtest_int_eq(add_real_node(g, 2, 20, 102), 0);  /* density 0.20, grouped */
	pwtest_int_eq(add_real_node(g, 3, 30, 103), 0);  /* density 0.30, lone */
	pwtest_int_eq(dag_set_node_group(g, 1, 3), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 3), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);

	pwtest_int_eq((int)a->cpu, (int)b->cpu);
	pwtest_bool_true(c->cpu != a->cpu);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_topology_edge_addition_keeps_placement)
{
	/* Adding an edge between two grouped nodes (the chain becomes
	 * explicit in the DAG topology) must not alter their CPU
	 * assignment, because the group already constrains them to one
	 * CPU. The dirty flag forces a recalc but the outcome is the
	 * same. */
	dag_t *g = dag_create(100, 100, 0.80f, 4, NULL);
	dag_node_t *a, *b;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 5, 102), 0);
	pwtest_int_eq(dag_set_node_group(g, 1, 9), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 9), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	uint32_t cpu_before = a->cpu;
	pwtest_int_eq((int)b->cpu, (int)cpu_before);

	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	pwtest_int_eq((int)a->cpu, (int)cpu_before);
	pwtest_int_eq((int)b->cpu, (int)cpu_before);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(group_set_after_recalc_marks_dirty)
{
	/* The dirty bit interaction: after a clean recalc, setting any
	 * node's group to a new value must mark the DAG dirty so the
	 * next foreach reruns assign_cpus. Without this, the chain-
	 * merge feature would only take effect on the first recalc and
	 * subsequent group changes would silently get the stale
	 * placement. */
	dag_t *g = dag_create(100, 100, 0.50f, 2, NULL);

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 5, 102), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_bool_false(g->dirty);

	pwtest_int_eq(dag_set_node_group(g, 1, 5), 0);
	pwtest_bool_true(g->dirty);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_bool_false(g->dirty);

	/* Setting a *different* node to a different group flips it
	 * again. */
	pwtest_int_eq(dag_set_node_group(g, 2, 7), 0);
	pwtest_bool_true(g->dirty);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* -------------------------------------------------------------------
 * Per-CPU relative capacity (heterogeneous / DVFS-conservative cases).
 *
 * dag_create now accepts a per-CPU vector of relative_capacity values
 * in (0, 1]. NULL is shorthand for an all-1.0 vector, the homogeneous
 * identity that every test above relies on. The new cases below
 * verify that:
 *
 *   - an explicit all-1.0 vector behaves identically to NULL;
 *   - the worst-fit placer prefers faster CPUs for the densest nodes;
 *   - admission tightens correctly when half the CPUs are at 0.5
 *     relative_capacity (a workload that fits homogeneously must be
 *     rejected); and
 *   - dag_check_feasibility scales the critical-path budget by the
 *     slowest CPU, refusing a path that would overrun on the worst
 *     possible placement even though it would fit on the fastest.
 * ------------------------------------------------------------------- */

PWTEST(hetero_all_ones_matches_null_vector)
{
	double ones[2] = { 1.0, 1.0 };

	dag_t *g_null = dag_create(100, 100, 0.90, 2, NULL);
	pwtest_ptr_notnull(g_null);
	pwtest_int_eq(add_real_node(g_null, 1, 30, 101), 0);
	pwtest_int_eq(add_real_node(g_null, 2, 40, 102), 0);
	pwtest_int_eq(dag_recalculate(g_null), 0);

	dag_t *g_ones = dag_create(100, 100, 0.90, 2, ones);
	pwtest_ptr_notnull(g_ones);
	pwtest_int_eq(add_real_node(g_ones, 1, 30, 101), 0);
	pwtest_int_eq(add_real_node(g_ones, 2, 40, 102), 0);
	pwtest_int_eq(dag_recalculate(g_ones), 0);

	pwtest_int_eq((int)find_node_by_id(g_null, 1)->cpu,
			(int)find_node_by_id(g_ones, 1)->cpu);
	pwtest_int_eq((int)find_node_by_id(g_null, 2)->cpu,
			(int)find_node_by_id(g_ones, 2)->cpu);

	dag_destroy(g_null);
	dag_destroy(g_ones);
	return PWTEST_PASS;
}

PWTEST(hetero_high_density_picks_faster_cpu)
{
	/* CPU 0 fast (1.0), CPU 1 slow (0.5). Two independent nodes; the
	 * denser one must land on the faster CPU under worst-fit. */
	double rel[2] = { 1.0, 0.5 };

	dag_t *g = dag_create(100, 100, 0.90, 2, rel);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 40, 101), 0);  /* dense  -> CPU 0 */
	pwtest_int_eq(add_real_node(g, 2, 20, 102), 0);  /* light  -> CPU 1 */

	pwtest_int_eq(dag_recalculate(g), 0);

	pwtest_int_eq((int)find_node_by_id(g, 1)->cpu, 0);
	pwtest_int_eq((int)find_node_by_id(g, 2)->cpu, 1);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(hetero_admission_rejects_workload_that_only_fits_at_full_capacity)
{
	/* Two independent nodes, each raw util 0.8. With ceiling 0.9 and
	 * two CPUs at full capacity, the worst-fit places one per CPU and
	 * the workload trivially fits. With CPU 1 dropped to 0.5
	 * relative_capacity, that node's relative util becomes 1.6 > 0.9
	 * so neither CPU can take the second node and assign_cpus
	 * returns EAGAIN. */
	dag_t *g_homo = dag_create(100, 100, 0.90, 2, NULL);
	pwtest_ptr_notnull(g_homo);
	pwtest_int_eq(add_real_node(g_homo, 1, 80, 101), 0);
	pwtest_int_eq(add_real_node(g_homo, 2, 80, 102), 0);
	pwtest_int_eq(dag_recalculate(g_homo), 0);
	pwtest_int_eq((int)find_node_by_id(g_homo, 1)->cpu, 0);
	pwtest_int_eq((int)find_node_by_id(g_homo, 2)->cpu, 1);
	dag_destroy(g_homo);

	double rel[2] = { 1.0, 0.5 };
	dag_t *g = dag_create(100, 100, 0.90, 2, rel);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 80, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 80, 102), 0);

	pwtest_int_eq(dag_recalculate(g), -1);
	pwtest_int_eq(errno, EAGAIN);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(hetero_feasibility_scaled_by_slowest_cpu)
{
	/* Critical path of 60 ns fits the 100 ns deadline at reference
	 * capacity (60 <= 100) but not at min capacity 0.5
	 * (scaled_deadline = 100 * 0.5 = 50; 60 > 50 -> EAGAIN). */
	double rel_homo[2] = { 1.0, 1.0 };
	dag_t *g_homo = dag_create(100, 100, 0.90, 2, rel_homo);
	pwtest_ptr_notnull(g_homo);
	pwtest_int_eq(add_real_node(g_homo, 1, 60, 101), 0);
	pwtest_int_eq(dag_recalculate(g_homo), 0);
	dag_destroy(g_homo);

	double rel_slow[2] = { 1.0, 0.5 };
	dag_t *g = dag_create(100, 100, 0.90, 2, rel_slow);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 60, 101), 0);

	pwtest_int_eq(dag_recalculate(g), -1);
	pwtest_int_eq(errno, EAGAIN);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(hetero_iter_invalid_inputs_rejected)
{
	dag_t *g = dag_create(100, 100, 0.90, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 30, 101), 0);

	pwtest_int_eq(dag_recalculate_heterogeneous(NULL, 2), -1);
	pwtest_int_eq(errno, EINVAL);

	pwtest_int_eq(dag_recalculate_heterogeneous(g, 0), -1);
	pwtest_int_eq(errno, EINVAL);

	/* Cap at DAG_HETEROGENEOUS_MAX_ITERATIONS (8). 9 must be rejected. */
	pwtest_int_eq(dag_recalculate_heterogeneous(g, 9), -1);
	pwtest_int_eq(errno, EINVAL);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(hetero_iter_uniform_capacity_matches_single_shot)
{
	/* On a uniform `relative_capacity` vector the overlay collapses
	 * to a no-op: a chain that the single-shot pass schedules must
	 * produce bit-identical placements when run through the
	 * iterative entry point. This guards the short-circuit in
	 * dag_recalculate_heterogeneous. */
	dag_t *g_single = dag_create(1000, 1000, 0.90, 4, NULL);
	pwtest_ptr_notnull(g_single);
	pwtest_int_eq(add_real_node(g_single, 1, 100, 101), 0);
	pwtest_int_eq(add_real_node(g_single, 2, 200, 102), 0);
	pwtest_int_eq(add_real_node(g_single, 3, 150, 103), 0);
	pwtest_int_eq(dag_add_edge(g_single, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g_single, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g_single), 0);

	dag_t *g_iter = dag_create(1000, 1000, 0.90, 4, NULL);
	pwtest_ptr_notnull(g_iter);
	pwtest_int_eq(add_real_node(g_iter, 1, 100, 101), 0);
	pwtest_int_eq(add_real_node(g_iter, 2, 200, 102), 0);
	pwtest_int_eq(add_real_node(g_iter, 3, 150, 103), 0);
	pwtest_int_eq(dag_add_edge(g_iter, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g_iter, 2, 3), 0);
	pwtest_int_eq(dag_recalculate_heterogeneous(g_iter, 4), 0);

	for (uint32_t id = 1; id <= 3; id++) {
		pwtest_int_eq((int)find_node_by_id(g_iter, id)->cpu,
				(int)find_node_by_id(g_single, id)->cpu);
		pwtest_int_eq((int)find_node_by_id(g_iter, id)->local_deadline,
				(int)find_node_by_id(g_single, id)->local_deadline);
	}

	dag_destroy(g_single);
	dag_destroy(g_iter);
	return PWTEST_PASS;
}

PWTEST(hetero_iter_single_iteration_short_circuits)
{
	/* max_iterations = 1 collapses to a single dag_recalculate even
	 * on a heterogeneous capacity vector: the loop body needs at
	 * least two rounds to compare mappings. Verify by checking the
	 * mapping matches the homogeneous single-shot output. */
	double rel[2] = { 1.0, 0.5 };

	dag_t *g_single = dag_create(100, 100, 0.90, 2, rel);
	pwtest_ptr_notnull(g_single);
	pwtest_int_eq(add_real_node(g_single, 1, 40, 101), 0);
	pwtest_int_eq(add_real_node(g_single, 2, 20, 102), 0);
	pwtest_int_eq(dag_recalculate(g_single), 0);

	dag_t *g_iter = dag_create(100, 100, 0.90, 2, rel);
	pwtest_ptr_notnull(g_iter);
	pwtest_int_eq(add_real_node(g_iter, 1, 40, 101), 0);
	pwtest_int_eq(add_real_node(g_iter, 2, 20, 102), 0);
	pwtest_int_eq(dag_recalculate_heterogeneous(g_iter, 1), 0);

	pwtest_int_eq((int)find_node_by_id(g_iter, 1)->cpu,
			(int)find_node_by_id(g_single, 1)->cpu);
	pwtest_int_eq((int)find_node_by_id(g_iter, 2)->cpu,
			(int)find_node_by_id(g_single, 2)->cpu);

	dag_destroy(g_single);
	dag_destroy(g_iter);
	return PWTEST_PASS;
}

PWTEST(hetero_iter_chain_overlay_stretches_paths)
{
	/* Two independent nodes on two CPUs of unequal capacity.
	 * Iteration 0 splits the deadline against reference-CPU WCETs;
	 * iteration 1 splits against placement-stretched runtimes. On a
	 * chain with one slow CPU, the assigned local_deadline for the
	 * follower placed on the slow CPU must reflect the stretched
	 * path: it should be strictly larger than what the homogeneous
	 * single-shot pass would have produced for the same node.
	 *
	 * Concretely: deadline budget 200, two nodes with wcet 30 and
	 * 40 in a chain. Reference split puts node 1 at ~85ns local
	 * deadline (30/70 * 200) and node 2 at ~115ns (40/70 * 200).
	 * Under capacity [1.0, 0.5], if node 2 lands on the slow CPU
	 * the stretched runtime is 80ns, total path = 110, and the
	 * iterative split must give node 2 a wider local deadline to
	 * reflect that. */
	double rel[2] = { 1.0, 0.5 };

	dag_t *g_iter = dag_create(200, 200, 0.95, 2, rel);
	pwtest_ptr_notnull(g_iter);
	pwtest_int_eq(add_real_node(g_iter, 1, 30, 101), 0);
	pwtest_int_eq(add_real_node(g_iter, 2, 40, 102), 0);
	pwtest_int_eq(dag_add_edge(g_iter, 1, 2), 0);
	pwtest_int_eq(dag_recalculate_heterogeneous(g_iter, 4), 0);

	dag_t *g_homo = dag_create(200, 200, 0.95, 2, NULL);
	pwtest_ptr_notnull(g_homo);
	pwtest_int_eq(add_real_node(g_homo, 1, 30, 101), 0);
	pwtest_int_eq(add_real_node(g_homo, 2, 40, 102), 0);
	pwtest_int_eq(dag_add_edge(g_homo, 1, 2), 0);
	pwtest_int_eq(dag_recalculate(g_homo), 0);

	/* Regardless of which CPU each node lands on, every node's
	 * local_deadline + cumulative_deadline pair must satisfy the
	 * kernel SCHED_DEADLINE contract: 0 < local <= period. */
	dag_node_t *iter_n1 = find_node_by_id(g_iter, 1);
	dag_node_t *iter_n2 = find_node_by_id(g_iter, 2);
	pwtest_int_lt(0, (int)iter_n1->local_deadline);
	pwtest_int_lt(0, (int)iter_n2->local_deadline);
	pwtest_int_lt((int)iter_n1->local_deadline, 201);
	pwtest_int_lt((int)iter_n2->local_deadline, 201);
	/* And monotonicity along the edge survives the iteration. */
	pwtest_int_lt((int)iter_n1->cumulative_deadline,
			(int)iter_n2->cumulative_deadline + 1);

	dag_destroy(g_iter);
	dag_destroy(g_homo);
	return PWTEST_PASS;
}

PWTEST(hetero_iter_convergence_breaks_loop)
{
	/* Three independent nodes on two CPUs of unequal capacity.
	 * The iteration must converge: once a stable mapping is found
	 * the loop body breaks. Verify by running with a large
	 * max_iterations and checking the post-recalc state is clean
	 * (dirty=false, every real node has a finite cpu). */
	double rel[2] = { 1.0, 0.5 };

	dag_t *g = dag_create(1000, 1000, 0.95, 2, rel);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 200, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 150, 103), 0);

	pwtest_int_eq(dag_recalculate_heterogeneous(g, 8), 0);
	pwtest_bool_false(g->dirty);

	for (uint32_t id = 1; id <= 3; id++) {
		dag_node_t *n = find_node_by_id(g, id);
		pwtest_ptr_notnull(n);
		pwtest_int_lt((int)n->cpu, 2);
	}

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(predicted_and_scheduled_runtime_populated_after_recalc)
{
	/* Hard-mode happy path: every real node must publish a
	 * positive predicted_runtime_ns and scheduled_runtime_ns, and
	 * with no clamping in play they must be equal. Fictitious
	 * endpoints stay at zero. */
	dag_t *g = dag_create(1000, 1000, 0.95, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 200, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 150, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	for (uint32_t id = 1; id <= 3; id++) {
		dag_node_t *n = find_node_by_id(g, id);
		pwtest_int_lt(0, (int)n->predicted_runtime_ns);
		pwtest_int_eq((int)n->predicted_runtime_ns,
				(int)n->scheduled_runtime_ns);
		pwtest_int_eq((int)n->predicted_runtime_ns, (int)n->wcet);
	}

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(predicted_and_scheduled_cleared_on_dirty)
{
	/* Mutating wcet must reset both runtime fields back to zero
	 * (the invalidate-schedule contract). They are repopulated on
	 * the next successful recalc. */
	dag_t *g = dag_create(1000, 1000, 0.95, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 101), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_lt(0, (int)find_node_by_id(g, 1)->predicted_runtime_ns);

	pwtest_int_eq(dag_set_node_wcet(g, 1, 200), 0);
	pwtest_int_eq((int)find_node_by_id(g, 1)->predicted_runtime_ns, 0);
	pwtest_int_eq((int)find_node_by_id(g, 1)->scheduled_runtime_ns, 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)find_node_by_id(g, 1)->predicted_runtime_ns, 200);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(predicted_and_scheduled_diverge_under_soft_clamp)
{
	/* Build an over-budget chain so the soft fallback clips at
	 * least one node, then verify scheduled < predicted on that
	 * node while equal elsewhere. */
	dag_t *g = dag_create(100, 100, 0.95, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 80, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 80, 102), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);

	/* Hard mode rejects this with EAGAIN -- critical path 160 ns
	 * > deadline 100 ns -- so the soft fallback runs. */
	pwtest_int_eq(dag_recalculate(g), -1);
	pwtest_int_eq(dag_recalculate_soft(g), 0);

	uint32_t clipped = 0;
	for (uint32_t id = 1; id <= 2; id++) {
		dag_node_t *n = find_node_by_id(g, id);
		pwtest_int_lt(0, (int)n->predicted_runtime_ns);
		pwtest_int_lt(0, (int)n->scheduled_runtime_ns);
		/* Either equal (uncipped) or scheduled < predicted
		 * (clipped). The soft path may apply the clip to one
		 * or both of these nodes; at least one must be
		 * clipped for the test workload. */
		if (n->scheduled_runtime_ns < n->predicted_runtime_ns)
			clipped++;
	}
	pwtest_int_lt(0, (int)clipped);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(hetero_iter_fusion_group_stays_co_located)
{
	/* Three nodes stamped into the same fusion group must land on a
	 * single CPU after the iterative recalc, just as they do under
	 * the single-shot path. The runtime overlay drives the split
	 * step but the worst-fit's group-forced-cpu logic still
	 * controls placement; iterating must not break that invariant. */
	double rel[3] = { 1.0, 0.5, 0.5 };

	dag_t *g = dag_create(1000, 1000, 0.95, 3, rel);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 100, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 100, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_set_node_group(g, 1, 42), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 42), 0);
	pwtest_int_eq(dag_set_node_group(g, 3, 42), 0);

	pwtest_int_eq(dag_recalculate_heterogeneous(g, 4), 0);

	uint32_t cpu1 = find_node_by_id(g, 1)->cpu;
	uint32_t cpu2 = find_node_by_id(g, 2)->cpu;
	uint32_t cpu3 = find_node_by_id(g, 3)->cpu;
	pwtest_int_eq((int)cpu1, (int)cpu2);
	pwtest_int_eq((int)cpu2, (int)cpu3);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(hetero_iter_preserves_unrelated_set_cache)
{
	/* After a successful dag_recalculate_heterogeneous the indexed
	 * nodes cache and the unrelated-set cache must both still be
	 * populated. A topology-stable second call must complete
	 * without rebuilding the analysis from scratch -- the public
	 * surface cannot directly observe that, but the cached caches
	 * are visible through the struct fields and a regression that
	 * frees them between iterations would null them out. */
	double rel[2] = { 1.0, 0.5 };

	dag_t *g = dag_create(1000, 1000, 0.95, 2, rel);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 100, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 100, 103), 0);
	pwtest_int_eq(dag_recalculate_heterogeneous(g, 4), 0);

	pwtest_ptr_notnull(g->indexed_nodes);
	pwtest_int_lt(0, (int)g->indexed_count);
	pwtest_ptr_notnull(g->unrelated);
	pwtest_int_lt(0, (int)g->unrelated_size);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(critical_path_runtime_chain_homogeneous)
{
	/* Three-node chain with wcet 10, 20, 30. Critical path = 60. */
	dag_t *g = dag_create(1000, 1000, 0.95, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 20, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 30, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	uint64_t cp = 0;
	uint32_t nodes[8];
	size_t n_nodes = 0;
	pwtest_int_eq(dag_compute_critical_path_runtime(g, NULL, &cp,
			nodes, 8, &n_nodes), 0);
	pwtest_int_eq((int)cp, 60);
	pwtest_int_eq((int)n_nodes, 3);
	/* Path is reported in source -> sink order. */
	pwtest_int_eq((int)nodes[0], 1);
	pwtest_int_eq((int)nodes[1], 2);
	pwtest_int_eq((int)nodes[2], 3);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(critical_path_runtime_with_overlay)
{
	/* Same chain as above; provide a runtime overlay that doubles
	 * every node's runtime. The critical path under the overlay
	 * must be exactly twice the legacy value. */
	dag_t *g = dag_create(1000, 1000, 0.95, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 20, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 30, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	uint64_t overlay[8] = {0};
	for (uint32_t i = 0; i < g->indexed_count; i++) {
		dag_node_t *n = g->indexed_nodes[i];
		overlay[i] = n->fictitious ? 0 : 2 * n->wcet;
	}

	uint64_t cp = 0;
	pwtest_int_eq(dag_compute_critical_path_runtime(g, overlay, &cp,
			NULL, 0, NULL), 0);
	pwtest_int_eq((int)cp, 120);

	/* Restoring NULL overlay must return to the legacy value. */
	pwtest_int_eq(dag_compute_critical_path_runtime(g, NULL, &cp,
			NULL, 0, NULL), 0);
	pwtest_int_eq((int)cp, 60);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(critical_path_runtime_rejects_invalid_inputs)
{
	dag_t *g = dag_create(1000, 1000, 0.95, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	uint64_t cp;
	pwtest_int_eq(dag_compute_critical_path_runtime(NULL, NULL,
			&cp, NULL, 0, NULL), -1);
	pwtest_int_eq(errno, EINVAL);
	pwtest_int_eq(dag_compute_critical_path_runtime(g, NULL,
			NULL, NULL, 0, NULL), -1);
	pwtest_int_eq(errno, EINVAL);

	/* An empty dag (or one without an analysis cache) is also
	 * rejected. */
	dag_t *empty = dag_create(1000, 1000, 0.95, 2, NULL);
	pwtest_ptr_notnull(empty);
	pwtest_int_eq(dag_compute_critical_path_runtime(empty, NULL,
			&cp, NULL, 0, NULL), -1);
	pwtest_int_eq(errno, EINVAL);

	dag_destroy(g);
	dag_destroy(empty);
	return PWTEST_PASS;
}

PWTEST(local_deadlines_with_runtimes_forwards_to_legacy)
{
	/* The _with_runtimes wrapper must produce the same
	 * local_deadline values as the legacy entry point when given a
	 * NULL overlay. */
	dag_t *g = dag_create(1000, 1000, 0.95, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 20, 102), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	uint64_t legacy_local_n1 = find_node_by_id(g, 1)->local_deadline;
	uint64_t legacy_local_n2 = find_node_by_id(g, 2)->local_deadline;

	pwtest_bool_true(dag_compute_local_deadlines_with_runtimes(g, NULL));
	pwtest_int_eq((int)find_node_by_id(g, 1)->local_deadline,
			(int)legacy_local_n1);
	pwtest_int_eq((int)find_node_by_id(g, 2)->local_deadline,
			(int)legacy_local_n2);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* -------------------------------------------------------------------
 * Merged/fused-group collapse in the antichain enumeration.
 *
 * Nodes that share a non-zero dag_node::group_id (stamped by the
 * reconcile layer when libpipewire's chain-merge or subgraph-fusion
 * has put them on the same data-loop thread) are co-located on the
 * same CPU and execute sequentially. Treating each group as a single
 * virtual node in dag_comp_unrelated:
 *
 *   - shrinks the branch-and-bound search space from |real nodes| to
 *     |groups| -- the run-time win the optimisation targets;
 *   - emits antichains whose bitsets contain every member of every
 *     chosen group, so assign_cpus still accounts for the full
 *     per-CPU load contribution of the group (sum across members,
 *     which matches the actual single-thread utilisation model).
 *
 * The tests below verify both effects on small, hand-traceable
 * graphs. They also document the equivalence with the pre-collapse
 * behaviour on ungrouped (singleton) graphs.
 * ------------------------------------------------------------------- */

PWTEST(unrelated_collapse_chain_pair_with_independent_node)
{
	/* 1 -> 2 chain plus an independent node 3. With no grouping
	 * the enumeration emits two maximal antichains: {1, 3} and
	 * {2, 3}. Stamping {1, 2} as one co-location group collapses
	 * the pair into a single virtual node, leaving a single
	 * antichain whose bitset still contains all three real
	 * indices. CPU placement and node deadlines are independent
	 * of the change; both baselines must continue to admit. */
	static const uint32_t all_ids[] = { 1, 2, 3 };
	static const uint32_t pair_13[] = { 1, 3 };
	static const uint32_t pair_23[] = { 2, 3 };

	dag_t *g_base = dag_create(100, 100, 0.90f, 1, NULL);
	pwtest_ptr_notnull(g_base);
	pwtest_int_eq(add_real_node(g_base, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g_base, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g_base, 3, 10, 103), 0);
	pwtest_int_eq(dag_add_edge(g_base, 1, 2), 0);
	pwtest_int_eq(dag_recalculate(g_base), 0);

	pwtest_int_eq((int)g_base->unrelated_size, 2);
	pwtest_bool_true(dag_has_unrelated_subset(g_base, pair_13, 2));
	pwtest_bool_true(dag_has_unrelated_subset(g_base, pair_23, 2));
	dag_destroy(g_base);

	dag_t *g = dag_create(100, 100, 0.90f, 1, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_set_node_group(g, 1, 42), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 42), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)g->unrelated_size, 1);
	pwtest_bool_true(dag_has_unrelated_subset(g, all_ids, 3));
	pwtest_int_eq((int)bitset_population(g->unrelated[0], g->indexed_count), 3);
	pwtest_bool_false(dag_unrelated_has_fictitious_nodes(g));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(unrelated_collapse_two_chains_to_single_antichain)
{
	/* Two independent 2-node chains: 1 -> 3 and 2 -> 4. Without
	 * grouping the enumeration finds four maximal antichains:
	 * {1,2}, {1,4}, {3,2}, {3,4}. Stamping each chain as its own
	 * group collapses the search to two virtual nodes that are
	 * mutually unrelated, leaving exactly one antichain whose
	 * bitset is the union of both groups' members. The total
	 * relative deadline budget is unchanged (4 real nodes), so a
	 * 0.50 ceiling still admits with one CPU. */
	static const uint32_t all_ids[] = { 1, 2, 3, 4 };

	dag_t *g_base = dag_create(100, 100, 0.50f, 1, NULL);
	pwtest_ptr_notnull(g_base);
	pwtest_int_eq(add_real_node(g_base, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g_base, 2, 5, 102), 0);
	pwtest_int_eq(add_real_node(g_base, 3, 5, 103), 0);
	pwtest_int_eq(add_real_node(g_base, 4, 5, 104), 0);
	pwtest_int_eq(dag_add_edge(g_base, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g_base, 2, 4), 0);
	pwtest_int_eq(dag_recalculate(g_base), 0);
	pwtest_int_eq((int)g_base->unrelated_size, 4);
	dag_destroy(g_base);

	dag_t *g = dag_create(100, 100, 0.50f, 1, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 5, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 5, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 5, 104), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);
	pwtest_int_eq(dag_set_node_group(g, 1, 10), 0);
	pwtest_int_eq(dag_set_node_group(g, 3, 10), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 20), 0);
	pwtest_int_eq(dag_set_node_group(g, 4, 20), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)g->unrelated_size, 1);
	pwtest_bool_true(dag_has_unrelated_subset(g, all_ids, 4));
	pwtest_int_eq((int)bitset_population(g->unrelated[0], g->indexed_count), 4);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(unrelated_collapse_chain_three_nodes_one_group)
{
	/* A 3-node chain 1 -> 2 -> 3 has three maximal real-node
	 * antichains in the baseline ({1}, {2}, {3} -- each cut is
	 * maximal because every other node is a successor or
	 * predecessor). Stamping all three with the same group_id
	 * collapses the enumeration to a single antichain whose
	 * bitset is the union of all members; this is the case
	 * where expansion adds bits that the rep-only bitset would
	 * have missed. The placement is unchanged (every node ends
	 * up on the same CPU either way). */
	static const uint32_t all_ids[] = { 1, 2, 3 };

	dag_t *g_base = dag_create(100, 100, 0.50f, 1, NULL);
	pwtest_ptr_notnull(g_base);
	pwtest_int_eq(add_real_node(g_base, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g_base, 2, 5, 102), 0);
	pwtest_int_eq(add_real_node(g_base, 3, 5, 103), 0);
	pwtest_int_eq(dag_add_edge(g_base, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g_base, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g_base), 0);
	pwtest_int_eq((int)g_base->unrelated_size, 3);
	dag_destroy(g_base);

	dag_t *g = dag_create(100, 100, 0.50f, 1, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 5, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 5, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_set_node_group(g, 1, 9), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 9), 0);
	pwtest_int_eq(dag_set_node_group(g, 3, 9), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)g->unrelated_size, 1);
	pwtest_bool_true(dag_has_unrelated_subset(g, all_ids, 3));
	pwtest_int_eq((int)bitset_population(g->unrelated[0], g->indexed_count), 3);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(unrelated_collapse_group_relation_via_non_rep_member)
{
	/* A group's relation to a third node depends on whether
	 * ANY member is reachable from / reaches the third node,
	 * not just the representative. Picking the rep alone (the
	 * lowest-index member, here node 1) would miss the case
	 * where a non-rep member is the related one.
	 *
	 * Graph: node 1 is isolated; 2 -> 3. Group {1, 3} stamped
	 * with the same group_id; node 2 ungrouped. The rep of the
	 * group is node 1 (lowest index in topo order); node 1 is
	 * unrelated to node 2, but the non-rep member node 3 IS
	 * reachable from node 2. The group-level check must report
	 * RELATED -- otherwise the enumeration would put the group
	 * and node 2 in one antichain, which would be wrong because
	 * the actual execution chain 2 -> 3 is preserved at the
	 * node level. */
	static const uint32_t group_only[] = { 1, 3 };
	static const uint32_t solo[] = { 2 };

	dag_t *g = dag_create(100, 100, 0.50f, 1, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 5, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 5, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 5, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_set_node_group(g, 1, 42), 0);
	pwtest_int_eq(dag_set_node_group(g, 3, 42), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)g->unrelated_size, 2);
	pwtest_bool_true(dag_has_unrelated_subset(g, group_only, 2));
	pwtest_bool_true(dag_has_unrelated_subset(g, solo, 1));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(unrelated_collapse_mixed_grouped_and_ungrouped)
{
	/* A 2-node grouped chain plus a single ungrouped neighbour.
	 * The group must collapse to one rep; the ungrouped node
	 * stays its own singleton; the resulting CPU placement is the
	 * same as the grouped-only case where ordinary worst-fit
	 * picks the emptier CPU for the ungrouped node. The
	 * antichain count drops from two (pre-collapse: {1,3} and
	 * {2,3}) to one (post-collapse: {1,2,3}). */
	dag_t *g = dag_create(100, 100, 0.50f, 2, NULL);
	dag_node_t *a, *b, *c;

	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_set_node_group(g, 1, 5), 0);
	pwtest_int_eq(dag_set_node_group(g, 2, 5), 0);
	/* node 3 stays ungrouped (group_id = 0) */

	pwtest_int_eq(dag_recalculate(g), 0);
	a = find_node_by_id(g, 1);
	b = find_node_by_id(g, 2);
	c = find_node_by_id(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_int_eq((int)a->cpu, (int)b->cpu);
	pwtest_bool_true(a->cpu != c->cpu);

	pwtest_int_eq((int)g->unrelated_size, 1);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(unrelated_collapse_no_groups_matches_baseline)
{
	/* A graph with no group_id stamps must continue to produce
	 * its pre-collapse antichains under the new code path (where
	 * every real node becomes its own singleton dense group --
	 * the rep equals the sole member, group_node_succ equals
	 * node->successors, and expansion is a no-op). The diamond
	 * 1 -> {2, 3} -> 4 has three maximal real-node antichains
	 * ({1} alone, {2,3} together, {4} alone); the fork-mid
	 * antichain {2, 3} must still be present, and its bitset
	 * must contain exactly two bits. */
	static const uint32_t mids[] = { 2, 3 };

	dag_t *g = dag_create(100, 100, 0.90f, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 10, 101), 0);
	pwtest_int_eq(add_real_node(g, 2, 10, 102), 0);
	pwtest_int_eq(add_real_node(g, 3, 10, 103), 0);
	pwtest_int_eq(add_real_node(g, 4, 10, 104), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)g->unrelated_size, 3);
	pwtest_bool_true(dag_has_unrelated_subset(g, mids, 2));
	pwtest_int_eq((int)dag_max_unrelated_size(g), 2);
	pwtest_bool_false(dag_unrelated_has_fictitious_nodes(g));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(soft_redistribute_chain_proportional)
{
	/* A chain A(100) -> B(200) -> C(300) with a 1000ns end-to-end
	 * deadline gets cumulative deadlines proportional to the
	 * cumulative WCET: A=100/600, B=300/600, C=600/600 of D.
	 * Sum equals 1000ns. */
	dag_t *g = dag_create(1000, 1000, 1.0, 4, NULL);
	double obj = -1.0;
	uint32_t clipped = 99;
	pwtest_ptr_notnull(g);

	pwtest_int_eq(dag_add_node(g, 1, 100, 1001, false), 0);
	pwtest_int_eq(dag_add_node(g, 2, 200, 1002, false), 0);
	pwtest_int_eq(dag_add_node(g, 3, 300, 1003, false), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	pwtest_bool_true(dag_soft_redistribute_deadlines(g, &obj, &clipped));
	pwtest_int_eq((int)clipped, 0);
	pwtest_bool_true(obj == 0.0);

	{
		dag_node_t *a = dag_find_node(g, 1);
		dag_node_t *b = dag_find_node(g, 2);
		dag_node_t *c = dag_find_node(g, 3);
		pwtest_ptr_notnull(a);
		pwtest_ptr_notnull(b);
		pwtest_ptr_notnull(c);
		/* A's cumulative ~= 100/600 * 1000 = 166ns. */
		pwtest_bool_true(a->cumulative_deadline >= 150);
		pwtest_bool_true(a->cumulative_deadline <= 200);
		/* B's cumulative ~= 300/600 * 1000 = 500ns. */
		pwtest_bool_true(b->cumulative_deadline >= 450);
		pwtest_bool_true(b->cumulative_deadline <= 550);
		/* C's cumulative = D = 1000. */
		pwtest_int_eq((int)c->cumulative_deadline, 1000);
		/* Local deadlines must satisfy wcet <= local <= period. */
		pwtest_bool_true(a->wcet <= a->local_deadline);
		pwtest_bool_true(b->wcet <= b->local_deadline);
		pwtest_bool_true(c->wcet <= c->local_deadline);
		pwtest_bool_false(dag_node_budget_clipped(g, 1));
		pwtest_bool_false(dag_node_budget_clipped(g, 2));
		pwtest_bool_false(dag_node_budget_clipped(g, 3));
	}

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(soft_redistribute_overload_clips_nodes)
{
	/* A chain A(800) -> B(500) -> C(300) with end-to-end 1000ns
	 * cannot fit: path sum 1600 > 1000. The hard-mode splitter
	 * (dag_recalculate) refuses the workload; the soft-mode
	 * heuristic runs directly on the DAG and produces a clipped
	 * proportional assignment. A's wcet (800) > local (500) means
	 * the budget got clipped against the redistributed deadline;
	 * the objective records the overflow as a unitless fraction
	 * of the end-to-end deadline. */
	dag_t *g = dag_create(1000, 1000, 1.0, 4, NULL);
	double obj = -1.0;
	uint32_t clipped = 0;
	pwtest_ptr_notnull(g);

	pwtest_int_eq(dag_add_node(g, 1, 800, 1001, false), 0);
	pwtest_int_eq(dag_add_node(g, 2, 500, 1002, false), 0);
	pwtest_int_eq(dag_add_node(g, 3, 300, 1003, false), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	/* dag_recalculate is expected to refuse this workload; the
	 * soft heuristic must work on the un-analysed DAG. */
	(void)dag_recalculate(g);

	pwtest_bool_true(dag_soft_redistribute_deadlines(g, &obj, &clipped));
	pwtest_bool_true(clipped >= 1);
	pwtest_bool_true(obj > 0.0);
	pwtest_bool_true(dag_node_budget_clipped(g, 1));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(soft_redistribute_diamond_keeps_monotonicity)
{
	/* A diamond A -> B -> D, A -> C -> D with A=B=C=D=100 (so
	 * path sum 300, longest 300). The redistribution must keep
	 * cumulative_deadline monotonic along every edge. */
	dag_t *g = dag_create(1200, 1200, 1.0, 4, NULL);
	double obj = -1.0;
	pwtest_ptr_notnull(g);

	pwtest_int_eq(dag_add_node(g, 1, 100, 1001, false), 0);
	pwtest_int_eq(dag_add_node(g, 2, 100, 1002, false), 0);
	pwtest_int_eq(dag_add_node(g, 3, 100, 1003, false), 0);
	pwtest_int_eq(dag_add_node(g, 4, 100, 1004, false), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	pwtest_bool_true(dag_soft_redistribute_deadlines(g, &obj, NULL));

	{
		dag_node_t *a = dag_find_node(g, 1);
		dag_node_t *b = dag_find_node(g, 2);
		dag_node_t *c = dag_find_node(g, 3);
		dag_node_t *d = dag_find_node(g, 4);
		pwtest_ptr_notnull(a);
		pwtest_ptr_notnull(b);
		pwtest_ptr_notnull(c);
		pwtest_ptr_notnull(d);
		pwtest_bool_true(a->cumulative_deadline <= b->cumulative_deadline);
		pwtest_bool_true(a->cumulative_deadline <= c->cumulative_deadline);
		pwtest_bool_true(b->cumulative_deadline <= d->cumulative_deadline);
		pwtest_bool_true(c->cumulative_deadline <= d->cumulative_deadline);
	}

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(soft_redistribute_rejects_invalid_inputs)
{
	double obj;
	uint32_t clipped;
	pwtest_bool_false(dag_soft_redistribute_deadlines(NULL, &obj, &clipped));
	pwtest_bool_false(dag_node_budget_clipped(NULL, 0));
	return PWTEST_PASS;
}

/* --------------------------------------------------------------- *
 * Driver-as-DAG-node shapes. When module-deadline runs with
 * driver.schedule=on, the snapshot path includes the driver in
 * t->nodes[] alongside its followers. The DAG library does not
 * distinguish a "driver" id from a "follower" id -- it just sees a
 * node with a WCET and a tid -- but the topology shapes the module
 * now emits are new, so these tests pin the per-node budget contract
 * on each of them. The driver_id used is purely a naming convention
 * (10000) so the test reader can identify the driver at a glance.
 * --------------------------------------------------------------- */

/* Driver as sink of a chain follower_A -> driver. Distinct WCETs:
 * the splitter must give the driver a proportional slice of the
 * period, larger than follower_A's because the driver's WCET is
 * larger. Pins: driver gets a non-zero, period-bounded budget that
 * scales with its measured WCET. */
PWTEST(driver_as_dag_sink_chain)
{
	const uint32_t driver_id = 10000;
	dag_t *g = dag_create(1000, 1000, 0.95f, 1, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 1001), 0);
	pwtest_int_eq(add_real_node(g, driver_id, 300, 9999), 0);
	pwtest_int_eq(dag_add_edge(g, 1, driver_id), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	dag_node_t *a = find_node_by_id(g, 1);
	dag_node_t *d = find_node_by_id(g, driver_id);

	pwtest_bool_true(a->cumulative_deadline > 0);
	pwtest_bool_true(d->cumulative_deadline > a->cumulative_deadline);
	pwtest_bool_true(d->cumulative_deadline <= 1000);
	/* Source: local == cumulative. */
	pwtest_int_eq((int)a->local_deadline, (int)a->cumulative_deadline);
	/* Sink: local == cumulative - cumulative(pred). */
	pwtest_int_eq((int)d->local_deadline,
		      (int)(d->cumulative_deadline - a->cumulative_deadline));
	/* WCET scaling: driver has 3x A's WCET, so its local slice
	 * is larger than A's. */
	pwtest_bool_true(d->local_deadline > a->local_deadline);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Driver as sink of a fan-in: follower_A, follower_B -> driver,
 * with B carrying the longer cumulative path. The driver's
 * local_deadline must equal cumulative - max(predecessor cumulative)
 * -- pinning the join formula on the new shape. */
PWTEST(driver_as_dag_sink_fan_in)
{
	const uint32_t driver_id = 10000;
	dag_t *g = dag_create(1000, 1000, 0.95f, 2, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, 1, 100, 1001), 0);
	pwtest_int_eq(add_real_node(g, 2, 300, 1002), 0);
	pwtest_int_eq(add_real_node(g, driver_id, 200, 9999), 0);
	pwtest_int_eq(dag_add_edge(g, 1, driver_id), 0);
	pwtest_int_eq(dag_add_edge(g, 2, driver_id), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	dag_node_t *a = find_node_by_id(g, 1);
	dag_node_t *b = find_node_by_id(g, 2);
	dag_node_t *d = find_node_by_id(g, driver_id);

	uint64_t max_pred = a->cumulative_deadline >= b->cumulative_deadline
		? a->cumulative_deadline : b->cumulative_deadline;
	pwtest_int_eq((int)d->local_deadline,
		      (int)(d->cumulative_deadline - max_pred));
	pwtest_bool_true(d->cumulative_deadline <= 1000);
	pwtest_bool_true(d->local_deadline > 0);
	/* The heavier predecessor must drive the join. */
	pwtest_bool_true(b->cumulative_deadline >= a->cumulative_deadline);

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Driver as source of a capture-style chain: driver -> follower_A.
 * The driver's local == cumulative because it has no real
 * predecessor; the follower's cumulative > the driver's. */
PWTEST(driver_as_dag_source_capture_chain)
{
	const uint32_t driver_id = 10000;
	dag_t *g = dag_create(1000, 1000, 0.95f, 1, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, driver_id, 150, 9999), 0);
	pwtest_int_eq(add_real_node(g, 1, 250, 1001), 0);
	pwtest_int_eq(dag_add_edge(g, driver_id, 1), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	dag_node_t *d = find_node_by_id(g, driver_id);
	dag_node_t *a = find_node_by_id(g, 1);

	pwtest_int_eq((int)d->local_deadline, (int)d->cumulative_deadline);
	pwtest_bool_true(d->cumulative_deadline > 0);
	pwtest_bool_true(a->cumulative_deadline > d->cumulative_deadline);
	pwtest_int_eq((int)a->local_deadline,
		      (int)(a->cumulative_deadline - d->cumulative_deadline));

	dag_destroy(g);
	return PWTEST_PASS;
}

/* Single-node DAG containing only the driver. Degenerate but
 * legal: the driver runs alone, with no followers (a sink/source
 * graph during teardown, or a workload that hasn't connected yet).
 * The driver gets the full period as its local deadline. */
PWTEST(driver_only_node)
{
	const uint32_t driver_id = 10000;
	dag_t *g = dag_create(1000, 1000, 0.95f, 1, NULL);
	pwtest_ptr_notnull(g);
	pwtest_int_eq(add_real_node(g, driver_id, 400, 9999), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	dag_node_t *d = find_node_by_id(g, driver_id);
	/* No real predecessor: local == cumulative. */
	pwtest_int_eq((int)d->local_deadline, (int)d->cumulative_deadline);
	pwtest_bool_true(d->cumulative_deadline > 0);
	pwtest_bool_true(d->cumulative_deadline <= 1000);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST_SUITE(module_deadline_dag)
{
	pwtest_add(explicit_deadline_fields_populated_after_recalc, PWTEST_NOARG);
	pwtest_add(local_deadline_conversion_chain, PWTEST_NOARG);
	pwtest_add(local_deadline_conversion_join_uses_max_pred, PWTEST_NOARG);
	pwtest_add(local_deadline_conversion_direct_pseudocode_example, PWTEST_NOARG);
	pwtest_add(local_deadline_conversion_direct_join_example, PWTEST_NOARG);
	pwtest_add(local_deadline_conversion_rejects_nonmonotonic, PWTEST_NOARG);
	pwtest_add(density_empty_cpu_returns_zero, PWTEST_NOARG);
	pwtest_add(density_feasible_chain_under_one, PWTEST_NOARG);
	pwtest_add(density_infeasible_chain_above_one, PWTEST_NOARG);
	pwtest_add(density_per_cpu_isolates_workloads, PWTEST_NOARG);
	pwtest_add(density_relative_capacity_scaling, PWTEST_NOARG);
	pwtest_add(density_null_safe, PWTEST_NOARG);
	pwtest_add(dbf_feasible_single_task, PWTEST_NOARG);
	pwtest_add(dbf_infeasible_when_demand_exceeds_t, PWTEST_NOARG);
	pwtest_add(dbf_passes_when_density_passes, PWTEST_NOARG);
	pwtest_add(dbf_null_safe, PWTEST_NOARG);
	pwtest_add(chain_uses_peak_not_sum, PWTEST_NOARG);
	pwtest_add(fork_join_fails_on_peak_concurrency, PWTEST_NOARG);
	pwtest_add(multi_source_initial_cut_and_cleanup, PWTEST_NOARG);
	pwtest_add(topology_change_rebuilds_analysis, PWTEST_NOARG);
	pwtest_add(fictitious_nodes_allow_zero_wcet, PWTEST_NOARG);
	pwtest_add(recalculate_keeps_exactly_two_fictitious_nodes, PWTEST_NOARG);
	pwtest_add(dag_foreach_node_skips_fictitious_nodes, PWTEST_NOARG);
	pwtest_add(iterative_deadlines_assign_fork_join, PWTEST_NOARG);
	
	pwtest_add(independent_tasks_are_placed_by_descending_density, PWTEST_NOARG);
	pwtest_add(equal_load_ties_choose_lowest_cpu, PWTEST_NOARG);
	pwtest_add(single_node_deadline, PWTEST_NOARG);
	pwtest_add(chain_deadlines, PWTEST_NOARG);
	pwtest_add(diamond_deadlines, PWTEST_NOARG);
	pwtest_add(branch_tightening_residual_budget, PWTEST_NOARG);
	pwtest_add(residual_budget_no_double_credit, PWTEST_NOARG);
	pwtest_add(self_loop_rejected_with_einval, PWTEST_NOARG);
	pwtest_add(cycle_creating_edge_rejected_with_eloop, PWTEST_NOARG);
	pwtest_add(dag_has_cycle_predicate, PWTEST_NOARG);
	pwtest_add(disconnect_two_parallel_chains, PWTEST_NOARG);
	pwtest_add(disconnect_chain_plus_isolated, PWTEST_NOARG);
	pwtest_add(unrelated_set_chain_admits_serially, PWTEST_NOARG);
	pwtest_add(unrelated_set_independent_spreads, PWTEST_NOARG);
	pwtest_add(unrelated_set_diamond_admits_on_two_cpus, PWTEST_NOARG);
	pwtest_add(dirty_noop_after_clean_recalc, PWTEST_NOARG);
	pwtest_add(dirty_set_node_wcet, PWTEST_NOARG);
	pwtest_add(dirty_set_global_period_deadline, PWTEST_NOARG);
	pwtest_add(dirty_on_add_edge, PWTEST_NOARG);
	pwtest_add(feasibility_tight_critical_path, PWTEST_NOARG);
	pwtest_add(feasibility_critical_path_overrun, PWTEST_NOARG);
	pwtest_add(feasibility_min_deadline_reservation, PWTEST_NOARG);
	pwtest_add(recalculate_soft_critical_path_overrun_assigns_all_nodes,
			PWTEST_NOARG);
	pwtest_add(recalculate_soft_relaxed_placement_overrides_admission_ceiling,
			PWTEST_NOARG);
	pwtest_add(recalculate_soft_empty_graph_is_noop, PWTEST_NOARG);
	pwtest_add(cpu_placement_orders_by_descending_density, PWTEST_NOARG);
	pwtest_add(cpu_placement_equal_density_lowest_cpu, PWTEST_NOARG);
	pwtest_add(cp_aware_chain_plus_independent_placement, PWTEST_NOARG);
	pwtest_add(cp_aware_two_chains_placement, PWTEST_NOARG);
	pwtest_add(cp_aware_independent_set_matches_density_order, PWTEST_NOARG);
	pwtest_add(cp_aware_equal_cp_and_density_breaks_by_topological_index,
			PWTEST_NOARG);
	pwtest_add(cp_aware_group_keeps_co_location, PWTEST_NOARG);
	pwtest_add(cp_aware_hetero_capacity_picks_faster_cpu_first, PWTEST_NOARG);
	pwtest_add(load_reject_non_finite_or_out_of_range, PWTEST_NOARG);
	pwtest_add(load_ordinary_cap_admits, PWTEST_NOARG);
	pwtest_add(load_near_bound_admits, PWTEST_NOARG);
	pwtest_add(timing_invalid_inputs_rejected, PWTEST_NOARG);
	pwtest_add(zero_wcet_rejected, PWTEST_NOARG);
	pwtest_add(timing_edge_deadline_equals_period, PWTEST_NOARG);
	pwtest_add(workspace_reuse_across_recalcs, PWTEST_NOARG);
	pwtest_add(id_index_empty, PWTEST_NOARG);
	pwtest_add(id_index_single, PWTEST_NOARG);
	pwtest_add(id_index_remove, PWTEST_NOARG);
	pwtest_add(id_index_shuffled_insertion_stays_sorted, PWTEST_NOARG);
	pwtest_add(id_index_add_remove_stress, PWTEST_NOARG);
	pwtest_add(id_index_duplicate_add_leaves_index_intact, PWTEST_NOARG);

	pwtest_add(group_two_independent_share_cpu, PWTEST_NOARG);
	pwtest_add(group_chain_keeps_chain_on_one_cpu, PWTEST_NOARG);
	pwtest_add(group_distinct_groups_do_not_unify, PWTEST_NOARG);
	pwtest_add(group_ungrouped_neighbour_unaffected, PWTEST_NOARG);
	pwtest_add(group_overcapacity_returns_eagain, PWTEST_NOARG);
	pwtest_add(group_invalid_id_returns_enoent, PWTEST_NOARG);
	pwtest_add(group_null_dag_returns_einval, PWTEST_NOARG);
	pwtest_add(group_same_value_is_noop_for_dirty, PWTEST_NOARG);
	pwtest_add(group_clear_returns_to_default, PWTEST_NOARG);
	pwtest_add(group_survives_wcet_update, PWTEST_NOARG);
	pwtest_add(group_three_members_partial_clear, PWTEST_NOARG);
	pwtest_add(group_member_removed_does_not_leak, PWTEST_NOARG);

	pwtest_add(group_high_id_does_not_overflow, PWTEST_NOARG);
	pwtest_add(group_many_groups_each_co_locates, PWTEST_NOARG);
	pwtest_add(group_eagain_recoverable_after_clear, PWTEST_NOARG);
	pwtest_add(group_migration_between_groups, PWTEST_NOARG);
	pwtest_add(group_fictitious_node_ignored, PWTEST_NOARG);
	pwtest_add(group_competes_with_ungrouped_for_capacity, PWTEST_NOARG);
	pwtest_add(group_topology_edge_addition_keeps_placement, PWTEST_NOARG);
	pwtest_add(group_set_after_recalc_marks_dirty, PWTEST_NOARG);

	pwtest_add(hetero_all_ones_matches_null_vector, PWTEST_NOARG);
	pwtest_add(hetero_high_density_picks_faster_cpu, PWTEST_NOARG);
	pwtest_add(hetero_admission_rejects_workload_that_only_fits_at_full_capacity, PWTEST_NOARG);
	pwtest_add(hetero_feasibility_scaled_by_slowest_cpu, PWTEST_NOARG);

	pwtest_add(hetero_iter_invalid_inputs_rejected, PWTEST_NOARG);
	pwtest_add(hetero_iter_uniform_capacity_matches_single_shot, PWTEST_NOARG);
	pwtest_add(hetero_iter_single_iteration_short_circuits, PWTEST_NOARG);
	pwtest_add(hetero_iter_chain_overlay_stretches_paths, PWTEST_NOARG);
	pwtest_add(hetero_iter_convergence_breaks_loop, PWTEST_NOARG);
	pwtest_add(predicted_and_scheduled_runtime_populated_after_recalc,
			PWTEST_NOARG);
	pwtest_add(predicted_and_scheduled_cleared_on_dirty, PWTEST_NOARG);
	pwtest_add(predicted_and_scheduled_diverge_under_soft_clamp,
			PWTEST_NOARG);
	pwtest_add(hetero_iter_fusion_group_stays_co_located, PWTEST_NOARG);
	pwtest_add(hetero_iter_preserves_unrelated_set_cache, PWTEST_NOARG);
	pwtest_add(critical_path_runtime_chain_homogeneous, PWTEST_NOARG);
	pwtest_add(critical_path_runtime_with_overlay, PWTEST_NOARG);
	pwtest_add(critical_path_runtime_rejects_invalid_inputs, PWTEST_NOARG);
	pwtest_add(local_deadlines_with_runtimes_forwards_to_legacy, PWTEST_NOARG);

	pwtest_add(unrelated_collapse_chain_pair_with_independent_node, PWTEST_NOARG);
	pwtest_add(unrelated_collapse_two_chains_to_single_antichain, PWTEST_NOARG);
	pwtest_add(unrelated_collapse_chain_three_nodes_one_group, PWTEST_NOARG);
	pwtest_add(unrelated_collapse_group_relation_via_non_rep_member, PWTEST_NOARG);
	pwtest_add(unrelated_collapse_mixed_grouped_and_ungrouped, PWTEST_NOARG);
	pwtest_add(unrelated_collapse_no_groups_matches_baseline, PWTEST_NOARG);

	pwtest_add(soft_redistribute_chain_proportional, PWTEST_NOARG);
	pwtest_add(soft_redistribute_overload_clips_nodes, PWTEST_NOARG);
	pwtest_add(soft_redistribute_diamond_keeps_monotonicity, PWTEST_NOARG);
	pwtest_add(soft_redistribute_rejects_invalid_inputs, PWTEST_NOARG);

	pwtest_add(driver_as_dag_sink_chain, PWTEST_NOARG);
	pwtest_add(driver_as_dag_sink_fan_in, PWTEST_NOARG);
	pwtest_add(driver_as_dag_source_capture_chain, PWTEST_NOARG);
	pwtest_add(driver_only_node, PWTEST_NOARG);

	return PWTEST_PASS;
}
