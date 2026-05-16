/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_FUSION_GRAPH_H
#define PIPEWIRE_FUSION_GRAPH_H

/*
 * fusion-graph: topology-shaped helper used by the subgraph-fusion
 * pass in context.c.
 *
 * Why a separate translation unit
 * -------------------------------
 *
 * The fusion decision involves three distinct algorithmic phases:
 *
 *   1. Build a dense view of the eligible nodes (filter against the
 *      remote/exported/driver/main-loop exclusions in context.c).
 *   2. Partition the view into weakly connected components by BFS
 *      over eligible edges in both directions.
 *   3. For each component, run a Kahn-topological sort + DP for
 *      the longest weighted path (cp_wcet, cp_hops), then apply
 *      the Sarkar 1989 §5.3 internalisation profitability criterion
 *      via pw_fusion_decide() (fusion-cost.h).
 *
 * Phases 2 and 3 are purely combinatorial: they consume id+wcet
 * tuples and (src, dst) edge tuples and produce per-component
 * verdicts. They have no dependency on pw_impl_node, pw_loop, or any
 * other PipeWire runtime object. Pulling them out lets unit tests
 * drive the full pipeline with synthetic graphs -- topologies, WCETs,
 * and edges constructed in-test -- without having to bring up a
 * pw_context. The context.c side is reduced to:
 *
 *   - building a fusion_graph by iterating pw_context::node_list and
 *     calling fusion_graph_add_node / fusion_graph_add_edge;
 *   - calling fusion_graph_evaluate() to get a per-node decision;
 *   - applying the per-node decision by stamping the
 *     PW_KEY_NODE_LOOP_GROUP property and relocating via
 *     pw_impl_node_set_data_loop().
 *
 * Citations
 * ---------
 *
 * Sarkar 1989 (chapter 5.3) -- profitability criterion. Gerasoulis &
 * Yang 1993 -- linear-cluster non-pessimism (the chain fallback
 * applied during the warm-up). Shi et al. RTAS 2024 -- execution
 * group terminology. See fusion-cost.h for the formal statements
 * and DOIs.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fusion-cost.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle. Allocated by fusion_graph_alloc, freed by
 * fusion_graph_free. */
struct pw_fusion_graph;

/* Allocate a fusion graph with capacity for `n_nodes_hint` nodes and
 * `n_edges_hint` edges. The hints are just initial allocations;
 * fusion_graph_add_node / fusion_graph_add_edge grow geometrically on
 * overflow. Returns NULL on ENOMEM. */
struct pw_fusion_graph *pw_fusion_graph_alloc(uint32_t n_nodes_hint,
		uint32_t n_edges_hint);

/* Free everything allocated by pw_fusion_graph_alloc. Safe on NULL. */
void pw_fusion_graph_free(struct pw_fusion_graph *g);

/* Reset to empty (0 nodes, 0 edges) without freeing the backing
 * arrays. Used by context.c to reuse the graph across recalc passes
 * (the per-call allocation cost is amortised across recalcs). */
void pw_fusion_graph_reset(struct pw_fusion_graph *g);

/* Add a node to the graph. `id` is the caller's identifier (typically
 * pw_impl_node::info.id); `wcet_ns` is the smoothed per-node CPU-time
 * estimate; `samples` is the EMA's sample count (used by the warm-up
 * gate in pw_fusion_decide).
 *
 * Returns the dense index assigned to the node (>=0), or -ENOMEM on
 * allocation failure, or -EEXIST if the id was already added. The
 * dense index is the value the caller will get back in the decision
 * output array, plus the value used for fusion_graph_add_edge. */
int pw_fusion_graph_add_node(struct pw_fusion_graph *g, uint32_t id,
		uint64_t wcet_ns, uint32_t samples);

/* Override the previous decision the cost model will see for the
 * component containing the given node. Used by the hysteresis path:
 * the caller records each component's last applied decision (keyed
 * by the leader's id) and supplies it on the next scan so the model
 * can suppress flip-flops in the band around Sarkar's threshold.
 *
 * The convention is to set it on every node that participated in
 * the previous scan; the leader of the new component aggregates
 * (any member's stored prev_decision agrees with the leader's
 * since the previous scan applied one decision per component).
 *
 * Returns 0 on success, -EINVAL on out-of-range index. */
int pw_fusion_graph_set_prev_decision(struct pw_fusion_graph *g,
		uint32_t node_idx, enum pw_fusion_decision prev);

/* Add a directed edge from `src_idx` to `dst_idx` (indices returned
 * by fusion_graph_add_node). Returns 0 on success, -EINVAL on
 * out-of-range indices, -ENOMEM on allocation failure. Duplicate
 * edges are accepted (the BFS / DP handle them idempotently). */
int pw_fusion_graph_add_edge(struct pw_fusion_graph *g,
		uint32_t src_idx, uint32_t dst_idx);

/* Output decision for one node, returned by fusion_graph_evaluate. */
struct pw_fusion_graph_decision {
	enum pw_fusion_decision decision;
	uint32_t component_leader_id; /* the smallest pw_fusion_graph node
	                               * id sharing this component (the
	                               * canonical group name suffix) */
	uint32_t component_n_nodes;
	uint64_t component_sum_wcet_ns;
	uint64_t component_cp_wcet_ns;
	uint32_t component_cp_hops;
	uint32_t component_min_samples;
	/* Aggregated prior decision the cost model saw for this
	 * component (max over members' prev_decision hints). Exposed
	 * so the caller can confirm what the hysteresis path was
	 * told. */
	enum pw_fusion_decision prev_decision;
};

/* Run the full pipeline: weakly-connected partition, per-component
 * critical-path DP, per-component Sarkar criterion.
 *
 * `out` is an array of length >= g->n_nodes. On return out[i] holds
 * the decision for the node added at index i, plus the per-component
 * aggregates the caller may want to log (component_*).
 *
 * Returns 0 on success, -ELOOP if a cycle is detected in any
 * component (defensive -- the caller should already have filtered
 * feedback edges), -ENOMEM on allocation failure. */
int pw_fusion_graph_evaluate(struct pw_fusion_graph *g,
		const struct pw_fusion_params *params,
		struct pw_fusion_graph_decision *out);

/* Introspection helpers for tests. */
uint32_t pw_fusion_graph_n_nodes(const struct pw_fusion_graph *g);
uint32_t pw_fusion_graph_n_edges(const struct pw_fusion_graph *g);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_FUSION_GRAPH_H */
