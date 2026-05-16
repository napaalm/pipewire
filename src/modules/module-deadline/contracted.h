/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#ifndef MODULE_DEADLINE_CONTRACTED_H
#define MODULE_DEADLINE_CONTRACTED_H

/*
 * contracted.h
 *
 * Contracted-DAG data structure for module-deadline.
 *
 * Fusion is modelled as a graph contraction: for a fusion group F
 * the contracted DAG holds a single macro-node carrying the union
 * of the members' kernel-facing identity (one data-loop TID, one
 * SCHED_DEADLINE reservation). Internal edges between members
 * disappear; external edges are inherited by the macro-node and
 * deduplicated. The analysis pass (deadline split, CPU placement,
 * feasibility) consumes the contracted DAG so the schedule
 * computed there matches what the kernel actually executes.
 *
 * The terminology and algebra follow Sarkar 1989's chapter 5 on
 * macro-actor formation (the macro-actor's cost equals the
 * internal sum plus internal overhead, and its in/out edges are
 * inherited from members) and Cucinotta et al. 2024 on
 * P-EDF DAG placement, which formulates per-macro-node
 * (cumulative_deadline, local_deadline, cpu) as the optimisation
 * variables.
 *
 * Lifecycle: a contracted_dag_t is built from an original
 * scheduling DAG + an array of group assignments (one group id
 * per original node, with id 0 meaning "ungrouped, owns a
 * singleton macro-node"). It is consumed by the analysis layer
 * and freed when the caller no longer needs it.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#include <spa/utils/list.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct contracted_node contracted_node_t;
typedef struct contracted_edge contracted_edge_t;
typedef struct contracted_dag  contracted_dag_t;

/* Member entry inside a contracted node. Carries the original
 * scheduling DAG's per-node identity (id, tid, WCET) so the
 * downstream apply pass can write SCHED_DEADLINE to the right
 * thread and surface the member set for diagnostic output. */
struct contracted_member {
	struct spa_list link;       /* link in contracted_node::members */
	uint32_t id;
	pid_t    tid;
	uint64_t wcet_ns;
};

/*
 * Macro-node residual overhead decomposition.
 *
 * Aggregating N original members under one SCHED_DEADLINE
 * reservation does not eliminate every accounting cost: the
 * contracted thread must still dispatch internal work in
 * topological order, decrement an internal pending counter as
 * each member completes, walk its input / output ports per
 * member, and run any per-group bookkeeping the runtime
 * imposes. Each of those is small per-cycle but bounded; the
 * sum must enter the analysis's WCET budget for the macro-node
 * or the deadline split silently under-budgets the group.
 *
 * The fields below carry the per-component upper bound in
 * nanoseconds. The aggregated overhead used by the analysis is
 *
 *   overhead_ns = group_dispatch_ns + internal_topo_ns
 *               + activation_pending_ns + buffer_port_iter_ns
 *
 * `wakeup_savings_ns` is reported separately (so an operator can
 * see how much wake-up cost the fusion saves) but is NOT
 * subtracted from the analysis budget: pessimism in the budget is
 * safe, optimism is not. A future analysis variant that explicitly
 * models the saving can plug it in; today the field is
 * observational only.
 *
 * The breakdown follows Sarkar 1989's macro-actor accounting
 * (chapter 5): internal overhead of a macro-actor versus the
 * wake-up cost saved by collapsing successor edges.
 */
struct contracted_overhead_components {
	uint64_t group_dispatch_ns;
	uint64_t internal_topo_ns;
	uint64_t activation_pending_ns;
	uint64_t buffer_port_iter_ns;
	uint64_t wakeup_savings_ns;
};

/* Macro-node in the contracted DAG. */
struct contracted_node {
	struct spa_list link;       /* link in contracted_dag::nodes */
	uint32_t id;                /* dense index assigned at build time */

	struct spa_list members;    /* head of contracted_member list */
	uint32_t        n_members;

	/* External predecessor / successor edges. Owned by
	 * contracted_dag; the lists below link contracted_edge
	 * structures. */
	struct spa_list preds;      /* head of contracted_edge::dst_link */
	struct spa_list succs;      /* head of contracted_edge::src_link */

	/* Aggregated timing. wcet_ns is the sum of member WCETs;
	 * overhead_ns is the aggregated residual overhead populated
	 * by contracted_node_set_overhead() from the per-component
	 * breakdown; the two are tracked separately so a reader can
	 * distinguish work from book-keeping. The analysis layer's
	 * effective WCET for the macro-node is
	 * contracted_node_effective_wcet() = wcet_ns + overhead_ns. */
	uint64_t wcet_ns;
	uint64_t overhead_ns;
	struct contracted_overhead_components overhead;

	/* Scheduling parameters, populated by the analysis layer. */
	uint64_t runtime_budget_ns;
	uint64_t cumulative_deadline_ns;
	uint64_t local_deadline_ns;
	int      cpu;

	/* Structural flags. is_fusion_group is true iff n_members > 1
	 * (a singleton macro-node mirrors an un-fused original node).
	 * externally_atomic is the validator's verdict on whether
	 * the group exposes its output only at completion; reserved
	 * for the soundness validator that lands next. */
	bool is_fusion_group;
	bool externally_atomic;
};

/* Directed edge in the contracted DAG. */
struct contracted_edge {
	struct spa_list link;       /* link in contracted_dag::edges */
	contracted_node_t *src;
	contracted_node_t *dst;
	struct spa_list src_link;   /* link in src->succs */
	struct spa_list dst_link;   /* link in dst->preds */
};

