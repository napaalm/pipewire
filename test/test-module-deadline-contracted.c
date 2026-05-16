/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * Unit tests for module-deadline/contracted.c.
 *
 * The contracted-DAG container holds a list of macro-nodes, each
 * with member nodes inherited from an original scheduling DAG, and
 * a list of de-duplicated directed edges between macro-nodes. The
 * tests pin: add-node and add-member behaviour, edge dedup,
 * self-loop rejection, cycle detection, and the n_members-driven
 * is_fusion_group flag.
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>

#include "pwtest.h"

#include "../src/modules/module-deadline/contracted.h"

PWTEST(contracted_create_destroy_null_safe)
{
	contracted_dag_destroy(NULL);
	pwtest_ptr_null(contracted_dag_add_node(NULL));
	pwtest_int_eq(contracted_node_add_member(NULL, 1, 100, 0), -EINVAL);
	pwtest_int_eq(contracted_dag_add_edge(NULL, NULL, NULL), -EINVAL);
	pwtest_bool_false(contracted_dag_has_cycle(NULL));
	return PWTEST_PASS;
}

PWTEST(contracted_create_returns_empty)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	pwtest_ptr_notnull(cg);
	pwtest_int_eq((int)cg->n_nodes, 0);
	pwtest_int_eq((int)cg->n_edges, 0);
	pwtest_int_eq((int)cg->period_ns, 1000);
	pwtest_int_eq((int)cg->deadline_ns, 1000);
	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_add_node_assigns_dense_id)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *a = contracted_dag_add_node(cg);
	contracted_node_t *b = contracted_dag_add_node(cg);
	contracted_node_t *c = contracted_dag_add_node(cg);
	pwtest_int_eq((int)a->id, 0);
	pwtest_int_eq((int)b->id, 1);
	pwtest_int_eq((int)c->id, 2);
	pwtest_int_eq((int)cg->n_nodes, 3);
	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_singleton_is_not_fusion_group)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *n = contracted_dag_add_node(cg);
	pwtest_int_eq(contracted_node_add_member(n, 42, 1234, 1000), 0);
	pwtest_int_eq((int)n->n_members, 1);
	pwtest_bool_false(n->is_fusion_group);
	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_multi_member_is_fusion_group)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *n = contracted_dag_add_node(cg);
	pwtest_int_eq(contracted_node_add_member(n, 1, 100, 50), 0);
	pwtest_int_eq(contracted_node_add_member(n, 2, 100, 30), 0);
	pwtest_int_eq((int)n->n_members, 2);
	pwtest_bool_true(n->is_fusion_group);
	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_add_edge_links_pred_and_succ)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *a = contracted_dag_add_node(cg);
	contracted_node_t *b = contracted_dag_add_node(cg);

	pwtest_int_eq(contracted_dag_add_edge(cg, a, b), 0);
	pwtest_int_eq((int)cg->n_edges, 1);

	contracted_edge_t *e;
	uint32_t succs = 0;
	spa_list_for_each(e, &a->succs, src_link) {
		pwtest_ptr_eq(e->dst, b);
		succs++;
	}
	pwtest_int_eq((int)succs, 1);

	uint32_t preds = 0;
	spa_list_for_each(e, &b->preds, dst_link) {
		pwtest_ptr_eq(e->src, a);
		preds++;
	}
	pwtest_int_eq((int)preds, 1);

	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_add_edge_deduplicates)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *a = contracted_dag_add_node(cg);
	contracted_node_t *b = contracted_dag_add_node(cg);

	pwtest_int_eq(contracted_dag_add_edge(cg, a, b), 0);
	pwtest_int_eq(contracted_dag_add_edge(cg, a, b), 0);
	pwtest_int_eq(contracted_dag_add_edge(cg, a, b), 0);
	pwtest_int_eq((int)cg->n_edges, 1);

	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_self_loop_rejected)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *a = contracted_dag_add_node(cg);
	pwtest_int_eq(contracted_dag_add_edge(cg, a, a), -EINVAL);
	pwtest_int_eq((int)cg->n_edges, 0);
	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_acyclic_chain_passes)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *a = contracted_dag_add_node(cg);
	contracted_node_t *b = contracted_dag_add_node(cg);
	contracted_node_t *c = contracted_dag_add_node(cg);
	pwtest_int_eq(contracted_dag_add_edge(cg, a, b), 0);
	pwtest_int_eq(contracted_dag_add_edge(cg, b, c), 0);
	pwtest_bool_false(contracted_dag_has_cycle(cg));
	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_cycle_detected)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *a = contracted_dag_add_node(cg);
	contracted_node_t *b = contracted_dag_add_node(cg);
	contracted_node_t *c = contracted_dag_add_node(cg);
	pwtest_int_eq(contracted_dag_add_edge(cg, a, b), 0);
	pwtest_int_eq(contracted_dag_add_edge(cg, b, c), 0);
	pwtest_int_eq(contracted_dag_add_edge(cg, c, a), 0);
	pwtest_bool_true(contracted_dag_has_cycle(cg));
	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST_SUITE(module_deadline_contracted)
{
	pwtest_add(contracted_create_destroy_null_safe, PWTEST_NOARG);
	pwtest_add(contracted_create_returns_empty, PWTEST_NOARG);
	pwtest_add(contracted_add_node_assigns_dense_id, PWTEST_NOARG);
	pwtest_add(contracted_singleton_is_not_fusion_group, PWTEST_NOARG);
	pwtest_add(contracted_multi_member_is_fusion_group, PWTEST_NOARG);
	pwtest_add(contracted_add_edge_links_pred_and_succ, PWTEST_NOARG);
	pwtest_add(contracted_add_edge_deduplicates, PWTEST_NOARG);
	pwtest_add(contracted_self_loop_rejected, PWTEST_NOARG);
	pwtest_add(contracted_acyclic_chain_passes, PWTEST_NOARG);
	pwtest_add(contracted_cycle_detected, PWTEST_NOARG);

	return PWTEST_PASS;
}
