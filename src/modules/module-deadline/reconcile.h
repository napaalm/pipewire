/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

#ifndef RECONCILE_H
#define RECONCILE_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#include "dag.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reconcile helpers for module-deadline.
 *
 * The reconcile layer sits between module-deadline's topology
 * snapshot (taken on the main loop, consumed by the deadline-recalc
 * worker thread) and the DAG library that produces SCHED_DEADLINE
 * parameters. It owns the persistent DAG state, the per-recalc
 * dirty bookkeeping, and the orchestration of the four reconcile
 * phases:
 *
 *   1. period   -- mirror topo->period into the dag_t.
 *   2. topology -- add/remove nodes and edges to match the snapshot,
 *                  filtering feedback / async links (already done by
 *                  the caller before reaching us).
 *   3. budgets  -- mirror per-follower WCET into the DAG, gated by
 *                  the recalc-threshold so a sub-threshold drift
 *                  does not invalidate the cached schedule.
 *   4. recalc   -- dag_recalculate then dag_foreach_node, calling
 *                  the caller-provided sched_cb for every follower.
 *
 * Soft-failure contract: any follower whose WCET is currently 0
 * (typically a node still bootstrapping, or one whose plugin
 * failed to start) is excluded from the DAG along with every edge
 * that touches it. The DAG library schedules the remaining sub-
 * graph; peers keep their SCHED_DEADLINE attributes. The soft
 * failure is sticky only as long as the WCET stays at 0; the next
 * non-zero sample re-incorporates the node on the following
 * reconcile pass.
 *
 * Back-off contract: consecutive reconcile failures (allocation
 * failures, library errors) raise a per-driver back-off counter;
 * after 16 strikes the worker stops trying until the topology
 * generation bumps. The state struct carries this counter so it
 * survives across reconcile_apply calls.
 *
 * Feedback-edge filtering rationale: feedback and async links in
 * the PipeWire graph use spa_io_async_buffers (one-cycle delay) and
 * therefore introduce no in-period precedence. The caller filters
 * them out of the topology snapshot before reaching reconcile, so
 * the DAG library never sees a cycle-creating edge in normal
 * operation; the library's ELOOP rejection is defense-in-depth.
 */

/* Opaque reconcile state. Owned by the caller, who must alloc-init
 * via reconcile_init and finalize via reconcile_fini. The state
 * caches the eventual persistent DAG so the typical no-op reconcile
 * cycle costs nothing beyond the freshness check. */
typedef struct reconcile_state reconcile_state_t;

/* Snapshot of a follower as seen by the reconcile layer. Fed by the
 * caller (module-deadline.c builds it from struct node). */
typedef struct {
	uint32_t id;
	pid_t    tid;
	uint64_t wcet;
} reconcile_follower_t;

/* Directed edge (src -> dst) in the topology view. */
typedef struct {
	uint32_t src;
	uint32_t dst;
} reconcile_edge_t;

/* Topology view consumed by reconcile_apply. */
typedef struct {
	const reconcile_follower_t *followers;
	uint32_t                    n_followers;

	const reconcile_edge_t     *edges;
	uint32_t                    n_edges;

	uint64_t period;
	uint64_t generation;

	/* When driver.schedule is active, the id of the driver node
	 * in the followers array. The reconcile layer marks this node
	 * as the timing root of the scheduling DAG so the deadline-
	 * splitting pass treats it as a source regardless of its
	 * data-flow edges. 0 means "no timing root" (either
	 * driver.schedule is off or the driver was not admitted). */
	uint32_t timing_root_id;
} reconcile_topo_t;

/*
 * Sched callback signature. Identical to dag_foreach_node's
 * callback. Called once per real follower per reconcile_apply with
 * the freshly assigned (runtime, cumulative_deadline, local_deadline,
 * period, cpu) tuple; the implementation in module-deadline.c is
 * sched_cb, which applies SCHED_DEADLINE + CPU affinity via
 * syscalls (gated by the per-follower last-applied tuple cache to
 * skip no-op syscalls). The kernel call consumes `local_deadline`
 * (kernel-relative), while `cumulative_deadline` (graph-relative)
 * is preserved for sound max-aggregation across fused-thread
 * members and for the JSON / debug snapshots.
 */
/* `fusion_group_leader_id` is the node id of the macro-node leader
 * the follower currently belongs to. For a singleton the leader is
 * the follower itself, so the value equals `id`. The Cucu-Grosjean
 * 2012 §IV "Path Coverage" rule treats a contracted node as a
 * fresh program: when the leader id changes between two reconcile
 * passes the follower's MBPTA estimator must be invalidated because
 * the new macro is observationally a different workload. Surfacing
 * the leader through the per-follower callback gives the
 * implementation a single point to detect the change without
 * reaching into reconcile internals. */
