/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 PipeWire contributors */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include "pwtest.h"

#include "module-deadline/dag.h"

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

static dag_t *create_test_dag(uint64_t deadline, uint32_t num_cpus)
{
	dag_t *g = dag_create(deadline, deadline, 1.0f, num_cpus);

	pwtest_ptr_notnull(g);
	return g;
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

PWTEST_SUITE(module_deadline_dag)
{
	pwtest_add(single_node_deadline, PWTEST_NOARG);
	pwtest_add(chain_deadlines, PWTEST_NOARG);
	pwtest_add(diamond_deadlines, PWTEST_NOARG);
	pwtest_add(preassigned_tightening_residual_budget, PWTEST_NOARG);

	return PWTEST_PASS;
}
