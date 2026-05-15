/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * End-to-end unit tests for src/pipewire/fusion-graph.[ch].
 *
 * The fusion-graph module owns the BFS partitioning, the Kahn
 * topological + DP critical-path computation, and the per-component
 * application of pw_fusion_decide(). Driving it from a test lets us
 * exercise the *full* fusion pipeline -- not just the criterion
 * (which test-fusion-cost.c already pins) -- against synthetic
 * topologies with controllable per-node WCETs, with no PipeWire
 * runtime objects required.
 *
 * Each test builds a topology, calls pw_fusion_graph_evaluate(), and
 * inspects the per-node decision. The dynamic-WCET tests then update
 * the per-node WCET (by re-creating the graph with new values, since
 * pw_fusion_graph has no "edit in place" API) and re-evaluate, which
 * is exactly how context.c will see runtime updates from cycle to
 * cycle.
 *
 * Citations for the criterion (Sarkar 1989, Gerasoulis-Yang 1993,
 * Shi 2024) live in fusion-cost.h / fusion-graph.h.
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pwtest.h"

#include "../src/pipewire/fusion-graph.h"

/* --------------------------------------------------------------------- */
/* Allocation / reset / introspection                                     */
/* --------------------------------------------------------------------- */

PWTEST(fg_alloc_returns_empty_graph)
{
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(0, 0);
	pwtest_ptr_notnull(g);
	pwtest_int_eq((int)pw_fusion_graph_n_nodes(g), 0);
	pwtest_int_eq((int)pw_fusion_graph_n_edges(g), 0);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST(fg_free_null_safe)
{
	pw_fusion_graph_free(NULL);
	pw_fusion_graph_reset(NULL);
	return PWTEST_PASS;
}

PWTEST(fg_add_node_returns_dense_index)
{
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(4, 4);
	pwtest_int_eq(pw_fusion_graph_add_node(g, 100, 1000, 10), 0);
	pwtest_int_eq(pw_fusion_graph_add_node(g, 101, 2000, 20), 1);
	pwtest_int_eq(pw_fusion_graph_add_node(g, 102, 3000, 30), 2);
	pwtest_int_eq((int)pw_fusion_graph_n_nodes(g), 3);

	/* Duplicate id -> EEXIST, no advance. */
	pwtest_int_eq(pw_fusion_graph_add_node(g, 101, 9999, 99), -EEXIST);
	pwtest_int_eq((int)pw_fusion_graph_n_nodes(g), 3);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST(fg_add_edge_validates_indices)
{
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(4, 4);
	pw_fusion_graph_add_node(g, 10, 1000, 100);
	pw_fusion_graph_add_node(g, 11, 1000, 100);
	pwtest_int_eq(pw_fusion_graph_add_edge(g, 0, 1), 0);
	pwtest_int_eq(pw_fusion_graph_add_edge(g, 1, 5), -EINVAL); /* OOB */
	pwtest_int_eq(pw_fusion_graph_add_edge(g, 5, 0), -EINVAL);
	pwtest_int_eq(pw_fusion_graph_add_edge(g, 0, 0), -EINVAL); /* self */
	pwtest_int_eq((int)pw_fusion_graph_n_edges(g), 1);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST(fg_reset_clears_counts_keeps_capacity)
{
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(8, 8);
	pw_fusion_graph_add_node(g, 1, 100, 100);
	pw_fusion_graph_add_node(g, 2, 100, 100);
	pw_fusion_graph_add_edge(g, 0, 1);
	pw_fusion_graph_reset(g);
	pwtest_int_eq((int)pw_fusion_graph_n_nodes(g), 0);
	pwtest_int_eq((int)pw_fusion_graph_n_edges(g), 0);
	/* Can reuse after reset. */
	pwtest_int_eq(pw_fusion_graph_add_node(g, 1, 100, 100), 0);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

/* --------------------------------------------------------------------- */
/* Topology helpers                                                       */
/* --------------------------------------------------------------------- */

/* Build a chain: source -> n1 -> n2 -> ... -> sink (length = `len`).
 * Returns the graph; per-node WCET is `wcet`, samples is `samples`. */
static struct pw_fusion_graph *make_chain(uint32_t len, uint64_t wcet,
		uint32_t samples)
{
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(len, len);
	uint32_t i;
	for (i = 0; i < len; i++)
		pw_fusion_graph_add_node(g, 1000 + i, wcet, samples);
	for (i = 0; i + 1 < len; i++)
		pw_fusion_graph_add_edge(g, i, i + 1);
	return g;
}

/* Build a Y-shape: source -> {a, b} -> sink. WCETs are per-node. */
static struct pw_fusion_graph *make_y_shape(uint64_t w_source, uint64_t w_a,
		uint64_t w_b, uint64_t w_sink, uint32_t samples)
{
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(4, 4);
	pw_fusion_graph_add_node(g, 1, w_source, samples);    /* idx 0 */
	pw_fusion_graph_add_node(g, 2, w_a,      samples);    /* idx 1 */
	pw_fusion_graph_add_node(g, 3, w_b,      samples);    /* idx 2 */
	pw_fusion_graph_add_node(g, 4, w_sink,   samples);    /* idx 3 */
	pw_fusion_graph_add_edge(g, 0, 1);
	pw_fusion_graph_add_edge(g, 0, 2);
	pw_fusion_graph_add_edge(g, 1, 3);
	pw_fusion_graph_add_edge(g, 2, 3);
	return g;
}

/* --------------------------------------------------------------------- */
/* Pure-topology evaluation tests                                         */
/* --------------------------------------------------------------------- */

PWTEST(fg_eval_chain_fuses)
{
	/* Three-node chain of uniform 1000 ns WCETs:
	 *   sum_wcet = 3000, cp_wcet = 3000, cp_hops = 2.
	 *   budget   = 3000 + 2*3000 = 9000.
	 *   sum_wcet <= budget => FUSE. */
	struct pw_fusion_graph *g = make_chain(3, 1000, 100);
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision out[3];
	int rc = pw_fusion_graph_evaluate(g, &p, out);
	pwtest_int_eq(rc, 0);
	pwtest_int_eq((int)out[0].decision, (int)PW_FUSION_DECISION_FUSE);
	pwtest_int_eq((int)out[1].decision, (int)PW_FUSION_DECISION_FUSE);
	pwtest_int_eq((int)out[2].decision, (int)PW_FUSION_DECISION_FUSE);

	/* Every node sees the same component leader (the smallest id =
	 * 1000), so the group name -- "fusion.1000" -- is identical for
	 * all three members. Pin the leader id directly. */
	pwtest_int_eq((int)out[0].component_leader_id, 1000);
	pwtest_int_eq((int)out[1].component_leader_id, 1000);
	pwtest_int_eq((int)out[2].component_leader_id, 1000);

	/* Pin the aggregate fields: a length-3 chain of uniform 1000 ns
	 * has sum_wcet = 3000, cp_wcet = 3000, cp_hops = 2. */
	pwtest_int_eq((int)out[0].component_n_nodes, 3);
	pwtest_int_eq((int)out[0].component_sum_wcet_ns, 3000);
	pwtest_int_eq((int)out[0].component_cp_wcet_ns, 3000);
	pwtest_int_eq((int)out[0].component_cp_hops, 2);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST(fg_eval_y_shape_cheap_fuses)
{
	struct pw_fusion_graph *g = make_y_shape(1000, 1000, 1000, 1000, 100);
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision out[4];
	int rc = pw_fusion_graph_evaluate(g, &p, out);
	pwtest_int_eq(rc, 0);
	pwtest_int_eq((int)out[0].decision, (int)PW_FUSION_DECISION_FUSE);
	pwtest_int_eq((int)out[3].decision, (int)PW_FUSION_DECISION_FUSE);

	pwtest_int_eq((int)out[0].component_n_nodes, 4);
	pwtest_int_eq((int)out[0].component_sum_wcet_ns, 4000);
	/* cp = source + max(a,b) + sink = 1000 + 1000 + 1000 = 3000;
	 * cp_hops = 2. */
	pwtest_int_eq((int)out[0].component_cp_wcet_ns, 3000);
	pwtest_int_eq((int)out[0].component_cp_hops, 2);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST(fg_eval_y_shape_heavy_arms_linear_only)
{
	/* Two heavy parallel branches: classic "do not fuse" case.
	 * source=100, a=10000, b=10000, sink=100.
	 *   sum = 20200, cp = 10200 (source + heavier + sink),
	 *   cp_hops = 2, budget = 10200 + 6000 = 16200.
	 *   sum (20200) > budget (16200) => LINEAR_ONLY. */
	struct pw_fusion_graph *g = make_y_shape(100, 10000, 10000, 100, 100);
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision out[4];
	pw_fusion_graph_evaluate(g, &p, out);
	pwtest_int_eq((int)out[0].decision,
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	pwtest_int_eq((int)out[1].decision,
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST(fg_eval_two_disjoint_components_independent)
{
	/* Two unconnected chains. Each evaluated independently. */
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(6, 4);
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision out[6];

	pw_fusion_graph_add_node(g, 10, 1000, 100); /* idx 0 */
	pw_fusion_graph_add_node(g, 11, 1000, 100); /* idx 1 */
	pw_fusion_graph_add_node(g, 12, 1000, 100); /* idx 2 */
	pw_fusion_graph_add_edge(g, 0, 1);
	pw_fusion_graph_add_edge(g, 1, 2);

	/* Disconnected second component: two heavy parallel nodes,
	 * no edges connecting back to the first chain. */
	pw_fusion_graph_add_node(g, 20, 10000, 100); /* idx 3 */
	pw_fusion_graph_add_node(g, 21, 10000, 100); /* idx 4 */
	pw_fusion_graph_add_node(g, 22, 100,   100); /* idx 5 sink */
	pw_fusion_graph_add_edge(g, 3, 5);
	pw_fusion_graph_add_edge(g, 4, 5);

	pw_fusion_graph_evaluate(g, &p, out);
	/* First chain: chain criterion holds. */
	pwtest_int_eq((int)out[0].decision, (int)PW_FUSION_DECISION_FUSE);
	pwtest_int_eq((int)out[0].component_leader_id, 10);
	pwtest_int_eq((int)out[0].component_n_nodes, 3);

	/* Second component: heavy parallel arms = LINEAR_ONLY. */
	pwtest_int_eq((int)out[3].decision,
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	pwtest_int_eq((int)out[3].component_leader_id, 20);
	pwtest_int_eq((int)out[3].component_n_nodes, 3);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST(fg_eval_singleton_splits)
{
	/* A single-node component has nothing to fuse. */
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(1, 0);
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision out[1];
	pw_fusion_graph_add_node(g, 1, 1000, 100);
	pw_fusion_graph_evaluate(g, &p, out);
	pwtest_int_eq((int)out[0].decision, (int)PW_FUSION_DECISION_SPLIT);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST(fg_eval_warmup_falls_back_to_linear_only)
{
	/* A 3-node chain whose members are still warming up
	 * (samples < min_samples). Even though the criterion would
	 * otherwise fire, we defer to LINEAR_ONLY until the EMA has
	 * accumulated at least min_samples observations per node. */
	struct pw_fusion_graph *g = make_chain(3, 1000, 1);
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision out[3];
	pw_fusion_graph_evaluate(g, &p, out);
	pwtest_int_eq((int)out[0].decision,
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

/* --------------------------------------------------------------------- */
/* "Merging is actually convenient" topologies                            */
/* --------------------------------------------------------------------- */

PWTEST(fg_eval_realistic_4channel_mixer_fuses)
{
	/* Four cheap sources -> mixer -> sink. The mixer is the
	 * fan-in vertex with cp_hops = 2 (source -> mixer -> sink).
	 * 6 nodes, all cheap; the merged thread executes them
	 * serially which is cheaper than 5 cross-thread wakes. */
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(8, 8);
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision out[6];
	uint32_t i;
	int idx_mix, idx_sink;
	for (i = 0; i < 4; i++)
		pw_fusion_graph_add_node(g, 100 + i, 1000, 100); /* 4 sources */
	idx_mix = pw_fusion_graph_add_node(g, 200, 1000, 100);
	idx_sink = pw_fusion_graph_add_node(g, 300, 1000, 100);
	for (i = 0; i < 4; i++)
		pw_fusion_graph_add_edge(g, i, (uint32_t)idx_mix);
	pw_fusion_graph_add_edge(g, (uint32_t)idx_mix, (uint32_t)idx_sink);
	pw_fusion_graph_evaluate(g, &p, out);
	pwtest_int_eq((int)out[0].decision, (int)PW_FUSION_DECISION_FUSE);
	pwtest_int_eq((int)out[idx_mix].decision, (int)PW_FUSION_DECISION_FUSE);
	pwtest_int_eq((int)out[idx_sink].decision, (int)PW_FUSION_DECISION_FUSE);
	pwtest_int_eq((int)out[0].component_n_nodes, 6);
	pwtest_int_eq((int)out[0].component_sum_wcet_ns, 6000);
	/* cp = source + mixer + sink = 3000, hops = 2. */
	pwtest_int_eq((int)out[0].component_cp_wcet_ns, 3000);
	pwtest_int_eq((int)out[0].component_cp_hops, 2);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST(fg_eval_long_chain_with_one_bump_fuses)
{
	/* Five-node chain where node 2 is somewhat heavier than the
	 * others. cp_wcet = sum_wcet (chain identity), cp_hops = 4,
	 * Sarkar's criterion: 0 <= 4 * W => always satisfied. */
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(5, 4);
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision out[5];
	pw_fusion_graph_add_node(g, 10, 1000, 100);
	pw_fusion_graph_add_node(g, 11, 1000, 100);
	pw_fusion_graph_add_node(g, 12, 5000, 100);  /* the bump */
	pw_fusion_graph_add_node(g, 13, 1000, 100);
	pw_fusion_graph_add_node(g, 14, 1000, 100);
	pw_fusion_graph_add_edge(g, 0, 1);
	pw_fusion_graph_add_edge(g, 1, 2);
	pw_fusion_graph_add_edge(g, 2, 3);
	pw_fusion_graph_add_edge(g, 3, 4);
	pw_fusion_graph_evaluate(g, &p, out);
	pwtest_int_eq((int)out[0].decision, (int)PW_FUSION_DECISION_FUSE);
	pwtest_int_eq((int)out[2].decision, (int)PW_FUSION_DECISION_FUSE);
	pwtest_int_eq((int)out[4].decision, (int)PW_FUSION_DECISION_FUSE);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST(fg_eval_binary_tree_mixer_fuses)
{
	/* Binary fan-in tree: 4 sources -> 2 sub-mixers -> 1 master
	 * mixer -> sink, totalling 8 nodes. cp_hops = 3, sum = 8000,
	 * cp = 4000. budget = 4000 + 9000 = 13000 -> FUSE. */
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(10, 10);
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision out[8];
	uint32_t i;
	for (i = 0; i < 4; i++)
		pw_fusion_graph_add_node(g, 100 + i, 1000, 100); /* 0..3 */
	pw_fusion_graph_add_node(g, 200, 1000, 100); /* sub-mixer L = 4 */
	pw_fusion_graph_add_node(g, 201, 1000, 100); /* sub-mixer R = 5 */
	pw_fusion_graph_add_node(g, 300, 1000, 100); /* master = 6 */
	pw_fusion_graph_add_node(g, 400, 1000, 100); /* sink = 7 */
	pw_fusion_graph_add_edge(g, 0, 4);
	pw_fusion_graph_add_edge(g, 1, 4);
	pw_fusion_graph_add_edge(g, 2, 5);
	pw_fusion_graph_add_edge(g, 3, 5);
	pw_fusion_graph_add_edge(g, 4, 6);
	pw_fusion_graph_add_edge(g, 5, 6);
	pw_fusion_graph_add_edge(g, 6, 7);
	pw_fusion_graph_evaluate(g, &p, out);
	for (i = 0; i < 8; i++)
		pwtest_int_eq((int)out[i].decision,
				(int)PW_FUSION_DECISION_FUSE);
	pwtest_int_eq((int)out[0].component_n_nodes, 8);
	pwtest_int_eq((int)out[0].component_sum_wcet_ns, 8000);
	pwtest_int_eq((int)out[0].component_cp_wcet_ns, 4000);
	pwtest_int_eq((int)out[0].component_cp_hops, 3);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

/* --------------------------------------------------------------------- */
/* Dynamic transitions: same topology, changing WCETs                     */
/* --------------------------------------------------------------------- */

PWTEST(fg_dynamic_y_flips_fuse_to_linear_to_fuse)
{
	/* Same Y-shape topology, evaluated three times with different
	 * per-node WCETs. The criterion FLIPS based on the WCETs the
	 * caller feeds; this is exactly what context.c will observe
	 * when fusion_runtime_ema evolves cycle-to-cycle. */
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision out[4];
	struct pw_fusion_graph *g;

	/* Phase 1: cheap -> FUSE. */
	g = make_y_shape(1000, 1000, 1000, 1000, 100);
	pw_fusion_graph_evaluate(g, &p, out);
	pwtest_int_eq((int)out[0].decision, (int)PW_FUSION_DECISION_FUSE);
	pw_fusion_graph_free(g);

	/* Phase 2: one heavy arm -> still FUSE (single heavy node sits
	 * on the critical path either way; sum stays under budget). */
	g = make_y_shape(1000, 1000, 50000, 1000, 100);
	pw_fusion_graph_evaluate(g, &p, out);
	pwtest_int_eq((int)out[0].decision, (int)PW_FUSION_DECISION_FUSE);
	pw_fusion_graph_free(g);

	/* Phase 3: BOTH arms heavy -> LINEAR_ONLY (sum doubles, cp
	 * barely moves, fusion would double the critical path). */
	g = make_y_shape(1000, 50000, 50000, 1000, 100);
	pw_fusion_graph_evaluate(g, &p, out);
	pwtest_int_eq((int)out[0].decision,
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	pw_fusion_graph_free(g);

	/* Phase 4: back to baseline -> FUSE again. */
	g = make_y_shape(1000, 1000, 1000, 1000, 100);
	pw_fusion_graph_evaluate(g, &p, out);
	pwtest_int_eq((int)out[0].decision, (int)PW_FUSION_DECISION_FUSE);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST(fg_dynamic_chain_stays_fused_regardless)
{
	/* Chains are linear clusters; Gerasoulis-Yang 1993 says they
	 * are non-pessimistic regardless of WCET distribution. We
	 * confirm: same chain topology, sweeping its bump-node WCET
	 * from 1000 to 1e8 ns, never observes LINEAR_ONLY. */
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph *g;
	uint64_t bump;
	for (bump = 1000; bump <= 100000000ULL; bump *= 10) {
		struct pw_fusion_graph_decision out[3];
		g = pw_fusion_graph_alloc(3, 2);
		pw_fusion_graph_add_node(g, 100, 1000, 100);
		pw_fusion_graph_add_node(g, 101, bump, 100);
		pw_fusion_graph_add_node(g, 102, 1000, 100);
		pw_fusion_graph_add_edge(g, 0, 1);
		pw_fusion_graph_add_edge(g, 1, 2);
		pw_fusion_graph_evaluate(g, &p, out);
		pwtest_int_eq((int)out[0].decision,
				(int)PW_FUSION_DECISION_FUSE);
		pwtest_int_eq((int)out[1].decision,
				(int)PW_FUSION_DECISION_FUSE);
		pwtest_int_eq((int)out[2].decision,
				(int)PW_FUSION_DECISION_FUSE);
		pw_fusion_graph_free(g);
	}
	return PWTEST_PASS;
}

PWTEST(fg_dynamic_threshold_crossing_is_monotonic)
{
	/* Symmetric Y-shape; sweep both arms' WCET from 1000 to 100000
	 * in 1000-ns steps; verify there is exactly ONE flip from
	 * FUSE to LINEAR_ONLY and no oscillation. */
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph *g;
	struct pw_fusion_graph_decision out[4];
	enum pw_fusion_decision last = PW_FUSION_DECISION_FUSE;
	int flips = 0;
	uint64_t w;
	for (w = 1000; w <= 100000; w += 1000) {
		g = make_y_shape(1000, w, w, 1000, 100);
		pw_fusion_graph_evaluate(g, &p, out);
		if (out[0].decision != last) {
			flips++;
			last = out[0].decision;
		}
		pw_fusion_graph_free(g);
	}
	pwtest_int_eq(flips, 1);
	pwtest_int_eq((int)last, (int)PW_FUSION_DECISION_LINEAR_ONLY);
	return PWTEST_PASS;
}

PWTEST(fg_dynamic_mixed_chain_and_fan_out)
{
	/* A "chain feeds a fan-out" topology. Two natural fusion
	 * partitions: (a) merge the whole thing (one execution group),
	 * (b) merge only the chain prefix.
	 *
	 * Topology:
	 *   s -> a -> b -> {c, d, e}
	 *
	 * With cheap nodes the whole thing fuses (5+ cheap members
	 * piled on one thread save 4 wakeups on cp_hops=2). With
	 * heavy parallel siblings (c, d, e all expensive) the
	 * cost model refuses fusion and falls back to linear
	 * sub-chains (the runner does that translation; the
	 * graph-level decision is LINEAR_ONLY for the whole
	 * component). */
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision out[5];
	struct pw_fusion_graph *g;

	/* Phase A: cheap -> FUSE. */
	g = pw_fusion_graph_alloc(5, 5);
	pw_fusion_graph_add_node(g, 10, 1000, 100); /* s */
	pw_fusion_graph_add_node(g, 11, 1000, 100); /* a */
	pw_fusion_graph_add_node(g, 12, 1000, 100); /* b (fan-out source) */
	pw_fusion_graph_add_node(g, 13, 1000, 100); /* c */
	pw_fusion_graph_add_node(g, 14, 1000, 100); /* d */
	pw_fusion_graph_add_edge(g, 0, 1);
	pw_fusion_graph_add_edge(g, 1, 2);
	pw_fusion_graph_add_edge(g, 2, 3);
	pw_fusion_graph_add_edge(g, 2, 4);
	pw_fusion_graph_evaluate(g, &p, out);
	pwtest_int_eq((int)out[0].decision, (int)PW_FUSION_DECISION_FUSE);
	pw_fusion_graph_free(g);

	/* Phase B: heavy siblings -> LINEAR_ONLY. */
	g = pw_fusion_graph_alloc(5, 5);
	pw_fusion_graph_add_node(g, 10, 1000, 100);
	pw_fusion_graph_add_node(g, 11, 1000, 100);
	pw_fusion_graph_add_node(g, 12, 1000, 100);
	pw_fusion_graph_add_node(g, 13, 50000, 100); /* heavy */
	pw_fusion_graph_add_node(g, 14, 50000, 100); /* heavy */
	pw_fusion_graph_add_edge(g, 0, 1);
	pw_fusion_graph_add_edge(g, 1, 2);
	pw_fusion_graph_add_edge(g, 2, 3);
	pw_fusion_graph_add_edge(g, 2, 4);
	pw_fusion_graph_evaluate(g, &p, out);
	pwtest_int_eq((int)out[0].decision,
			(int)PW_FUSION_DECISION_LINEAR_ONLY);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

/* --------------------------------------------------------------------- */
/* Robustness                                                             */
/* --------------------------------------------------------------------- */

PWTEST(fg_eval_zero_nodes_returns_zero)
{
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(0, 0);
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision out[1] = { { 0 } };
	pwtest_int_eq(pw_fusion_graph_evaluate(g, &p, out), 0);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST(fg_evaluate_null_inputs_einval)
{
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(2, 1);
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision out[1];
	pw_fusion_graph_add_node(g, 1, 1000, 100);
	pwtest_int_eq(pw_fusion_graph_evaluate(NULL, &p, out), -EINVAL);
	pwtest_int_eq(pw_fusion_graph_evaluate(g, NULL, out), -EINVAL);
	pwtest_int_eq(pw_fusion_graph_evaluate(g, &p, NULL), -EINVAL);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST(fg_eval_grows_geometrically)
{
	/* Add 100 nodes and 99 edges to confirm the geometric realloc
	 * path doesn't drop entries. */
	struct pw_fusion_graph *g = pw_fusion_graph_alloc(0, 0);
	uint32_t i;
	for (i = 0; i < 100; i++)
		pwtest_int_eq(pw_fusion_graph_add_node(g, 1000 + i, 1000, 100),
				(int)i);
	for (i = 0; i + 1 < 100; i++)
		pwtest_int_eq(pw_fusion_graph_add_edge(g, i, i + 1), 0);
	pwtest_int_eq((int)pw_fusion_graph_n_nodes(g), 100);
	pwtest_int_eq((int)pw_fusion_graph_n_edges(g), 99);

	/* Evaluate the giant chain: every member should FUSE. */
	struct pw_fusion_params p = { .wakeup_cost_ns = 3000, .min_samples = 4 };
	struct pw_fusion_graph_decision *out = calloc(100, sizeof(*out));
	pw_fusion_graph_evaluate(g, &p, out);
	for (i = 0; i < 100; i++)
		pwtest_int_eq((int)out[i].decision,
				(int)PW_FUSION_DECISION_FUSE);
	pwtest_int_eq((int)out[0].component_n_nodes, 100);
	pwtest_int_eq((int)out[0].component_cp_hops, 99);
	free(out);
	pw_fusion_graph_free(g);
	return PWTEST_PASS;
}

PWTEST_SUITE(fusion_graph)
{
	pwtest_add(fg_alloc_returns_empty_graph, PWTEST_NOARG);
	pwtest_add(fg_free_null_safe, PWTEST_NOARG);
	pwtest_add(fg_add_node_returns_dense_index, PWTEST_NOARG);
	pwtest_add(fg_add_edge_validates_indices, PWTEST_NOARG);
	pwtest_add(fg_reset_clears_counts_keeps_capacity, PWTEST_NOARG);
	pwtest_add(fg_eval_chain_fuses, PWTEST_NOARG);
	pwtest_add(fg_eval_y_shape_cheap_fuses, PWTEST_NOARG);
	pwtest_add(fg_eval_y_shape_heavy_arms_linear_only, PWTEST_NOARG);
	pwtest_add(fg_eval_two_disjoint_components_independent, PWTEST_NOARG);
	pwtest_add(fg_eval_singleton_splits, PWTEST_NOARG);
	pwtest_add(fg_eval_warmup_falls_back_to_linear_only, PWTEST_NOARG);
	pwtest_add(fg_eval_realistic_4channel_mixer_fuses, PWTEST_NOARG);
	pwtest_add(fg_eval_long_chain_with_one_bump_fuses, PWTEST_NOARG);
	pwtest_add(fg_eval_binary_tree_mixer_fuses, PWTEST_NOARG);
	pwtest_add(fg_dynamic_y_flips_fuse_to_linear_to_fuse, PWTEST_NOARG);
	pwtest_add(fg_dynamic_chain_stays_fused_regardless, PWTEST_NOARG);
	pwtest_add(fg_dynamic_threshold_crossing_is_monotonic, PWTEST_NOARG);
	pwtest_add(fg_dynamic_mixed_chain_and_fan_out, PWTEST_NOARG);
	pwtest_add(fg_eval_zero_nodes_returns_zero, PWTEST_NOARG);
	pwtest_add(fg_evaluate_null_inputs_einval, PWTEST_NOARG);
	pwtest_add(fg_eval_grows_geometrically, PWTEST_NOARG);
	return PWTEST_PASS;
}
