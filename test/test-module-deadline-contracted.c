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

/* --- builder tests --- */

/* Chain A -> B -> C with fusion {A, B} produces a 2-node
 * contracted DAG {AB, C} with one edge AB -> C and macro WCET
 * for AB equal to W_A + W_B (overhead left at 0). */
PWTEST(contracted_builder_chain_fuse_first_two)
{
	struct contracted_member_input members[] = {
		{ 1, 1001, 10 },
		{ 2, 1002, 20 },
		{ 3, 1003, 30 },
	};
	uint32_t groups[] = { 7, 7, 0 };
	struct contracted_edge_input edges[] = {
		{ 1, 2 },
		{ 2, 3 },
	};
	contracted_dag_t *cg = NULL;

	pwtest_int_eq(contracted_dag_build(100, 100,
			members, groups, 3,
			edges, 2, &cg), 0);
	pwtest_ptr_notnull(cg);
	pwtest_int_eq((int)cg->n_nodes, 2);
	pwtest_int_eq((int)cg->n_edges, 1);
	pwtest_bool_false(contracted_dag_has_cycle(cg));

	/* Find the fused macro-node (2 members) and the singleton. */
	contracted_node_t *ab = NULL, *c = NULL;
	contracted_node_t *cn;
	spa_list_for_each(cn, &cg->nodes, link) {
		if (cn->n_members == 2)
			ab = cn;
		else if (cn->n_members == 1)
			c = cn;
	}
	pwtest_ptr_notnull(ab);
	pwtest_ptr_notnull(c);

	/* Members of AB are nodes 1 and 2; member of C is node 3. */
	pwtest_int_eq((int)ab->wcet_ns, 30);
	pwtest_int_eq((int)c->wcet_ns, 30);
	pwtest_bool_true(ab->is_fusion_group);
	pwtest_bool_false(c->is_fusion_group);

	/* The contracted edge runs AB -> C, not C -> AB. */
	contracted_edge_t *ce;
	uint32_t ab_succs = 0;
	spa_list_for_each(ce, &ab->succs, src_link) {
		pwtest_ptr_eq(ce->dst, c);
		ab_succs++;
	}
	pwtest_int_eq((int)ab_succs, 1);

	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

/* Internal edges (between members of the same group) disappear;
 * the dropped count matches the count of within-group edges. */
PWTEST(contracted_builder_drops_internal_edges)
{
	struct contracted_member_input members[] = {
		{ 1, 1001, 10 },
		{ 2, 1002, 20 },
		{ 3, 1003, 30 },
	};
	uint32_t groups[] = { 7, 7, 7 };
	struct contracted_edge_input edges[] = {
		{ 1, 2 },
		{ 2, 3 },
	};
	contracted_dag_t *cg = NULL;

	pwtest_int_eq(contracted_dag_build(100, 100,
			members, groups, 3,
			edges, 2, &cg), 0);
	pwtest_int_eq((int)cg->n_nodes, 1);
	pwtest_int_eq((int)cg->n_edges, 0);
	contracted_node_t *cn = spa_list_first(&cg->nodes,
			contracted_node_t, link);
	pwtest_int_eq((int)cn->n_members, 3);
	pwtest_int_eq((int)cn->wcet_ns, 60);

	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

/* Two original edges A -> X and B -> X with {A, B} fused dedupe
 * to a single AB -> X edge. */
PWTEST(contracted_builder_deduplicates_parallel_external_edges)
{
	struct contracted_member_input members[] = {
		{ 1, 1001, 10 },
		{ 2, 1002, 20 },
		{ 3, 1003, 30 },
	};
	uint32_t groups[] = { 7, 7, 0 };
	struct contracted_edge_input edges[] = {
		{ 1, 3 },
		{ 2, 3 },
	};
	contracted_dag_t *cg = NULL;

	pwtest_int_eq(contracted_dag_build(100, 100,
			members, groups, 3,
			edges, 2, &cg), 0);
	pwtest_int_eq((int)cg->n_nodes, 2);
	pwtest_int_eq((int)cg->n_edges, 1);

	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_builder_singletons_only)
{
	struct contracted_member_input members[] = {
		{ 1, 1001, 10 },
		{ 2, 1002, 20 },
	};
	uint32_t groups[] = { 0, 0 };
	struct contracted_edge_input edges[] = {
		{ 1, 2 },
	};
	contracted_dag_t *cg = NULL;

	pwtest_int_eq(contracted_dag_build(100, 100,
			members, groups, 2,
			edges, 1, &cg), 0);
	pwtest_int_eq((int)cg->n_nodes, 2);
	pwtest_int_eq((int)cg->n_edges, 1);
	contracted_node_t *cn;
	spa_list_for_each(cn, &cg->nodes, link)
		pwtest_int_eq((int)cn->n_members, 1);
	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_builder_rejects_duplicate_ids)
{
	struct contracted_member_input members[] = {
		{ 1, 1001, 10 },
		{ 1, 1002, 20 },
	};
	uint32_t groups[] = { 0, 0 };
	contracted_dag_t *cg = NULL;

	pwtest_int_eq(contracted_dag_build(100, 100,
			members, groups, 2,
			NULL, 0, &cg), -EINVAL);
	pwtest_ptr_null(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_builder_rejects_dangling_edge)
{
	struct contracted_member_input members[] = {
		{ 1, 1001, 10 },
	};
	uint32_t groups[] = { 0 };
	struct contracted_edge_input edges[] = {
		{ 1, 99 },
	};
	contracted_dag_t *cg = NULL;

	pwtest_int_eq(contracted_dag_build(100, 100,
			members, groups, 1,
			edges, 1, &cg), -ENOTRECOVERABLE);
	pwtest_ptr_null(cg);
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
	pwtest_add(contracted_builder_chain_fuse_first_two, PWTEST_NOARG);
	pwtest_add(contracted_builder_drops_internal_edges, PWTEST_NOARG);
	pwtest_add(contracted_builder_deduplicates_parallel_external_edges, PWTEST_NOARG);
	pwtest_add(contracted_builder_singletons_only, PWTEST_NOARG);
	pwtest_add(contracted_builder_rejects_duplicate_ids, PWTEST_NOARG);
	pwtest_add(contracted_builder_rejects_dangling_edge, PWTEST_NOARG);

	return PWTEST_PASS;
}
