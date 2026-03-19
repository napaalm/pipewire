/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 PipeWire contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#include "pwtest.h"

#include "module-deadline/dag.h"

#define TEST_LOAD_EPSILON (64.0 * DBL_EPSILON)

static dag_node_t *find_node(dag_t *g, uint32_t id)
{
	dag_node_t *node;

	spa_list_for_each(node, &g->nodes, link) {
		if (node->id == id)
			return node;
	}

	return NULL;
}

static uint64_t path_deadline_sum(dag_t *g, const uint32_t *ids, size_t n_ids)
{
	uint64_t sum = 0;

	for (size_t i = 0; i < n_ids; i++) {
		dag_node_t *node = find_node(g, ids[i]);

		pwtest_ptr_notnull(node);
		sum += node->deadline;
	}

	return sum;
}

static bool double_eq_eps(double a, double b)
{
	return fabs(a - b) <= TEST_LOAD_EPSILON;
}

static bool double_leq_eps(double a, double b)
{
	return a <= b + TEST_LOAD_EPSILON;
}

static bool double_gt_eps(double a, double b)
{
	return a > b + TEST_LOAD_EPSILON;
}

static dag_t *create_test_dag_with_util(uint64_t deadline, double utilization,
		uint32_t num_cpus)
{
	dag_t *g = dag_create(deadline, deadline, utilization, num_cpus);

	pwtest_ptr_notnull(g);
	return g;
}

static dag_t *create_test_dag(uint64_t deadline, uint32_t num_cpus)
{
	return create_test_dag_with_util(deadline, 1.0, num_cpus);
}

static double node_density(dag_t *g, dag_node_t *node)
{
	uint64_t denom = node->deadline < g->period ? node->deadline : g->period;

	pwtest_bool_true(node->deadline_assigned);
	pwtest_bool_true(denom > 0);
	return (double) node->wcet / (double) denom;
}

static double cpu_raw_density_sum(dag_t *g, uint32_t cpu)
{
	double sum = 0.0;
	dag_node_t *node;

	spa_list_for_each(node, &g->nodes, link) {
		if (node->cpu == cpu)
			sum += node_density(g, node);
	}

	return sum;
}

static bool node_reaches_recursive(dag_node_t *src, dag_node_t *dst)
{
	dag_edge_t *edge;

	if (src == dst)
		return true;

	spa_list_for_each(edge, &src->outgoing, src_link) {
		if (node_reaches_recursive(edge->dst, dst))
			return true;
	}

	return false;
}

static double cpu_exact_unrelated_load(dag_t *g, uint32_t cpu)
{
	dag_node_t *selected[64];
	size_t count = 0;
	double best = 0.0;
	uint64_t subsets;
	dag_node_t *node;

	spa_list_for_each(node, &g->nodes, link) {
		if (node->cpu == cpu) {
			pwtest_bool_true(count < SPA_N_ELEMENTS(selected));
			selected[count++] = node;
		}
	}

	subsets = 1ULL << count;

	for (uint64_t mask = 1; mask < subsets; mask++) {
		bool unrelated = true;
		double load = 0.0;

		for (size_t i = 0; i < count && unrelated; i++) {
			if ((mask & (1ULL << i)) == 0)
				continue;

			load += node_density(g, selected[i]);

			for (size_t j = i + 1; j < count; j++) {
				if ((mask & (1ULL << j)) == 0)
					continue;
				if (node_reaches_recursive(selected[i], selected[j]) ||
						node_reaches_recursive(selected[j], selected[i])) {
					unrelated = false;
					break;
				}
			}
		}

		if (unrelated && load > best)
			best = load;
	}

	return best;
}

static void assert_all_on_cpu(dag_t *g, uint32_t cpu)
{
	dag_node_t *node;

	spa_list_for_each(node, &g->nodes, link)
		pwtest_int_eq((int) node->cpu, (int) cpu);
}

static void force_add_edge(dag_t *g, uint32_t src_id, uint32_t dst_id)
{
	dag_node_t *src = find_node(g, src_id);
	dag_node_t *dst = find_node(g, dst_id);
	dag_edge_t *edge;

	pwtest_ptr_notnull(src);
	pwtest_ptr_notnull(dst);

	edge = calloc(1, sizeof(*edge));
	pwtest_ptr_notnull(edge);

	edge->src = src;
	edge->dst = dst;
	spa_list_append(&g->edges, &edge->link);
	spa_list_append(&src->outgoing, &edge->src_link);
	spa_list_append(&dst->incoming, &edge->dst_link);
}

