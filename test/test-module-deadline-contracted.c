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
#include "../src/modules/module-deadline/dag.h"

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

/* --- overhead tests --- */

PWTEST(contracted_overhead_defaults_to_zero)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *n = contracted_dag_add_node(cg);
	pwtest_int_eq(contracted_node_add_member(n, 1, 100, 500), 0);
	n->wcet_ns += 500;
	pwtest_int_eq((int)n->overhead_ns, 0);
	pwtest_int_eq((int)n->overhead.group_dispatch_ns, 0);
	pwtest_int_eq((int)n->overhead.internal_topo_ns, 0);
	pwtest_int_eq((int)n->overhead.activation_pending_ns, 0);
	pwtest_int_eq((int)n->overhead.buffer_port_iter_ns, 0);
	pwtest_int_eq((int)n->overhead.wakeup_savings_ns, 0);
	pwtest_int_eq((int)contracted_node_effective_wcet(n), 500);
	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_overhead_set_aggregates_components)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *n = contracted_dag_add_node(cg);
	pwtest_int_eq(contracted_node_add_member(n, 1, 100, 500), 0);
	n->wcet_ns += 500;

	struct contracted_overhead_components c = {
		.group_dispatch_ns     = 30,
		.internal_topo_ns      = 20,
		.activation_pending_ns = 10,
		.buffer_port_iter_ns   = 40,
		.wakeup_savings_ns     = 200, /* observational only */
	};
	pwtest_int_eq(contracted_node_set_overhead(n, &c), 0);

	/* 30 + 20 + 10 + 40 = 100; wakeup_savings_ns NOT subtracted. */
	pwtest_int_eq((int)n->overhead_ns, 100);
	pwtest_int_eq((int)n->overhead.wakeup_savings_ns, 200);
	pwtest_int_eq((int)contracted_node_effective_wcet(n), 600);
	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_overhead_set_rejects_null)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *n = contracted_dag_add_node(cg);
	struct contracted_overhead_components c = { 0 };
	pwtest_int_eq(contracted_node_set_overhead(NULL, &c), -EINVAL);
	pwtest_int_eq(contracted_node_set_overhead(n, NULL), -EINVAL);
	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_overhead_effective_wcet_null_safe)
{
	pwtest_int_eq((int)contracted_node_effective_wcet(NULL), 0);
	return PWTEST_PASS;
}