typedef void (*reconcile_sched_cb_t)(void *data, uint32_t id, pid_t tid,
		uint64_t runtime,
		uint64_t cumulative_deadline, uint64_t local_deadline,
		uint64_t period, uint32_t cpu,
		uint32_t fusion_group_leader_id);

/* Allocate and initialize a reconcile state. `n_cpus` and
 * `cpu_utilization` mirror the dag_create arguments;
 * `relative_capacity` is an optional length-`n_cpus` vector of
 * per-CPU capacity scalars in (0, 1] (NULL = uniform 1.0, the
 * homogeneous host); `recalc_threshold` is the per-WCET fractional
 * change required to mark the DAG dirty (0 means every change marks
 * dirty, 0.01 = the recommended default). `persistent` selects the
 * persistent-DAG path; when false, the legacy destroy-and-rebuild
 * path runs on every reconcile_apply and the threshold is ignored (a
 * permanent kill switch for the new code path).
 *
 * Returns NULL on allocation failure (errno set). */
reconcile_state_t *reconcile_init(uint32_t n_cpus,
		double cpu_utilization,
		const double *relative_capacity,
		double recalc_threshold,
		bool persistent);

/* Configure the bounded iterative recalc bound. After
 * reconcile_init, an additional knob controls whether the
 * dispatcher takes the single-shot recalc path or the iterative
 * heterogeneous-host path, and how many rounds the latter is
 * allowed to run before forcing convergence.
 *
 * `max_iterations` must be in [1, 8]; out-of-range values are
 * clamped to 1 (single-shot equivalent) with a warning. A value of
 * 1 -- and the absence of this call entirely -- both keep the
 * dispatcher on the legacy dag_recalculate path.
 *
 * Safe to call after reconcile_init and before the first
 * reconcile_apply; also safe to call later to retune at runtime
 * (the next reconcile_apply picks up the new bound). The function
 * is a no-op on a NULL state. */
void reconcile_state_set_heterogeneous_iterations(
		reconcile_state_t *state, uint32_t max_iterations);

/* Has the state carried over a persistent DAG since the last
 * reconcile_apply? Used by tests / instrumentation to distinguish
 * the persistent path from the legacy rebuild path. Always false
 * when reconcile_init was called with persistent=false. */
bool reconcile_state_has_persistent_dag(const reconcile_state_t *state);

/*
 * Query the count of in-period predecessors (incoming edges) of the
 * scheduling-DAG node with the given follower id, as a uint32_t.
 * This is the `required_external_inputs` value the per-cycle macro-
 * node release barrier needs: how many predecessors must complete
 * the current period before the follower's data loop may begin its
 * job. The result reflects the contracted scheduling DAG (fusion
 * collapsed where applicable), so a follower whose chain has been
 * fused with its predecessor returns 0 (the predecessor is now
 * internal and disappears from the contracted edge set).
 *
 * Returns 0 on NULL state, an unset persistent DAG, or an unknown
 * follower id; a return of 0 with state != NULL is a legitimate
 * "no in-period predecessors" answer for a graph source.
 */
uint32_t reconcile_state_node_required_external_inputs(
		const reconcile_state_t *state, uint32_t follower_id);

/* Lookup the persistent dag_node behind `follower_id` and copy its
 * predicted_runtime_ns / scheduled_runtime_ns fields out. The
 * predicted value is the analysis-layer estimate (conformal
 * estimator output, or its placement-stretched equivalent when the
 * heterogeneous iterative recalc supplied a runtime overlay); the
 * scheduled value is the kernel-facing runtime (equal to predicted
 * on the hard path, the clipped local_deadline on a soft-fallback
 * node). Both outputs are set to 0 when the follower has not yet
 * undergone a recalc that populated the dag_node fields, when the
 * state has no persistent dag (legacy rebuild path), or on NULL
 * inputs. Either out-pointer may be NULL to skip its copy. */
void reconcile_state_node_runtime_split(const reconcile_state_t *state,
		uint32_t follower_id,
		uint64_t *out_predicted_ns, uint64_t *out_scheduled_ns);

/*
 * Free everything reconcile_init allocated, plus any persistent
 * DAG state. Safe to call on a NULL pointer. Called by module-
 * deadline.c at driver_removed and module_destroy; no other
 * lifecycle hook frees the reconcile state.
 */
void reconcile_fini(reconcile_state_t *state);

/*
 * Drop the persistent DAG (forces a full rebuild on the next
 * reconcile_apply). Used by the back-off path when the DAG enters
 * an unrecoverable state. The reconcile_state itself stays alive;
 * only the cached dag_t is freed.
 */
void reconcile_drop(reconcile_state_t *state);

/* Run the full four-phase reconcile against `topo`. Calls
 * `sched_cb` for every real follower with its updated
 * (runtime, deadline, period, cpu) tuple. Returns 0 on success
 * with the DAG clean afterwards; -1 on infeasible input or
 * allocation failure (errno set), with the persistent state
 * dropped so the next call retries from scratch. */