static void assert_assignments_cleared(dag_t *g)
{
	dag_node_t *node;

	spa_list_for_each(node, &g->nodes, link) {
		pwtest_bool_false(node->deadline_assigned);
		pwtest_int_eq((int)node->deadline, 0);
		pwtest_int_eq((int) node->cpu, (int) DAG_CPU_INVALID);
	}
}

struct foreach_stats {
	uint32_t count;
	uint64_t deadline_sum;
};

static void collect_foreach_stats(void *data, pid_t tid, uint64_t wcet,
		uint64_t deadline, uint64_t period, uint32_t cpu)
{
	struct foreach_stats *stats = data;

	(void) tid;
	(void) wcet;
	(void) period;
	(void) cpu;

	stats->count++;
	stats->deadline_sum += deadline;
}

PWTEST(single_node_deadline)
{
	dag_t *g = create_test_dag(25, 1);
	dag_node_t *node;

	pwtest_int_eq(dag_add_node(g, 1, 7, 1), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	node = find_node(g, 1);
	pwtest_ptr_notnull(node);
	pwtest_bool_true(node->deadline_assigned);
	pwtest_int_eq((int)node->deadline, 25);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(self_loop_rejected)
{
	dag_t *g = create_test_dag(10, 1);
	dag_node_t *node;

	pwtest_int_eq(dag_add_node(g, 1, 4, 1), 0);
	pwtest_errno(dag_add_edge(g, 1, 1), ELOOP);
	pwtest_int_eq(dag_recalculate(g), 0);

	node = find_node(g, 1);
	pwtest_ptr_notnull(node);
	pwtest_bool_true(node->deadline_assigned);
	pwtest_int_eq((int)node->deadline, 10);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(cycle_edge_rejected)
{
	static const uint32_t path[] = { 1, 2, 3 };
	dag_t *g = create_test_dag(15, 3);

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 1, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_errno(dag_add_edge(g, 3, 1), ELOOP);
	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_bool_true(path_deadline_sum(g, path, SPA_N_ELEMENTS(path)) <= g->deadline);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(chain_deadlines)
{
	static const uint32_t path[] = { 1, 2, 3 };
	dag_t *g = create_test_dag(18, 3);
	dag_node_t *a, *b, *c;

	pwtest_int_eq(dag_add_node(g, 1, 2, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 3, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 4, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node(g, 1);
	b = find_node(g, 2);
	c = find_node(g, 3);
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

PWTEST(infeasible_chain_fails_early)
{
	dag_t *g = create_test_dag(5, 1);

	pwtest_int_eq(dag_add_node(g, 1, 2, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 2, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 2, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	pwtest_errno(dag_recalculate(g), EAGAIN);
	pwtest_bool_true(g->dirty);
	assert_assignments_cleared(g);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(minimum_deadline_quantum_repairs_zero_wcet_path)
{
	static const uint32_t path[] = { 1, 2 };
	dag_t *g = create_test_dag(2, 1);
	dag_node_t *a, *b;

	pwtest_int_eq(dag_add_node(g, 1, 0, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node(g, 1);
	b = find_node(g, 2);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_bool_true(a->deadline_assigned);
	pwtest_bool_true(b->deadline_assigned);
	pwtest_int_eq((int) a->deadline, 1);
	pwtest_int_eq((int) b->deadline, 1);
	pwtest_bool_true(path_deadline_sum(g, path, SPA_N_ELEMENTS(path)) <= g->deadline);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(tight_feasible_chain_assigns_positive_deadlines)
{
	static const uint32_t path[] = { 1, 2, 3 };
	dag_t *g = create_test_dag(9, 1);
	dag_node_t *a, *b, *c;

	pwtest_int_eq(dag_add_node(g, 1, 2, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 3, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 4, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node(g, 1);
	b = find_node(g, 2);
	c = find_node(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_bool_false(g->dirty);
	pwtest_bool_true(a->deadline > 0);
	pwtest_bool_true(b->deadline > 0);
	pwtest_bool_true(c->deadline > 0);
	pwtest_int_eq((int) a->deadline, 2);
	pwtest_int_eq((int) b->deadline, 3);
	pwtest_int_eq((int) c->deadline, 4);
	pwtest_bool_true(path_deadline_sum(g, path, SPA_N_ELEMENTS(path)) <= g->deadline);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(diamond_deadlines)
{
	static const uint32_t left_path[] = { 1, 2, 4 };
	static const uint32_t right_path[] = { 1, 3, 4 };
	dag_t *g = create_test_dag(12, 4);
	dag_node_t *a, *b, *c, *d;

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 1, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 1, 3), 0);
	pwtest_int_eq(dag_add_node(g, 4, 1, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node(g, 1);
	b = find_node(g, 2);
	c = find_node(g, 3);
	d = find_node(g, 4);
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

PWTEST(chain_cpu_load_uses_unrelated_sets)
{
	dag_t *g = create_test_dag(9, 1);
	double exact_load, raw_load;

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 1, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	assert_all_on_cpu(g, 0);
	exact_load = cpu_exact_unrelated_load(g, 0);
	raw_load = cpu_raw_density_sum(g, 0);
	pwtest_bool_true(fabs(exact_load - (1.0 / 3.0)) < 1.0e-9);
	pwtest_bool_true(raw_load > exact_load);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(independent_tasks_load_sums_densities)
{
	dag_t *g = create_test_dag(10, 1);
	double exact_load, raw_load;

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 2, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 3, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	assert_all_on_cpu(g, 0);
	exact_load = cpu_exact_unrelated_load(g, 0);
	raw_load = cpu_raw_density_sum(g, 0);
	pwtest_bool_true(fabs(exact_load - raw_load) < 1.0e-9);
	pwtest_bool_true(fabs(exact_load - 0.6) < 1.0e-9);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(diamond_cpu_load_captures_parallelism)
{
	dag_t *g = create_test_dag_with_util(12, 0.6, 1);
	double exact_load, raw_load;

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 1, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 1, 3), 0);
	pwtest_int_eq(dag_add_node(g, 4, 1, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	assert_all_on_cpu(g, 0);
	exact_load = cpu_exact_unrelated_load(g, 0);
	raw_load = cpu_raw_density_sum(g, 0);
	pwtest_bool_true(fabs(exact_load - 0.5) < 1.0e-9);
	pwtest_bool_true(double_gt_eps(raw_load, g->utilization));
	pwtest_bool_true(double_leq_eps(exact_load, g->utilization));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(chain_topology_aware_admission_regression)
{
	dag_t *g = create_test_dag_with_util(9, 0.4, 1);
	double exact_load, raw_load;

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 1, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	assert_all_on_cpu(g, 0);
	exact_load = cpu_exact_unrelated_load(g, 0);
	raw_load = cpu_raw_density_sum(g, 0);
	pwtest_bool_true(double_gt_eps(raw_load, g->utilization));
	pwtest_bool_true(double_leq_eps(exact_load, g->utilization));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(near_bound_utilization_accepts_small_roundoff)
{
	dag_t *g = create_test_dag_with_util(9, nextafter(1.0 / 3.0, 0.0), 1);
	double exact_load;

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 1, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	assert_all_on_cpu(g, 0);
	exact_load = cpu_exact_unrelated_load(g, 0);
	pwtest_bool_true(double_eq_eps(exact_load, 1.0 / 3.0));
	pwtest_bool_true(double_leq_eps(exact_load, g->utilization));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(near_bound_utilization_rejects_clear_overload)
{
	dag_t *g = create_test_dag_with_util(9, 0.33, 1);

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 1, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_errno(dag_recalculate(g), EAGAIN);
	pwtest_bool_true(g->dirty);
	assert_assignments_cleared(g);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(ordinary_feasible_load_case_still_succeeds)
{
	dag_t *g = create_test_dag_with_util(10, 0.7, 1);
	double exact_load, raw_load;

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 2, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 3, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	assert_all_on_cpu(g, 0);
	exact_load = cpu_exact_unrelated_load(g, 0);
	raw_load = cpu_raw_density_sum(g, 0);
	pwtest_bool_true(double_eq_eps(exact_load, raw_load));
	pwtest_bool_true(double_eq_eps(exact_load, 0.6));
	pwtest_bool_true(double_leq_eps(exact_load, g->utilization));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(independent_tasks_are_placed_by_descending_density)
{
	dag_t *g = create_test_dag(10, 2);
	dag_node_t *a, *b, *c;

	pwtest_int_eq(dag_add_node(g, 1, 5, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 4, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 3, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node(g, 1);
	b = find_node(g, 2);
	c = find_node(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_int_eq((int) a->cpu, 0);
	pwtest_int_eq((int) b->cpu, 1);
	pwtest_int_eq((int) c->cpu, 1);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(equal_load_ties_choose_lowest_cpu)
{
	dag_t *g = create_test_dag(9, 2);

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 1, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	assert_all_on_cpu(g, 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(multiple_sources_and_sinks_skip_unreachable_pairs)
{
	static const uint32_t left_path[] = { 1, 3, 5 };
	static const uint32_t upper_path[] = { 1, 4, 6 };
	static const uint32_t lower_path[] = { 2, 4, 6 };
	dag_t *g = create_test_dag(18, 6);
	dag_node_t *node;

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 1, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 1, 3), 0);
	pwtest_int_eq(dag_add_node(g, 4, 1, 4), 0);
	pwtest_int_eq(dag_add_node(g, 5, 1, 5), 0);
	pwtest_int_eq(dag_add_node(g, 6, 1, 6), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 5), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 4, 6), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	spa_list_for_each(node, &g->nodes, link) {
		pwtest_bool_true(node->deadline_assigned);
		pwtest_bool_true(node->deadline > 0);
	}
	pwtest_bool_true(path_deadline_sum(g, left_path, SPA_N_ELEMENTS(left_path)) <= g->deadline);
	pwtest_bool_true(path_deadline_sum(g, upper_path, SPA_N_ELEMENTS(upper_path)) <= g->deadline);
	pwtest_bool_true(path_deadline_sum(g, lower_path, SPA_N_ELEMENTS(lower_path)) <= g->deadline);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(preassigned_tightening_residual_budget)
{
	static const uint32_t heavy_path[] = { 1, 2, 5, 6 };
	static const uint32_t discounted_path[] = { 1, 3, 4, 5, 6 };
	dag_t *g = create_test_dag(54, 6);
	dag_node_t *a, *b, *c, *x, *d, *e;

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 20, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 1, 3), 0);
	pwtest_int_eq(dag_add_node(g, 4, 1, 4), 0);
	pwtest_int_eq(dag_add_node(g, 5, 5, 5), 0);
	pwtest_int_eq(dag_add_node(g, 6, 1, 6), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 5), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 4, 5), 0);
	pwtest_int_eq(dag_add_edge(g, 5, 6), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	a = find_node(g, 1);
	b = find_node(g, 2);
	c = find_node(g, 3);
	x = find_node(g, 4);
	d = find_node(g, 5);
	e = find_node(g, 6);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_ptr_notnull(x);
	pwtest_ptr_notnull(d);
	pwtest_ptr_notnull(e);

	pwtest_int_eq((int)a->deadline, 2);
	pwtest_int_eq((int)b->deadline, 40);
	pwtest_int_eq((int)c->deadline, 20);
	pwtest_int_eq((int)d->deadline, 10);
	pwtest_int_eq((int)e->deadline, 2);
	pwtest_int_eq((int)x->deadline, 8);
	pwtest_bool_true(path_deadline_sum(g, heavy_path, SPA_N_ELEMENTS(heavy_path)) <= g->deadline);
	pwtest_bool_true(path_deadline_sum(g, discounted_path, SPA_N_ELEMENTS(discounted_path)) < g->deadline);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(topo_sort_rejects_forced_cycle)
{
	dag_t *g = create_test_dag(15, 3);

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 1, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);

	force_add_edge(g, 3, 1);
	pwtest_errno(dag_recalculate(g), ELOOP);
	pwtest_bool_true(g->dirty);
	assert_assignments_cleared(g);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(wcet_update_invalidates_schedule)
{
	dag_t *g = create_test_dag(18, 3);
	dag_node_t *a, *b, *c;
	struct foreach_stats stats = { 0, };

	pwtest_int_eq(dag_add_node(g, 1, 2, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 3, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 4, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_bool_false(g->dirty);

	a = find_node(g, 1);
	b = find_node(g, 2);
	c = find_node(g, 3);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_ptr_notnull(c);
	pwtest_int_eq((int) a->deadline, 4);
	pwtest_int_eq((int) b->deadline, 6);
	pwtest_int_eq((int) c->deadline, 8);

	pwtest_int_eq(dag_set_node_wcet(g, 2, 6), 0);
	pwtest_bool_true(g->dirty);
	assert_assignments_cleared(g);

	pwtest_int_eq(dag_foreach_node(g, collect_foreach_stats, &stats), 0);
	pwtest_bool_false(g->dirty);
	pwtest_int_eq((int) stats.count, 3);
	pwtest_int_eq((int) a->deadline, 3);
	pwtest_int_eq((int) b->deadline, 9);
	pwtest_int_eq((int) c->deadline, 6);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(edge_update_invalidates_schedule)
{
	dag_t *g = create_test_dag(9, 1);
	dag_node_t *a, *b;
	struct foreach_stats stats = { 0, };

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 1, 2), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_bool_false(g->dirty);

	a = find_node(g, 1);
	b = find_node(g, 2);
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(b);
	pwtest_int_eq((int) a->deadline, 9);
	pwtest_int_eq((int) b->deadline, 9);

	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_bool_true(g->dirty);
	assert_assignments_cleared(g);

	pwtest_int_eq(dag_foreach_node(g, collect_foreach_stats, &stats), 0);
	pwtest_bool_false(g->dirty);
	pwtest_int_eq((int) stats.count, 2);
	pwtest_int_eq((int) a->deadline, 4);
	pwtest_int_eq((int) b->deadline, 4);

	memset(&stats, 0, sizeof(stats));
	pwtest_int_eq(dag_remove_edge(g, 1, 2), 0);
	pwtest_bool_true(g->dirty);
	assert_assignments_cleared(g);

	pwtest_int_eq(dag_foreach_node(g, collect_foreach_stats, &stats), 0);
	pwtest_bool_false(g->dirty);
	pwtest_int_eq((int) stats.count, 2);
	pwtest_int_eq((int) a->deadline, 9);
	pwtest_int_eq((int) b->deadline, 9);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(recalculate_failure_keeps_dag_dirty)
{
	dag_t *g = create_test_dag(15, 3);
	struct foreach_stats stats = { 0, };

	pwtest_int_eq(dag_add_node(g, 1, 1, 1), 0);
	pwtest_int_eq(dag_add_node(g, 2, 1, 2), 0);
	pwtest_int_eq(dag_add_node(g, 3, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_bool_false(g->dirty);

	force_add_edge(g, 3, 1);
	pwtest_int_eq(dag_set_node_wcet(g, 1, 2), 0);
	pwtest_bool_true(g->dirty);
	assert_assignments_cleared(g);
	pwtest_errno(dag_foreach_node(g, collect_foreach_stats, &stats), ELOOP);
	pwtest_bool_true(g->dirty);
	pwtest_int_eq((int) stats.count, 0);
	assert_assignments_cleared(g);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST_SUITE(module_deadline_dag)
{
	pwtest_add(single_node_deadline, PWTEST_NOARG);
	pwtest_add(self_loop_rejected, PWTEST_NOARG);
	pwtest_add(cycle_edge_rejected, PWTEST_NOARG);
	pwtest_add(chain_deadlines, PWTEST_NOARG);
	pwtest_add(infeasible_chain_fails_early, PWTEST_NOARG);
	pwtest_add(minimum_deadline_quantum_repairs_zero_wcet_path, PWTEST_NOARG);
	pwtest_add(tight_feasible_chain_assigns_positive_deadlines, PWTEST_NOARG);
	pwtest_add(chain_cpu_load_uses_unrelated_sets, PWTEST_NOARG);
	pwtest_add(independent_tasks_load_sums_densities, PWTEST_NOARG);
	pwtest_add(diamond_deadlines, PWTEST_NOARG);
	pwtest_add(diamond_cpu_load_captures_parallelism, PWTEST_NOARG);
	pwtest_add(chain_topology_aware_admission_regression, PWTEST_NOARG);
	pwtest_add(near_bound_utilization_accepts_small_roundoff, PWTEST_NOARG);
	pwtest_add(near_bound_utilization_rejects_clear_overload, PWTEST_NOARG);
	pwtest_add(ordinary_feasible_load_case_still_succeeds, PWTEST_NOARG);
	pwtest_add(independent_tasks_are_placed_by_descending_density, PWTEST_NOARG);
	pwtest_add(equal_load_ties_choose_lowest_cpu, PWTEST_NOARG);
	pwtest_add(multiple_sources_and_sinks_skip_unreachable_pairs, PWTEST_NOARG);
	pwtest_add(preassigned_tightening_residual_budget, PWTEST_NOARG);
	pwtest_add(topo_sort_rejects_forced_cycle, PWTEST_NOARG);
	pwtest_add(wcet_update_invalidates_schedule, PWTEST_NOARG);
	pwtest_add(edge_update_invalidates_schedule, PWTEST_NOARG);
	pwtest_add(recalculate_failure_keeps_dag_dirty, PWTEST_NOARG);

	return PWTEST_PASS;
}