/* The contracted DAG container. */
struct contracted_dag {
	struct spa_list nodes;      /* head of contracted_node::link */
	struct spa_list edges;      /* head of contracted_edge::link */
	uint32_t        n_nodes;
	uint32_t        n_edges;

	/* Global period / end-to-end deadline copied from the
	 * originating dag_t at build time. */
	uint64_t period_ns;
	uint64_t deadline_ns;
};

/* Allocate an empty contracted DAG with given global period and
 * end-to-end deadline. Returns NULL on ENOMEM. */
contracted_dag_t *contracted_dag_create(uint64_t period_ns, uint64_t deadline_ns);

/* Free everything owned by the contracted DAG. NULL-safe. */
void contracted_dag_destroy(contracted_dag_t *cg);

/* Append a fresh macro-node. The returned pointer remains valid
 * until contracted_dag_destroy. Returns NULL on ENOMEM. The
 * caller is expected to fill in the members and overhead before
 * the analysis layer reads the node. */
contracted_node_t *contracted_dag_add_node(contracted_dag_t *cg);

/* Append a member to the macro-node. Returns 0 on success, -ENOMEM
 * on allocation failure. wcet_ns is the member's individual
 * (denormalised, reference-CPU) WCET; the caller is responsible
 * for accumulating it into the macro-node's wcet_ns. */
int contracted_node_add_member(contracted_node_t *cn, uint32_t id,
		pid_t tid, uint64_t wcet_ns);

/* Add a directed edge between two contracted nodes. Duplicate
 * (src, dst) pairs are deduplicated: subsequent adds with the same
 * endpoints are no-ops. Returns 0 on success, -ENOMEM on
 * allocation failure, -EINVAL on a self-loop (src == dst). */
int contracted_dag_add_edge(contracted_dag_t *cg,
		contracted_node_t *src, contracted_node_t *dst);

/* Returns true if any directed cycle exists. The analysis layer
 * asserts acyclicity post-build and rejects the contracted DAG
 * otherwise -- a cyclic macro-graph would let the splitter loop
 * forever. */
bool contracted_dag_has_cycle(const contracted_dag_t *cg);

/*
 * Build a contracted DAG from a flat description of an original
 * scheduling graph plus a group assignment.
 *
 * Inputs:
 *   - period_ns, deadline_ns: global timing carried through
 *     unchanged on the output contracted_dag_t.
 *   - members[]: array of (id, tid, wcet_ns) tuples. Each entry
 *     describes one original scheduling node.
 *   - group_id[]: parallel to members[]. group_id[i] == 0 means
 *     "node i has no fusion group; it forms a singleton
 *     macro-node". Any non-zero group_id is a fusion-group
 *     identifier; nodes with the same non-zero group_id are
 *     placed in the same macro-node. The group ids are caller-
 *     defined; the builder only uses them for equivalence.
 *   - n_members: length of members[] and group_id[].
 *   - edges[]: array of (src_id, dst_id) tuples describing every
 *     original scheduling edge. Edges with both endpoints in the
 *     same macro-node are dropped as internal; every other edge
 *     becomes a contracted edge between the two distinct
 *     macro-nodes the endpoints belong to, deduplicated.
 *   - n_edges: length of edges[].
 *
 * Output:
 *   - On success, *out is a freshly-allocated contracted_dag_t
 *     whose nodes carry their member lists, their summed WCET
 *     (sum of members' wcet_ns; overhead_ns left at zero for the
 *     caller to update if a measurement is available), and the
 *     correctly-contracted edge set. Return value is 0.
 *
 * Failure modes:
 *   - -EINVAL on NULL outputs or on duplicate ids in members[].
 *   - -ENOMEM on allocation failure.
 *   - -ENOTRECOVERABLE when an edge references an id absent from
 *     members[] (a defensive guard against caller bugs; the
 *     builder cannot infer the missing macro-node).
 *
 * The builder does not run the cycle check; the caller is
 * expected to call contracted_dag_has_cycle() and tear the
 * contracted DAG down if it returns true.
 */
struct contracted_member_input {
	uint32_t id;
	pid_t    tid;
	uint64_t wcet_ns;
};

struct contracted_edge_input {
	uint32_t src_id;
	uint32_t dst_id;
};

int contracted_dag_build(uint64_t period_ns, uint64_t deadline_ns,
		const struct contracted_member_input *members,
		const uint32_t *group_id,
		uint32_t n_members,
		const struct contracted_edge_input *edges,
		uint32_t n_edges,
		contracted_dag_t **out);

/*
 * Replace the overhead breakdown on a macro-node and recompute the
 * aggregated overhead_ns the analysis layer reads. Pass a fresh
 * components struct (set unobserved fields to 0); this is the only
 * supported way to mutate cn->overhead so the wcet/overhead split
 * stays consistent. Returns 0 on success, -EINVAL on NULL cn or
 * components.
 *
 * The aggregation is intentionally additive over every component
 * other than wakeup_savings_ns: that field is reported back to the
 * operator but never subtracted from the analysis budget so the
 * fused-thread reservation is never sized smaller than the work
 * actually requires.
 */
int contracted_node_set_overhead(contracted_node_t *cn,
		const struct contracted_overhead_components *components);

/* Effective WCET (wcet_ns + overhead_ns) the analysis layer uses
 * as the macro-node's runtime budget input. Returns 0 on NULL. */
uint64_t contracted_node_effective_wcet(const contracted_node_t *cn);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEADLINE_CONTRACTED_H */
