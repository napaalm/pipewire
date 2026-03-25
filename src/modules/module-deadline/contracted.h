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

	/* Macro-node completion timestamp for the most recent
	 * activation. The fused thread executes its members in
	 * topological order; the timestamp at which the LAST member
	 * finishes is the macro completion. Externally atomic groups
	 * expose exactly one such timestamp per cycle so downstream
	 * observers (a trace consumer, an external-successor
	 * activation tracker) see one rising edge per group activation,
	 * not one per member. The field is updated by the runtime hook
	 * after every cycle; analysis-layer code does not consume it.
	 *
	 * Stored in CLOCK_MONOTONIC nanoseconds for parity with the
	 * impl-node cycle-counter convention. Default 0 means "no
	 * cycle observed yet". */
	uint64_t macro_completion_ns;
	uint64_t macro_completion_count;

	/*
	 * Release-barrier state for the macro-node's per-cycle
	 * activation. required_external_inputs is the constant set at
	 * topology snapshot time: the count of distinct contracted-node
	 * predecessors that must complete the current period before the
	 * fused thread may begin its job. pending_external_inputs is the
	 * per-cycle decrementing counter the runtime drives:
	 *
	 *   armed at cycle start  -> pending = required
	 *   external pred completes -> pending--
	 *   pending == 0           -> wake fused thread
	 *
	 * For a singleton macro-node required equals the count of
	 * scheduling-DAG predecessors. For a fused group it is the size
	 * of external_pred(F) computed on the contracted DAG. A
	 * required of 0 (graph source) means the group wakes with the
	 * driver activation; no external waits required.
	 *
	 * Today's apply path uses the conservative strict-predecessor-
	 * closure validator to refuse groups that would self-suspend, so
	 * the per-cycle barrier is a no-op in production (every accepted
	 * group has required == size-of-external-pred and the existing
	 * activation already gates on those). The fields are populated
	 * and exercised in the unit tests so the runtime hook can wire
	 * them into impl-node's group-wake path without changing the
	 * data contract.
	 */
	uint32_t required_external_inputs;
	uint32_t pending_external_inputs;

	/* Structural flags. is_fusion_group is true iff n_members > 1
	 * (a singleton macro-node mirrors an un-fused original node).
	 * externally_atomic is the validator's verdict on whether
	 * the group exposes its output only at completion; reserved
	 * for the soundness validator that lands next. */
	bool is_fusion_group;
	bool externally_atomic;
};

/*
 * Per-original-edge metadata retained on a contracted edge.
 *
 * Edge contraction collapses every original edge with the same
 * (src_macro, dst_macro) pair into one contracted edge for analysis
 * (one edge in the deadline-splitter / placer input). The original
 * edges, however, may carry diagnostic context that the runtime
 * needs at fault-injection or trace time: which scheduling-DAG edge
 * the kernel signal traversed, the source / destination port ids on
 * the original nodes, and any classifier flags (async, feedback,
 * exported) that distinguish a back-pressure edge from a normal
 * data edge.
 *
 * The list of original edges that collapsed onto a contracted edge
 * is preserved here so the diagnostic dumps can surface "contracted
 * edge AB -> C carries N original edges, listed below" without
 * pretending the dedup never happened. The runtime analysis layer
 * does not consume this list; it exists for logging and tests.
 */
struct contracted_edge_meta {
	struct spa_list link;       /* link in contracted_edge::originals */
	uint32_t        edge_id;    /* opaque id assigned by caller; 0 if unknown */
	uint32_t        src_port;   /* originating port id; 0 if not modelled */
	uint32_t        dst_port;   /* terminating port id; 0 if not modelled */
	uint32_t        flags;      /* bitmask of contracted_edge_flag */
};

/*
 * Classification flags carried per original edge. Mirror the
 * scheduling-DAG edge classifier the diag layer already uses
 * (rt_diag_sched_excluded_reason) so the logging vocabulary stays
 * consistent. Only the kinds a contracted edge may legitimately
 * collapse from are enumerated here; edges classified as
 * cross-driver or unsupported are rejected upstream by the fusion
 * validator and never reach this layer.
 */
enum contracted_edge_flag {
	CONTRACTED_EDGE_ASYNC    = 1u << 0,
	CONTRACTED_EDGE_FEEDBACK = 1u << 1,
	CONTRACTED_EDGE_EXPORTED = 1u << 2,
};

/* Directed edge in the contracted DAG. */
struct contracted_edge {
	struct spa_list link;       /* link in contracted_dag::edges */
	contracted_node_t *src;
	contracted_node_t *dst;
	struct spa_list src_link;   /* link in src->succs */
	struct spa_list dst_link;   /* link in dst->preds */

