/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>
#include <stdbool.h>

#include "pwtest.h"

#include "../src/modules/module-deadline/dag.h"

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

static uint32_t dag_node_count(dag_t *g)
{
	uint32_t count = 0;
	dag_node_t *n;

	spa_list_for_each(n, &g->nodes, link)
		count++;

	return count;
}

PWTEST(chain_uses_peak_not_sum)
{
	dag_t *g = dag_create(100, 100, 0.55f, 1);
	pwtest_ptr_notnull(g);

	pwtest_int_eq(dag_add_node(g, 1, 10, 101, true, false), 0);
	pwtest_int_eq(dag_add_node(g, 2, 10, 102, false, false), 0);
	pwtest_int_eq(dag_add_node(g, 3, 10, 103, false, true), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	dag_node_t *n1 = find_node_by_id(g, 1);
	dag_node_t *n2 = find_node_by_id(g, 2);
	dag_node_t *n3 = find_node_by_id(g, 3);
	pwtest_ptr_notnull(n1);
	pwtest_ptr_notnull(n2);
	pwtest_ptr_notnull(n3);

	pwtest_int_eq((int)g->indexed_count, 3);
	pwtest_int_eq(g->relatives[n1->index][n1->index], 1);
	pwtest_int_eq(g->relatives[n1->index][n2->index], 1);
	pwtest_int_eq(g->relatives[n2->index][n1->index], 1);
	pwtest_int_eq(g->relatives[n1->index][n3->index], 1);
	pwtest_int_eq(g->relatives[n3->index][n1->index], 1);
	pwtest_int_eq((int)dag_max_unrelated_size(g), 1);
	pwtest_int_eq((int)n1->cpu, 0);
	pwtest_int_eq((int)n2->cpu, 0);
	pwtest_int_eq((int)n3->cpu, 0);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(fork_join_fails_on_peak_concurrency)
{
	dag_t *g = dag_create(100, 100, 0.55f, 1);
	static const uint32_t middle_parallel[] = { 2, 3 };
	int res;

	pwtest_ptr_notnull(g);

	pwtest_int_eq(dag_add_node(g, 1, 10, 101, true, false), 0);
	pwtest_int_eq(dag_add_node(g, 2, 10, 102, false, false), 0);
	pwtest_int_eq(dag_add_node(g, 3, 10, 103, false, false), 0);
	pwtest_int_eq(dag_add_node(g, 4, 10, 104, false, true), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);

	res = dag_recalculate(g);
	pwtest_int_eq(res, -1);
	pwtest_int_eq(errno, EAGAIN);
	pwtest_bool_true(dag_has_unrelated_subset(g, middle_parallel, 2));

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

	pwtest_int_eq(dag_add_node(g, 1, 10, 101, true, false), 0);
	pwtest_int_eq(dag_add_node(g, 2, 10, 102, true, false), 0);
	pwtest_int_eq(dag_add_node(g, 3, 10, 103, false, false), 0);
	pwtest_int_eq(dag_add_node(g, 4, 10, 104, false, true), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 3, 4), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 4), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	pwtest_bool_true(dag_has_unrelated_subset(g, sources, 2));
	pwtest_bool_true(dag_has_unrelated_subset(g, mixed_cut, 2));
	pwtest_bool_false(dag_has_unrelated_subset(g, invalid_cut, 2));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(topology_change_rebuilds_analysis)
{
	dag_t *g = dag_create(100, 100, 0.90f, 1);
	static const uint32_t concurrent_pair[] = { 1, 3 };

	pwtest_ptr_notnull(g);

	pwtest_int_eq(dag_add_node(g, 1, 10, 101, true, false), 0);
	pwtest_int_eq(dag_add_node(g, 2, 10, 102, false, true), 0);
	pwtest_int_eq(dag_add_node(g, 3, 10, 103, true, true), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)g->indexed_count, 3);
	pwtest_int_eq((int)dag_max_unrelated_size(g), 2);
	pwtest_bool_true(dag_has_unrelated_subset(g, concurrent_pair, 2));

	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_ptr_eq(g->indexed_nodes, NULL);
	pwtest_ptr_eq(g->relatives, NULL);
	pwtest_ptr_eq(g->unrelated, NULL);
	pwtest_int_eq((int)g->indexed_count, 0);
	pwtest_int_eq((int)g->unrelated_size, 0);

	pwtest_int_eq(dag_recalculate(g), 0);
	pwtest_int_eq((int)g->indexed_count, 3);
	pwtest_int_eq((int)dag_max_unrelated_size(g), 1);
	pwtest_bool_false(dag_has_unrelated_subset(g, concurrent_pair, 2));

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST(spurious_nodes_are_removed_from_analysis)
{
	dag_t *g = dag_create(100, 100, 0.90f, 1);

	pwtest_ptr_notnull(g);

	pwtest_int_eq(dag_add_node(g, 1, 10, 101, true, false), 0);
	pwtest_int_eq(dag_add_node(g, 2, 10, 102, false, false), 0);
	pwtest_int_eq(dag_add_node(g, 3, 10, 103, false, true), 0);
	pwtest_int_eq(dag_add_node(g, 4, 10, 104, false, false), 0);
	pwtest_int_eq(dag_add_node(g, 5, 10, 105, true, false), 0);
	pwtest_int_eq(dag_add_node(g, 6, 10, 106, false, true), 0);
	pwtest_int_eq(dag_add_node(g, 7, 10, 107, false, false), 0);

	pwtest_int_eq(dag_add_edge(g, 1, 2), 0);
	pwtest_int_eq(dag_add_edge(g, 2, 3), 0);
	pwtest_int_eq(dag_add_edge(g, 1, 4), 0);

	pwtest_int_eq(dag_recalculate(g), 0);

	pwtest_int_eq((int)dag_node_count(g), 3);
	pwtest_int_eq((int)g->indexed_count, 3);
	pwtest_ptr_notnull(find_node_by_id(g, 1));
	pwtest_ptr_notnull(find_node_by_id(g, 2));
	pwtest_ptr_notnull(find_node_by_id(g, 3));
	pwtest_ptr_eq(find_node_by_id(g, 4), NULL);
	pwtest_ptr_eq(find_node_by_id(g, 5), NULL);
	pwtest_ptr_eq(find_node_by_id(g, 6), NULL);
	pwtest_ptr_eq(find_node_by_id(g, 7), NULL);

	dag_destroy(g);
	return PWTEST_PASS;
}

PWTEST_SUITE(module_deadline_dag)
{
	pwtest_add(chain_uses_peak_not_sum, PWTEST_NOARG);
	pwtest_add(fork_join_fails_on_peak_concurrency, PWTEST_NOARG);
	pwtest_add(multi_source_initial_cut_and_cleanup, PWTEST_NOARG);
	pwtest_add(topology_change_rebuilds_analysis, PWTEST_NOARG);
	pwtest_add(spurious_nodes_are_removed_from_analysis, PWTEST_NOARG);

	return PWTEST_PASS;
}
