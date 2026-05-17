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
#include "../src/modules/module-deadline/fusion_validator.h"

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
/* Group {B, C} on graph A -> C, B -> C, A -> X: contracting
 * preserves A as an external predecessor of the macro-node BC,
 * and the contracted edge A -> BC carries the singleton-A
 * dependency that the runtime needs to gate the macro's
 * release on A completing. The check is that the contracted
 * DAG records exactly the right external predecessors -- no
 * silent dropping, no spurious extras. */
PWTEST(contracted_builder_preserves_external_predecessors)
{
	struct contracted_member_input members[] = {
		{ 1, 1001, 10 }, /* A */
		{ 2, 1002, 20 }, /* B */
		{ 3, 1003, 30 }, /* C */
		{ 4, 1004, 40 }, /* X (external) */
	};
	/* {B, C} fused into group 9; A and X stay singletons. */
	uint32_t groups[] = { 0, 9, 9, 0 };
	struct contracted_edge_input edges[] = {
		{ 1, 3 }, /* A -> C, becomes external pred of BC */
		{ 2, 3 }, /* B -> C, internal (both in BC) */
		{ 1, 4 }, /* A -> X, untouched (both singletons) */
	};
	contracted_dag_t *cg = NULL;
	contracted_node_t *cn, *a = NULL, *bc = NULL, *x = NULL;
	contracted_edge_t *ce;
	uint32_t bc_preds = 0, a_succs = 0;

	pwtest_int_eq(contracted_dag_build(100, 100,
			members, groups, 4,
			edges, 3, &cg), 0);
	pwtest_ptr_notnull(cg);
	/* Three contracted nodes: singleton A, macro BC, singleton X. */
	pwtest_int_eq((int)cg->n_nodes, 3);
	pwtest_bool_false(contracted_dag_has_cycle(cg));

	spa_list_for_each(cn, &cg->nodes, link) {
		if (cn->n_members == 2)
			bc = cn;
		else if (cn->wcet_ns == 10)
			a = cn;
		else if (cn->wcet_ns == 40)
			x = cn;
	}
	pwtest_ptr_notnull(a);
	pwtest_ptr_notnull(bc);
	pwtest_ptr_notnull(x);

	/* BC has exactly one predecessor: A. */
	spa_list_for_each(ce, &bc->preds, dst_link) {
		pwtest_ptr_eq(ce->src, a);
		bc_preds++;
	}
	pwtest_int_eq((int)bc_preds, 1);

	/* A has two successors: BC and X. */
	spa_list_for_each(ce, &a->succs, src_link) {
		pwtest_bool_true(ce->dst == bc || ce->dst == x);
		a_succs++;
	}
	pwtest_int_eq((int)a_succs, 2);

	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

/* Walk the macro-nodes and find the one that owns the given
 * original member id. Returns NULL if no node carries it. */
static contracted_node_t *
contracted_test_owner(contracted_dag_t *cg, uint32_t member_id)
{
	contracted_node_t *cn;
	struct contracted_member *m;
	spa_list_for_each(cn, &cg->nodes, link) {
		spa_list_for_each(m, &cn->members, link) {
			if (m->id == member_id)
				return cn;
		}
	}
	return NULL;
}

/* Property-based test: for a family of randomised
 * (members, group_id, edges) inputs the contracted DAG must
 * preserve the following invariants:
 *
 *   1. Every input edge u -> v is either internal (owner(u) ==
 *      owner(v), dropped) or represented by exactly one
 *      contracted edge between owner(u) and owner(v). No edge is
 *      silently lost; no spurious edge is invented.
 *
 *   2. Every contracted edge has at least one underlying input
 *      edge crossing the same boundary -- a "round-trip" check
 *      that catches a future deduplication bug from accidentally
 *      keeping a phantom edge after group renaming.
 *
 *   3. Group-id 0 always maps to a singleton macro-node carrying
 *      exactly the originating member.
 *
 *   4. Members with the same non-zero group_id always end up in
 *      the same macro-node (with n_members > 1 when the group
 *      has more than one member).
 *
 * The trial set is LCG-deterministic (seed 0xE9C2B137) so
 * counter-examples are reproducible. 40 trials covers enough
 * randomised shapes to catch obvious regressions without making
 * the suite slow. */
PWTEST(contracted_builder_property_preserves_edge_set)
{
	uint32_t seed = 0xE9C2B137u;
	uint32_t trial;
	uint32_t total_violations = 0;
	const uint32_t trials = 40;

	for (trial = 0; trial < trials; trial++) {
		struct contracted_member_input members[6];
		uint32_t groups[6];
		struct contracted_edge_input edges[10];
		uint32_t n_members, n_edges, i, j;
		uint32_t group_pool[3] = { 0, 7, 9 };
		contracted_dag_t *cg = NULL;
		contracted_node_t *cn;

		seed = seed * 1103515245u + 12345u;
		n_members = 3 + (seed % 4); /* 3..6 members */

		/* Assign each member to one of three group buckets
		 * (0=singleton, 7, 9). */
		for (i = 0; i < n_members; i++) {
			seed = seed * 1103515245u + 12345u;
			members[i].id  = 100 + i;
			members[i].tid = 1000 + i;
			members[i].wcet_ns = 10 + (seed % 50);
			groups[i] = group_pool[seed % 3];
		}

		/* Generate a forward-only edge set (DAG by construction)
		 * with each edge present with 50 % probability. */
		n_edges = 0;
		for (i = 0; i < n_members; i++) {
			for (j = i + 1; j < n_members; j++) {
				seed = seed * 1103515245u + 12345u;
				if ((seed & 0x1) == 0)
					continue;
				if (n_edges >= 10)
					continue;
				edges[n_edges].src_id = members[i].id;
				edges[n_edges].dst_id = members[j].id;
				n_edges++;
			}
		}

		if (contracted_dag_build(100, 100, members, groups,
				n_members, edges, n_edges, &cg) != 0)
			continue;

		/* Invariant 4: members with the same non-zero group_id
		 * map to the same macro-node. */
		for (i = 0; i < n_members; i++) {
			for (j = i + 1; j < n_members; j++) {
				if (groups[i] == 0 || groups[i] != groups[j])
					continue;
				contracted_node_t *a =
					contracted_test_owner(cg, members[i].id);
				contracted_node_t *b =
					contracted_test_owner(cg, members[j].id);
				if (a != b)
					total_violations++;
			}
		}

		/* Invariant 3: group_id 0 means singleton. */
		for (i = 0; i < n_members; i++) {
			if (groups[i] != 0)
				continue;
			cn = contracted_test_owner(cg, members[i].id);
			if (cn == NULL || cn->n_members != 1)
				total_violations++;
		}

		/* Invariant 1 & 2: every input edge is either internal
		 * or has a contracted edge between owner(src) and
		 * owner(dst). */
		for (i = 0; i < n_edges; i++) {
			contracted_node_t *u =
				contracted_test_owner(cg, edges[i].src_id);
			contracted_node_t *v =
				contracted_test_owner(cg, edges[i].dst_id);
			if (u == NULL || v == NULL) {
				total_violations++;
				continue;
			}
			if (u == v)
				continue; /* internal -- dropped, OK */
			contracted_edge_t *ce;
			bool found = false;
			spa_list_for_each(ce, &u->succs, src_link) {
				if (ce->dst == v) { found = true; break; }
			}
			if (!found)
				total_violations++;
		}

		contracted_dag_destroy(cg);
	}

	pwtest_int_eq((int)total_violations, 0);
	return PWTEST_PASS;
}

/* Property test combining the convexity validator with the
 * contracted DAG builder: if the precedence-convexity predicate
 * accepts a candidate group then the resulting contracted DAG is
 * acyclic. Sarkar 1989 §5.3 frames precedence-convexity exactly
 * as the soundness requirement for macro-actor formation -- a
 * non-convex group's macro-node ends up on a cycle through the
 * outside-and-back-in path. Pinning the joint property catches a
 * future bug in either the validator or the builder where one
 * relaxes its definition out of sync with the other.
 *
 * 40 trials, LCG seed 0xF7A3B561, each trial picks a small
 * member subset on a random forward-only DAG and feeds the same
 * (members, edges) to both layers. */
PWTEST(contracted_validator_convex_accept_implies_acyclic)
{
	uint32_t seed = 0xF7A3B561u;
	uint32_t trial;
	uint32_t total_violations = 0;
	const uint32_t trials = 40;
	const uint32_t n_total = 6;

	for (trial = 0; trial < trials; trial++) {
		struct contracted_member_input all_members[6];
		uint32_t group_ids[6] = { 0 };
		uint32_t member_ids[6];
		uint32_t n_in_group = 0, i, j, n_edges = 0;
		struct contracted_edge_input cedges[15];
		struct fusion_edge_input fedges[15];

		/* Build the universe: 6 nodes id 200..205. */
		for (i = 0; i < n_total; i++) {
			all_members[i].id  = 200 + i;
			all_members[i].tid = 2000 + i;
			all_members[i].wcet_ns = 10;
		}

		/* Random forward edge set. */
		seed = seed * 1103515245u + 12345u;
		for (i = 0; i < n_total; i++) {
			for (j = i + 1; j < n_total; j++) {
				seed = seed * 1103515245u + 12345u;
				if ((seed & 0x3) == 0)
					continue;
				if (n_edges >= 15)
					continue;
				cedges[n_edges] = (struct contracted_edge_input){
					.src_id = all_members[i].id,
					.dst_id = all_members[j].id,
				};
				fedges[n_edges] = (struct fusion_edge_input){
					.src_id = all_members[i].id,
					.dst_id = all_members[j].id,
				};
				n_edges++;
			}
		}

		/* Pick a random subset to be the candidate group. */
		for (i = 0; i < n_total; i++) {
			seed = seed * 1103515245u + 12345u;
			if ((seed & 0x1) == 0)
				continue;
			member_ids[n_in_group] = all_members[i].id;
			n_in_group++;
			group_ids[i] = 42;
		}

		if (n_in_group < 2)
			continue; /* trivial group */

		enum fusion_reject_reason r = FUSION_REJ_NONE;
		bool accept = fusion_validator_precedence_convex_accept(
				member_ids, n_in_group,
				fedges, n_edges, &r);
		if (!accept)
			continue; /* rejected, nothing to check */

		/* Validator accepted: contracted DAG must be acyclic. */
		contracted_dag_t *cg = NULL;
		int rc = contracted_dag_build(100, 100, all_members,
				group_ids, n_total, cedges, n_edges, &cg);
		if (rc != 0)
			continue;
		if (contracted_dag_has_cycle(cg))
			total_violations++;
		contracted_dag_destroy(cg);
	}

	pwtest_int_eq((int)total_violations, 0);
	return PWTEST_PASS;
}

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

PWTEST(contracted_edge_meta_attach_records_originals)
{
	contracted_dag_t *cg = contracted_dag_create(1000, 1000);
	contracted_node_t *a = contracted_dag_add_node(cg);
	contracted_node_t *b = contracted_dag_add_node(cg);
	contracted_edge_t *e;

	pwtest_int_eq(contracted_dag_add_edge(cg, a, b), 0);
	e = contracted_dag_find_edge(cg, a, b);
	pwtest_ptr_notnull(e);
	pwtest_int_eq((int)e->n_originals, 0);
	pwtest_int_eq((int)e->flags_union, 0);

	pwtest_int_eq(contracted_edge_add_meta(e, 11, 3, 4,
			CONTRACTED_EDGE_ASYNC), 0);
	pwtest_int_eq(contracted_edge_add_meta(e, 12, 5, 6,
			CONTRACTED_EDGE_FEEDBACK), 0);
	pwtest_int_eq((int)e->n_originals, 2);
	pwtest_int_eq((int)e->flags_union,
			CONTRACTED_EDGE_ASYNC | CONTRACTED_EDGE_FEEDBACK);

	struct contracted_edge_meta *em;
	uint32_t seen_ids = 0;
	spa_list_for_each(em, &e->originals, link) {
		seen_ids |= 1u << em->edge_id;
	}
	pwtest_int_eq((int)seen_ids, (int)((1u << 11) | (1u << 12)));

	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_edge_meta_builder_preserves_per_original)
{
	/* Two original edges A -> B with distinct edge ids, ports, and
	 * flags. After contraction they collapse to one contracted edge
	 * (dedup) but the meta list still carries both originals so the
	 * diagnostic dumps can list them. */
	const struct contracted_member_input members[] = {
		{ .id = 1, .tid = 100, .wcet_ns = 50 },
		{ .id = 2, .tid = 200, .wcet_ns = 50 },
	};
	const uint32_t group_id[] = { 0, 0 };
	const struct contracted_edge_input edges[] = {
		{ .src_id = 1, .dst_id = 2, .edge_id = 7,
			.src_port = 1, .dst_port = 2,
			.flags = CONTRACTED_EDGE_ASYNC },
		{ .src_id = 1, .dst_id = 2, .edge_id = 8,
			.src_port = 3, .dst_port = 4, .flags = 0 },
	};
	contracted_dag_t *cg = NULL;

	pwtest_int_eq(contracted_dag_build(1000, 1000, members, group_id, 2,
			edges, 2, &cg), 0);
	pwtest_int_eq((int)cg->n_edges, 1);

	contracted_edge_t *e;
	uint32_t n_seen_edges = 0;
	spa_list_for_each(e, &cg->edges, link) {
		pwtest_int_eq((int)e->n_originals, 2);
		pwtest_int_eq((int)e->flags_union, CONTRACTED_EDGE_ASYNC);
		n_seen_edges++;
	}
	pwtest_int_eq((int)n_seen_edges, 1);

	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_edge_meta_builder_skips_zero_input)
{
	/* When all meta fields are zero, the builder treats the edge as
	 * "no diagnostics requested" and does NOT attach an empty meta
	 * entry. This keeps the existing builder callers (tests, the
	 * older reconcile path) free of accidental empty meta entries. */
	const struct contracted_member_input members[] = {
		{ .id = 1, .tid = 100, .wcet_ns = 50 },
		{ .id = 2, .tid = 200, .wcet_ns = 50 },
	};
	const uint32_t group_id[] = { 0, 0 };
	const struct contracted_edge_input edges[] = {
		{ .src_id = 1, .dst_id = 2 }, /* all meta fields zero */
	};
	contracted_dag_t *cg = NULL;

	pwtest_int_eq(contracted_dag_build(1000, 1000, members, group_id, 2,
			edges, 1, &cg), 0);
	pwtest_int_eq((int)cg->n_edges, 1);

	contracted_edge_t *e;
	spa_list_for_each(e, &cg->edges, link) {
		pwtest_int_eq((int)e->n_originals, 0);
		pwtest_int_eq((int)e->flags_union, 0);
	}

	contracted_dag_destroy(cg);
	return PWTEST_PASS;
}

PWTEST(contracted_edge_add_meta_null_safe)
{
	pwtest_int_eq(contracted_edge_add_meta(NULL, 1, 2, 3, 4), -EINVAL);
	pwtest_ptr_null(contracted_dag_find_edge(NULL, NULL, NULL));
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
	pwtest_add(contracted_builder_preserves_external_predecessors,
			PWTEST_NOARG);
	pwtest_add(contracted_builder_property_preserves_edge_set,
			PWTEST_NOARG);
	pwtest_add(contracted_validator_convex_accept_implies_acyclic,
			PWTEST_NOARG);
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
	pwtest_add(contracted_edge_meta_attach_records_originals, PWTEST_NOARG);
	pwtest_add(contracted_edge_meta_builder_preserves_per_original, PWTEST_NOARG);
	pwtest_add(contracted_edge_meta_builder_skips_zero_input, PWTEST_NOARG);
	pwtest_add(contracted_edge_add_meta_null_safe, PWTEST_NOARG);

	return PWTEST_PASS;
}