	/* List of contracted_edge_meta describing every original edge
	 * that collapsed onto this contracted edge. The first call to
	 * contracted_dag_add_edge for a given (src, dst) pair appends
	 * no meta (callers may attach explicitly via
	 * contracted_edge_add_meta or pass meta through the builder);
	 * subsequent duplicate calls fold their meta entries in.
	 *
	 * The list aggregates a logical-OR of all per-original flags
	 * into `flags_union` so a single is-this-edge-async check
	 * does not have to walk the list. */
	struct spa_list originals;
	uint32_t        n_originals;
	uint32_t        flags_union;
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
 * allocation failure, -EINVAL on a self-loop (src == dst).
 *
 * No per-original metadata is attached by this entry point; callers
 * that need to preserve the original-edge identity for diagnostics
 * should call contracted_edge_add_meta() on the resulting edge or
 * pass meta through contracted_dag_build(). */
int contracted_dag_add_edge(contracted_dag_t *cg,
		contracted_node_t *src, contracted_node_t *dst);

/*
 * Attach one piece of original-edge metadata to a contracted edge.
 * Returns 0 on success, -EINVAL on NULL ce, -ENOMEM on allocation
 * failure. flags is OR'd into ce->flags_union; the meta entry is
 * appended to ce->originals and ce->n_originals is bumped. The
 * meta becomes owned by the contracted_edge and is freed by
 * contracted_dag_destroy.
 */
int contracted_edge_add_meta(contracted_edge_t *ce,
		uint32_t edge_id, uint32_t src_port, uint32_t dst_port,
		uint32_t flags);

/*
 * Convenience: look up an existing contracted edge between two
 * macro-nodes. Returns NULL if no edge exists. O(out-degree of src).
 */
contracted_edge_t *contracted_dag_find_edge(contracted_dag_t *cg,
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
	/* Optional diagnostic metadata preserved across contraction.
	 * Zero-initialised fields are treated as "not specified" by the
	 * meta layer (edge_id 0 means unknown caller id; ports 0 mean
	 * unmodelled; flags 0 means a plain in-period edge). */
	uint32_t edge_id;
	uint32_t src_port;
	uint32_t dst_port;
	uint32_t flags;        /* bitmask of contracted_edge_flag */
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

/*
 * Instrumentation-driven residual-overhead update.
 *
 * The fusion path does not ship a per-component microbench: the
 * dispatch / pending-update / port-iteration costs are interleaved
 * with member work in the fused thread and cannot be probed in
 * isolation without invasive instrumentation. Instead, the runtime
 * supplies the aggregate: a measured macro-node execution time
 * (already captured per cycle via CLOCK_THREAD_CPUTIME_ID in
 * impl-node and surfaced through the adaptive-conformal estimator).
 *
 * The residual is
 *
 *   residual_ns = max(0, observed_macro_runtime_ns - cn->wcet_ns)
 *
 * where cn->wcet_ns is the pre-fusion sum of member WCETs. The
 * residual is non-zero by construction whenever the fused thread
 * runs longer than the work it claims to bundle, which is the
 * empirical signal a fusion budget must carry: the dispatch cost
 * is bounded but not zero. The residual is attributed to
 * internal_topo_ns -- the catch-all for dispatch / pending
 * bookkeeping that we cannot decompose without probes -- so
 * contracted_node_set_overhead's additive aggregation keeps
 * overhead_ns honest.
 *
 * The function takes the max across calls: overhead is a worst-case
 * budget, not a moving average. Resetting requires a fresh
 * contracted_overhead_components passed to set_overhead.
 *
 * Returns 0 on success, -EINVAL on NULL cn. observed_macro_runtime_ns
 * == 0 returns 0 without mutation (no observation to fold in).
 */
int contracted_node_observe_macro_runtime(contracted_node_t *cn,
		uint64_t observed_macro_runtime_ns);

/*
 * Record one macro completion event. Called by the runtime hook
 * when the fused thread finishes its last in-cycle member: the
 * timestamp is the moment that final member's process() returned
 * (CLOCK_MONOTONIC ns). For a singleton macro-node this collapses
 * to the original-node completion time; for a fused chain it is
 * the chain tail's completion. Either way the macro-node now has
 * a single rising edge per cycle that downstream observers can
 * sync on without iterating members.
 *
 * The count is bumped on every call so an external-successor
 * tracker can detect a missed cycle (count gap). Successive calls
 * with non-monotonic timestamps are not rejected: the runtime
 * may emit out-of-order cycles in rare conditions (e.g. across a
 * topology generation flip); the cleaner observation strategy is
 * to compare against the previous count, not assume monotonicity.
 *
 * Returns 0 on success, -EINVAL on NULL cn.
 */
int contracted_node_observe_macro_completion(contracted_node_t *cn,
		uint64_t timestamp_ns);

/*
 * Release-barrier helpers.
 *
 * contracted_node_set_required_external_inputs assigns the
 * constant required count from the contracted-DAG predecessor list
 * (callers typically compute it as spa_list_count(&cn->preds) at
 * topology-snapshot time). The pending counter is reset to the
 * same value so the next cycle starts armed.
 *
 * contracted_node_arm_cycle resets pending to required at the
 * start of a new cycle.
 *
 * contracted_node_pred_completed decrements pending by one and
 * returns true iff pending has reached zero (i.e. the fused thread
 * is ready to be woken). Calling past zero is clamped at zero;
 * cn->pending_external_inputs may not become negative.
 *
 * contracted_node_ready_to_wake is the predicate variant: returns
 * true iff pending == 0 without mutating state. Useful in the
 * conservative strict-predecessor-closure path where the predicate
 * is read but the decrement is driven by a separate dependency
 * tracker.
 *
 * All four are NULL-safe (-EINVAL or false on NULL cn).
 */
int  contracted_node_set_required_external_inputs(contracted_node_t *cn,
		uint32_t required);
int  contracted_node_arm_cycle(contracted_node_t *cn);
bool contracted_node_pred_completed(contracted_node_t *cn);
bool contracted_node_ready_to_wake(const contracted_node_t *cn);

/*
 * Bridge between the contracted DAG and the existing scheduling-DAG
 * analysis. The analysis layer (dag_recalculate -> deadline split ->
 * worst-fit placement -> EDF feasibility) is the one place the
 * project pins its proofs; rewriting it on top of contracted_dag_t
 * directly would either duplicate the implementation or risk
 * divergence from the unit-tested baseline. The functions below
 * project the contracted DAG onto a synthesised dag_t -- one
 * dag_node per macro-node, carrying the macro-node's effective
 * WCET and leader TID -- so the existing splitter / placer /
 * feasibility checker operate on the macro-node task set without
 * change.
 *
 * Mapping convention:
 *   - dag_node::id    = contracted_node::id (the dense index
 *     contracted_dag_add_node assigned). Every macro-node id is
 *     unique within a contracted DAG, so the dag_t's id space stays
 *     consistent.
 *   - dag_node::wcet  = contracted_node_effective_wcet(macro)
 *                     = macro.wcet_ns + macro.overhead_ns.
 *   - dag_node::tid   = the leader member's tid (the lowest-id
 *     member when iterated). Used by the foreach callback to
 *     identify the macro-node's data-loop thread.
 *   - edges           = contracted edges, 1-to-1.
 *
 * The synthesised dag_t is owned by the caller and must be freed
 * with dag_destroy(). The contracted_dag_t is independently owned;
 * the bridge does not take a reference to it past the build call.
 */
struct dag;

/*
 * Build a fresh dag_t from a contracted DAG. The caller supplies
 * the global period / deadline (carried through from contracted_dag),
 * the admission ceiling and CPU configuration (forwarded to
 * dag_create), and an optional per-CPU relative_capacity vector.
 *
 * Returns 0 on success with *out pointing at the new dag_t.
 * Returns -EINVAL on null arguments, -ENOMEM on allocation failure,
 * or the dag_t API's error code on rejection (e.g. an invalid
 * admission ceiling, see dag_create). On failure *out is set to
 * NULL and any partially-built dag_t is destroyed.
 */
int contracted_dag_to_dag(const contracted_dag_t *cg,
		double admission_ceiling, uint32_t num_cpus,
		const double *relative_capacity,
		struct dag **out);

/*
 * Copy the per-macro-node scheduling parameters (cumulative_deadline,
 * local_deadline, cpu) from the recalculated dag_t back into the
 * contracted DAG. The dag_t must have been produced by
 * contracted_dag_to_dag() against the same contracted DAG and run
 * through dag_recalculate() since.
 *
 * On a missing macro-node id in the dag_t the field is left
 * untouched; this is intentional so a follow-up call can build a
 * partial schedule without erasing previously-populated values.
 *
 * Returns 0 on success, -EINVAL on null arguments.
 */
int contracted_dag_apply_dag_schedule(contracted_dag_t *cg,
		const struct dag *g);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEADLINE_CONTRACTED_H */