int reconcile_apply(reconcile_state_t *state,
		const reconcile_topo_t *topo,
		reconcile_sched_cb_t sched_cb, void *sched_data);

/*
 * Reconcile-side feasibility classification.
 *
 * After each reconcile_apply, the dispatcher records whether the
 * contracted-DAG schedule passes the constrained-deadline EDF
 * density and processor-demand (DBF) tests (Baruah, Howell &
 * Rosier 1990 RTS). The summary lives on reconcile_state; the
 * accessor below copies it out so module-deadline.c can surface
 * it in the JSON snapshot's mode / feasibility fields without
 * needing access to the struct definition.
 *
 *   RECONCILE_MODE_HARD: both density and DBF agree the schedule
 *     is feasible, or density failed but the exact DBF accepted.
 *   RECONCILE_MODE_SOFT_DEGRADED: both predicates rejected. The
 *     kernel call still ships valid parameters (runtime <=
 *     local_deadline <= period stays enforced in module-deadline's
 *     apply path) but the analysis no longer claims to meet
 *     every deadline on every activation.
 *
 * Demotion is immediate on the first failure; promotion from
 * SOFT to HARD requires N consecutive feasible passes
 * (hysteresis is internal to reconcile_state). A NULL state
 * returns a default-zero report.
 */
enum reconcile_mode {
	RECONCILE_MODE_HARD          = 0,
	RECONCILE_MODE_SOFT_DEGRADED = 1,
};

struct reconcile_feasibility {
	enum reconcile_mode mode;
	bool     density_passed;
	double   max_density;
	uint32_t density_failing_cpu;
	bool     dbf_passed;
	uint64_t dbf_failing_t;
	uint64_t dbf_failing_demand;
	uint32_t dbf_failing_cpu;
	uint32_t consecutive_hard_passes;
	/* Stable lower_snake_case rejection reason; empty in HARD. */
	char     reason[64];
};

void reconcile_state_feasibility(const reconcile_state_t *state,
		struct reconcile_feasibility *out);

/*
 * Force the schedule mode to SOFT_DEGRADED with the given
 * reason. The caller uses this when an out-of-band signal
 * (e.g. sched_setattr() returning a non-zero errno after
 * reconcile_apply has already published a HARD classification)
 * proves the kernel rejected the in-process predicates'
 * verdict. The next reconcile_apply re-evaluates feasibility
 * on the macro-dag and may restore HARD via the standard
 * hysteresis path if the predicates accept; this call is a
 * one-shot demotion of the published classification.
 *
 * NULL state is a no-op. The reason string is copied verbatim
 * into state->feas.reason (truncated to fit).
 */
void reconcile_state_force_soft(reconcile_state_t *state, const char *reason);

/*
 * Typed reason vocabulary for one-shot soft-degraded demotions.
 *
 * Five reasons originate inside reconcile_apply (placer_rejected,
 * edf_infeasible, density_above_one, dbf_overload) and one
 * (kernel_rejected_or_invalid_params) at the apply path's
 * sched_setattr boundary. The constants below name the two
 * remaining triggers documented by the project's scheduling-model
 * reference -- a runtime-detected analyzability hole and an
 * insufficient-confidence estimator -- so callers do not invent
 * fresh strings on each call site and the JSON snapshot keeps a
 * closed vocabulary of reason tokens.
 *
 * The constants are plain C strings to avoid an enum-to-string
 * lookup; the reason field that records them is itself a
 * fixed-size char[] copy.
 *
 *   RECONCILE_SOFT_REASON_NODE_UNANALYZABLE -- a contracted node
 *     carries a member the daemon cannot place under
 *     SCHED_DEADLINE (a main-loop node, an exported node with no
 *     controllable reservation, a remote node whose processing
 *     TID is unknown, a multithreaded plugin's uncontrolled
 *     worker set) but the graph still needs to run. The hard EDF
 *     proof does not cover such work; soft mode is the only
 *     honest classification (Chen et al. 2019 on the limits of
 *     suspension-aware analysis).
 *   RECONCILE_SOFT_REASON_WCET_CONFIDENCE_LOW -- the MBPTA
 *     estimator has not converged AND the empirical sketch has
 *     not yet cleared its minimum-samples gate, so the budget the
 *     analysis would consume is a bootstrap fallback. A hard
 *     claim that the kernel runtime field bounds the work would
 *     not be defensible (Cucu-Grosjean 2012 §III on the
 *     minimum-number-of-observations result).
 */
#define RECONCILE_SOFT_REASON_NODE_UNANALYZABLE       "node_unanalyzable"
#define RECONCILE_SOFT_REASON_WCET_CONFIDENCE_LOW     "wcet_confidence_low"

#ifdef __cplusplus
}
#endif

#endif /* RECONCILE_H */
