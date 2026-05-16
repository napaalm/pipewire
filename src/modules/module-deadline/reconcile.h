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
typedef void (*reconcile_sched_cb_t)(void *data, uint32_t id, pid_t tid,
		uint64_t runtime,
		uint64_t cumulative_deadline, uint64_t local_deadline,
		uint64_t period, uint32_t cpu);

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

/* Has the state carried over a persistent DAG since the last
 * reconcile_apply? Used by tests / instrumentation to distinguish
 * the persistent path from the legacy rebuild path. Always false
 * when reconcile_init was called with persistent=false. */
bool reconcile_state_has_persistent_dag(const reconcile_state_t *state);

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

#ifdef __cplusplus
}
#endif

#endif /* RECONCILE_H */
