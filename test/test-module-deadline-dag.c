/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
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

static void foreach_count_cb(void *data, pid_t tid, uint64_t wcet,
		uint64_t deadline, uint64_t period, uint32_t cpu)
{
	struct foreach_info *info = data;

	(void)deadline;
	(void)period;
	(void)cpu;

	info->count++;
	if (tid < 0 || wcet == 0)
		info->saw_internal_tid = true;
}

PWTEST(chain_uses_peak_not_sum)
{
	dag_t *g = dag_create(100, 100, 0.55f, 1);
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
	dag_t *g = dag_create(100, 100, 0.55f, 1);
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
	dag_t *g = dag_create(100, 100, 0.90f, 1);
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
	dag_t *g = dag_create(100, 100, 0.90f, 1);
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
	dag_t *g = dag_create(100, 100, 0.90f, 1);

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
	dag_t *g = dag_create(100, 100, 0.90f, 1);

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
	dag_t *g = dag_create(100, 100, 0.90f, 1);
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
	dag_t *g = dag_create(100, 100, 0.90f, 2);
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
	dag_t *g = dag_create(100, 100, 0.10f, 2);
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
	dag_t *g = dag_create(25, 25, 1.0f, 1);
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
	dag_t *g = dag_create(18, 18, 1.0f, 3);
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
	dag_t *g = dag_create(12, 12, 1.0f, 4);
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
	dag_t *g = dag_create(54, 54, 1.0f, 6);
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

/* U-resbudget: a fork-join where one side of the fork has a much
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
	dag_t *g = dag_create(1000, 1000, 0.95f, 4);
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

PWTEST(equal_load_ties_choose_lowest_cpu)
{
	dag_t *g = dag_create(300, 300, 0.10f, 2);
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

PWTEST_SUITE(module_deadline_dag)
{
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

	return PWTEST_PASS;
}