PWTEST(contracted_overhead_set_replaces_not_adds)
{
	/* set_overhead overwrites the components struct verbatim;
	 * two calls do not accumulate. The analysis layer relies on
	 * this when it refreshes the overhead between recalc passes. */
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *n = contracted_dag_add_node(cg);
	struct contracted_overhead_components c1 = { 100, 0, 0, 0, 0 };
	struct contracted_overhead_components c2 = { 50, 0, 0, 0, 0 };
	pwtest_int_eq(contracted_node_set_overhead(n, &c1), 0);
	pwtest_int_eq((int)n->overhead_ns, 100);
	pwtest_int_eq(contracted_node_set_overhead(n, &c2), 0);
	pwtest_int_eq((int)n->overhead_ns, 50);
	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

/* --- bridge tests --- */

/* Build a chain {A,B} -> C contracted DAG, project to dag_t, run
 * the splitter, copy the schedule back. Verify the macro-node's
 * fields are populated and that the AB macro-node (with overhead)
 * sees its effective WCET as its kernel runtime input. */
PWTEST(contracted_bridge_round_trip)
{
	contracted_dag_t *cg;
	struct dag *g = NULL;
	int r;

	cg = contracted_dag_create(1000, 1000);
	contracted_node_t *ab = contracted_dag_add_node(cg);
	contracted_node_t *c  = contracted_dag_add_node(cg);

	/* AB: two members with WCETs 50 + 30, plus 10ns overhead. */
	pwtest_int_eq(contracted_node_add_member(ab, 10, 1010, 50), 0);
	pwtest_int_eq(contracted_node_add_member(ab, 11, 1011, 30), 0);
	ab->wcet_ns = 80;
	struct contracted_overhead_components ohc = {
		.group_dispatch_ns = 10, 0, 0, 0, 0,
	};
	pwtest_int_eq(contracted_node_set_overhead(ab, &ohc), 0);

	/* C: singleton, WCET 100, no overhead. */
	pwtest_int_eq(contracted_node_add_member(c, 12, 1012, 100), 0);
	c->wcet_ns = 100;

	pwtest_int_eq(contracted_dag_add_edge(cg, ab, c), 0);

	/* Bridge to dag_t and run analysis. */
	r = contracted_dag_to_dag(cg, 0.95, 1, NULL, &g);
	pwtest_int_eq(r, 0);
	pwtest_ptr_notnull(g);

	pwtest_int_eq(dag_recalculate(g), 0);

	/* AB's dag_node wcet must equal the effective WCET (work +
	 * overhead). The leader's tid (lowest member id 10) becomes
	 * the dag_node tid. */
	dag_node_t *ab_dn = dag_find_node(g, ab->id);
	pwtest_ptr_notnull(ab_dn);
	pwtest_int_eq((int)ab_dn->wcet, 90);   /* 50 + 30 + 10 */
	pwtest_int_eq((int)ab_dn->tid, 1010);
	pwtest_bool_true(ab_dn->local_deadline > 0);
	pwtest_bool_true(ab_dn->cumulative_deadline > 0);

	dag_node_t *c_dn = dag_find_node(g, c->id);
	pwtest_int_eq((int)c_dn->wcet, 100);
	pwtest_int_eq((int)c_dn->tid, 1012);

	/* Copy results back. */
	pwtest_int_eq(contracted_dag_apply_dag_schedule(cg, g), 0);
	pwtest_bool_true(ab->local_deadline_ns > 0);
	pwtest_bool_true(ab->cumulative_deadline_ns > 0);
	pwtest_int_eq((int)ab->runtime_budget_ns, 90);
	pwtest_int_eq((int)ab->local_deadline_ns,
		      (int)ab_dn->local_deadline);
	pwtest_int_eq((int)ab->cumulative_deadline_ns,
		      (int)ab_dn->cumulative_deadline);
	pwtest_int_eq(ab->cpu, (int)ab_dn->cpu);

	pwtest_bool_true(c->local_deadline_ns > 0);
	pwtest_int_eq((int)c->runtime_budget_ns, 100);

	/* Edge is preserved: in the dag_t there is exactly one
	 * incoming edge to C from AB. */
	dag_edge_t *e;
	uint32_t c_preds = 0;
	spa_list_for_each(e, &c_dn->incoming, dst_link) {
		pwtest_ptr_eq(e->src, ab_dn);
		c_preds++;
	}
	pwtest_int_eq((int)c_preds, 1);

	dag_destroy(g);
	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_bridge_null_safe)
{
	struct dag *g = NULL;
	pwtest_int_eq(contracted_dag_to_dag(NULL, 0.95, 1, NULL, &g),
		      -EINVAL);
	pwtest_int_eq(contracted_dag_to_dag((contracted_dag_t *)0x1,
				0.95, 1, NULL, NULL), -EINVAL);
	pwtest_int_eq(contracted_dag_apply_dag_schedule(NULL, NULL), -EINVAL);
	return PWTEST_PASS;
}

/* If the dag_t is missing a macro-node id (e.g., because the
 * caller built a partial schedule), apply_dag_schedule must leave
 * the contracted node's existing fields alone rather than zero
 * them. */
PWTEST(contracted_bridge_missing_node_preserves_fields)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *cn = contracted_dag_add_node(cg);
	pwtest_int_eq(contracted_node_add_member(cn, 1, 100, 50), 0);
	cn->wcet_ns = 50;
	cn->local_deadline_ns = 12345;
	cn->cumulative_deadline_ns = 67890;
	cn->runtime_budget_ns = 50;
	cn->cpu = 7;

	/* An empty dag_t has no matching macro-node id. */
	struct dag *g = dag_create(1000, 1000, 0.95, 1, NULL);
	pwtest_int_eq(contracted_dag_apply_dag_schedule(cg, g), 0);
	pwtest_int_eq((int)cn->local_deadline_ns, 12345);
	pwtest_int_eq((int)cn->cumulative_deadline_ns, 67890);
	pwtest_int_eq((int)cn->runtime_budget_ns, 50);
	pwtest_int_eq(cn->cpu, 7);

	dag_destroy(g);
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
	pwtest_add(contracted_builder_chain_fuse_first_two, PWTEST_NOARG);
	pwtest_add(contracted_builder_drops_internal_edges, PWTEST_NOARG);
	pwtest_add(contracted_builder_deduplicates_parallel_external_edges, PWTEST_NOARG);
	pwtest_add(contracted_builder_singletons_only, PWTEST_NOARG);
	pwtest_add(contracted_builder_rejects_duplicate_ids, PWTEST_NOARG);
	pwtest_add(contracted_builder_rejects_dangling_edge, PWTEST_NOARG);
	pwtest_add(contracted_overhead_defaults_to_zero, PWTEST_NOARG);
	pwtest_add(contracted_overhead_set_aggregates_components, PWTEST_NOARG);
	pwtest_add(contracted_overhead_set_rejects_null, PWTEST_NOARG);
	pwtest_add(contracted_overhead_effective_wcet_null_safe, PWTEST_NOARG);
	pwtest_add(contracted_overhead_set_replaces_not_adds, PWTEST_NOARG);
	pwtest_add(contracted_bridge_round_trip, PWTEST_NOARG);
	pwtest_add(contracted_bridge_null_safe, PWTEST_NOARG);
	pwtest_add(contracted_bridge_missing_node_preserves_fields, PWTEST_NOARG);

	return PWTEST_PASS;
}
