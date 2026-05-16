#ifndef DAG_H
#define DAG_H

/*
 * dag.h
 *
 * Library to calculate P-EDF scheduling parameters on a DAG of tasks.
 *
 * API inspired by the DAG implementation from the paper
 * "Multi-Criteria Optimization of Real-Time DAGs on Heterogeneous Platforms under P-EDF"
 * T. Cucinotta, A. Amory, G. Ara, F. Paladino, M. Di Natale
 * https://doi.org/10.1145/3592609
 *
 * Copyright (C) 2024 Antonio Napolitano and Francesco Barcherini
 */

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdbool.h>
#include <assert.h>

#include "bitset.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/log.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dag_node dag_node_t;
typedef struct dag_edge dag_edge_t;
typedef struct dag dag_t;

/* Sentinel CPU value stamped on every dag_node while its scheduling
 * assignment is stale (i.e. while the DAG is dirty or fresh out of
 * dag_add_node). It is distinct from any valid 0..num_cpus-1 index
 * so a caller can tell "this node has never been placed" from "this
 * node was placed on CPU 0".
 *
 * The library never emits DAG_CPU_INVALID through dag_foreach_node
 * on a clean DAG: dag_recalculate either fully assigns every real
 * node or fails with the DAG left dirty. */
#define DAG_CPU_INVALID UINT32_MAX

struct dag_node {
	struct spa_list link;        /* link in dag->nodes */
	struct spa_list outgoing;    /* list of outgoing edges (dag_edge_t) */
	struct spa_list incoming;    /* list of incoming edges (dag_edge_t) */

	uint32_t id;
	uint32_t index;      /* dense topological index for analysis caches */
	uint64_t wcet;       /* worst case execution time */
	uint64_t deadline;   /* assigned relative deadline (legacy field;
			      * mirrors local_deadline once populated and is
			      * retained for in-flight callers that have not
			      * yet switched to the explicit fields below). */

	/*
	 * Explicit deadline semantics.
	 *
	 * `cumulative_deadline` is **graph-relative**: it expresses
	 * the latest instant, measured from the driver-graph's
	 * activation, by which this node must have finished. It is the
	 * absolute milestone the analysis layer reasons about; it is
	 * never passed to the kernel directly. The forward
	 * topological sum that populates it is monotonic along every
	 * scheduling edge: for every edge u -> v,
	 * cumulative_deadline[u] <= cumulative_deadline[v].
	 *
	 * `local_deadline` is **kernel-relative**: it is the relative
	 * deadline value handed to sched_setattr() for the node's
	 * data-loop thread, expressing "after the node is released
	 * (its predecessors have completed), this is the time it has
	 * to finish". For a source node local_deadline equals
	 * cumulative_deadline. For a non-source node it is
	 *
	 *   local_deadline = cumulative_deadline
	 *                  - max(pred.cumulative_deadline)
	 *
	 * which is the kernel API's natural unit (Linux kernel docs,
	 * "Deadline Task Scheduling", sched-deadline.rst).
	 *
	 * Both fields are zero outside an analysis pass; they are
	 * populated by dag_recalculate and cleared by
	 * dag_invalidate_analysis.
	 */
	uint64_t cumulative_deadline;
	uint64_t local_deadline;

	uint32_t cpu;        /* assigned CPU */
	pid_t tid;           /* associated thread id */

	/* Co-location group: all nodes sharing this non-zero id must
	 * land on the same CPU in the worst-fit pass. Zero (the
	 * default) means "ungrouped" -- the placement is free to pick
	 * any feasible CPU. Used by the chain-merge feature: nodes
	 * that libpipewire has consolidated onto a single thread are
	 * stamped with the same group_id so the scheduling DAG places
	 * them together. Per-node deadlines, periods, and WCETs are
	 * unaffected -- the group only constrains CPU assignment. */
	uint32_t group_id;

	bool fictitious;
	uint64_t remaining_deadline;
	uint64_t longest_len;
	int longest_next;
	bitset_t *successors;

	bool deadline_assigned;
};

struct dag_edge {
	struct spa_list link;  /* link in dag->edges */
	dag_node_t *src;
	dag_node_t *dst;
	struct spa_list src_link; /* link in src->outgoing */
	struct spa_list dst_link; /* link in dst->incoming */
};

struct dag {
	uint64_t period;    /* global period */
	uint64_t deadline;  /* global end-to-end deadline */
	double admission_ceiling; /* max per-CPU DAG load (in relative
				   * utilisation units), must be finite
				   * and in (0, 1]; rejected at create
				   * time otherwise. Compared against the
				   * worst-case running sum during the
				   * worst-fit placement pass, with a
				   * small epsilon so floating-point
				   * round-off does not exclude exactly
				   * fitting workloads. */
	uint32_t num_cpus;

	/* Per-CPU capacity scalar in (0, 1]. relative_capacity[i] = 1.0
	 * means CPU i is the reference (any CPU with raw_capacity *
	 * freq equal to the max in the set); slower CPUs come in below
	 * 1.0. Always non-NULL after dag_create (NULL input is
	 * promoted to a vector of 1.0s, the homogeneous identity).
	 * Length is num_cpus; owned by the dag_t, freed in
	 * dag_destroy. */
	double  *relative_capacity;

	/* Set whenever a successful timing or topology mutation
	 * invalidates the previously-computed scheduling parameters.
	 * dag_recalculate clears it on success and re-sets it (with
	 * cleared assignments) on failure. dag_foreach_node triggers
	 * an internal recalculate only when this is true, so a
	 * caller-side no-op cycle (no mutations between two foreach
	 * passes) does not re-run the analyser. */
	bool dirty;

	struct spa_list nodes; /* list of dag_node_t */
	struct spa_list edges; /* list of dag_edge_t */

	dag_node_t **indexed_nodes;
	uint32_t indexed_count;
	bitset_t **unrelated;
	uint32_t unrelated_size;
	uint32_t unrelated_capacity;

	/* Co-location group caches used to collapse merged/fused nodes
	 * into a single virtual node during the antichain enumeration.
	 * Nodes that share a non-zero dag_node::group_id (stamped by
	 * the reconcile layer from a shared PW_KEY_NODE_LOOP_TID) are
	 * placed in the same dense group; ungrouped real nodes each
	 * become their own singleton group. Fictitious nodes never
	 * participate (node_group_index[i] == UINT32_MAX). The
	 * antichain enumeration only iterates over one representative
	 * per group, shrinking the branching factor of the
	 * dag_comp_unrelated branch-and-bound from |real nodes| to
	 * |groups|; on emission, every representative bit is expanded
	 * into the full member bitset so the downstream worst-fit
	 * pass still sees the correct per-node load contributions.
	 *
	 * Allocated by dag_build_groups (called from
	 * dag_build_analysis after dag_build_successors); freed by
	 * dag_invalidate_analysis (same lifecycle as indexed_nodes /
	 * unrelated). NULL outside an active analysis. */
	uint32_t   group_count;
	uint32_t  *node_group_index;     /* indexed_count entries; UINT32_MAX = fictitious / not in a group */
	uint32_t  *group_rep_node_index; /* group_count entries; lowest-index member of each group */
	bitset_t **group_members;        /* group_count entries; each is a bitset over indexed_count */
	bitset_t **group_node_succ;      /* group_count entries; union of members' node-level successors */

	/* Persistent O(log N) id -> dag_node_t* index. Sorted by
	 * dag_node_t::id ascending; maintained by dag_add_node and
	 * dag_remove_node, queried by dag_find_node. Geometric
	 * reallocation (start 16, double on overflow). Outlives the
	 * indexed_nodes / unrelated caches, which only exist between
	 * dag_build_analysis and the next mutation; this index is
	 * always valid as long as nodes exist. */
	dag_node_t **nodes_by_id;
	uint32_t     nodes_by_id_count;
	uint32_t     nodes_by_id_cap;

	/* Scratch buffers used inside a single dag_recalculate run.
	 * Allocated once when the indexed-nodes cache is built (so
	 * they share the same lifetime as indexed_nodes), reused across
	 * every compute_longest_path / assign_path_head_deadline call
	 * that runs inside the same recalc. Freed by
	 * dag_invalidate_analysis. The persistent across-recalc id
	 * index (nodes_by_id, added in a later commit) is a different,
	 * complementary cache. */
	dag_node_t **ws_path;       /* path buffer, length <= indexed_count */
	bool        *ws_excluded;   /* excluded-from-discount flags */
	uint32_t     ws_capacity;   /* allocated length of both above */
};

/* Create and destroy a DAG.
 *
 * `admission_ceiling` is the per-CPU upper bound on total relative
 * utilisation: every per-CPU running sum produced by the worst-fit
 * placement is compared against this value. Must be finite and in
 * (0, 1].
 *
 * `relative_capacity` is an optional length-`num_cpus` vector of
 * per-CPU capacity scalars in (0, 1]; entry i is CPU i's relative
 * throughput, with the fastest CPU(s) at 1.0. NULL is shorthand for a
 * uniform 1.0 vector, the homogeneous-host case, which reduces
 * admission and placement to the original (pre-heterogeneous-CPU)
 * arithmetic. The vector is copied; the caller keeps ownership of
 * its input. */
dag_t *dag_create(uint64_t period, uint64_t deadline, double admission_ceiling,
		uint32_t num_cpus, const double *relative_capacity);
void dag_destroy(dag_t *g);

/* Set global period and deadline */
int dag_set_global_period_deadline(dag_t *g, uint64_t period, uint64_t deadline);

/* Add and remove nodes */
int dag_add_node(dag_t *g, uint32_t id, uint64_t wcet, pid_t tid, bool fictitious);
int dag_remove_node(dag_t *g, uint32_t id);

/* Add and remove edges */
int dag_add_edge(dag_t *g, uint32_t src_id, uint32_t dst_id);
int dag_remove_edge(dag_t *g, uint32_t src_id, uint32_t dst_id);

/* Update the WCET of a node */
int dag_set_node_wcet(dag_t *g, uint32_t id, uint64_t wcet);

/* Stamp a node with a co-location group id. Nodes sharing a non-zero
 * group_id are constrained to land on the same CPU during the
 * worst-fit assignment pass: the highest-utilisation member of the
 * group (in the existing util-descending iteration order) picks the
 * CPU, and every subsequent member is forced onto that same CPU,
 * with admission still checked. Passing group_id=0 clears the
 * grouping. Returns 0 on success, -1 with errno set on ENOENT
 * (unknown id) or EINVAL (null dag). The dirty bit is set only when
 * the assignment actually changes, so calling this with the current
 * value is a no-op. */
int dag_set_node_group(dag_t *g, uint32_t id, uint32_t group_id);

/* O(log N) lookup of a real or fictitious node by id. Returns
 * NULL if the id is not in the graph. The returned pointer is
 * stable until the next dag_remove_node touching this id. */
dag_node_t *dag_find_node(dag_t *g, uint32_t id);

/* Recalculate scheduling parameters after changes */
int dag_recalculate(dag_t *g);

/*
 * Per-CPU EDF density. For every real node assigned to `cpu`, sum
 *
 *     C_i / min(D_i, T_i)
 *
 * where C_i is the node's wcet (in reference-CPU units), D_i is
 * its local_deadline, and T_i is the global period. The result is
 * divided by `cpu`'s relative_capacity so a slower CPU's density
 * reflects the wall-clock cost the node will actually incur
 * there. The result is the constrained-deadline EDF density on
 * that CPU; the kernel's SCHED_DEADLINE sufficient-feasibility
 * test (Linux kernel sched-deadline.rst §Bandwidth management;
 * Baruah, Howell & Rosier 1990 RTS) requires density <= 1.
 *
 * Returns 0.0 on a CPU with no assigned nodes, on null inputs, or
 * when cpu is out of range. Skips nodes whose cumulative analysis
 * results have not yet been populated (deadline_assigned == false).
 */
double dag_per_cpu_density(const dag_t *g, uint32_t cpu);

/*
 * Density-sufficient feasibility test on the post-placement
 * schedule. Returns true iff every CPU's density (see
 * dag_per_cpu_density) is <= 1.0; otherwise returns false and
 * sets *out_max_density / *out_failing_cpu to the highest density
 * observed and the CPU that produced it (NULL pointers are
 * tolerated).
 *
 * The check is sufficient but not necessary: a task set that
 * fails density may still be EDF-feasible by the exact
 * processor-demand criterion (Baruah 1990). The DBF check is the
 * heavier fallback once it lands.
 */
bool dag_density_feasible(const dag_t *g,
		double *out_max_density,
		uint32_t *out_failing_cpu);

/*
 * Processor-demand (DBF) feasibility test on the post-placement
 * schedule. Baruah, Howell & Rosier 1990 (Real-Time Systems
 * 2(4):301-324) gives an exact EDF feasibility test for
 * constrained-deadline sporadic task sets on a uniprocessor: the
 * task set is schedulable iff
 *
 *     sum_i dbf_i(t) <= scaled_t  for every relevant t
 *
 * where dbf_i(t) = max(0, floor((t - D_i) / T_i) + 1) * C_i for
 * t >= D_i (zero otherwise), and scaled_t accounts for the CPU's
 * relative capacity.
 *
 * In the current scheduling model every task shares the global
 * period (T_i == g->period), so the relevant checkpoints reduce
 * to {k * T + D_i} for k in [0, K_max] and every task on the
 * partition. The implementation walks k up to a bound derived
 * from the per-CPU utilisation; on a feasible partition the
 * sweep terminates after at most one period worth of checkpoints.
 *
 * Returns true iff every CPU is DBF-feasible. On the first
 * failing checkpoint, returns false and sets *out_failing_cpu /
 * *out_failing_t / *out_failing_demand to the offending partition,
 * the checkpoint t at which the bound was breached, and the
 * computed demand sum (NULL pointers are tolerated).
 *
 * The check is strictly stronger than dag_density_feasible: a
 * task set that passes density also passes DBF, but DBF can
 * accept density-infeasible task sets whose tail of jobs
 * actually fits.
 */
bool dag_dbf_feasible(const dag_t *g,
		uint32_t *out_failing_cpu,
		uint64_t *out_failing_t,
		uint64_t *out_failing_demand);

/* Compute every real node's local_deadline from its already-populated
 * cumulative_deadline using the kernel-API conversion
 *
 *   local_deadline = cumulative_deadline - max(pred.cumulative_deadline)
 *
 * with the boundary rule that a source node (no real predecessor)
 * has local_deadline = cumulative_deadline. Returns true on
 * success, false on any of:
 *
 *   - g or g->indexed_nodes is NULL,
 *   - cumulative_deadline is non-monotonic along some edge,
 *   - the conversion yields zero or a value greater than g->period
 *     for any real node (the kernel SCHED_DEADLINE contract rejects
 *     zero deadlines and a deadline above the period).
 *
 * The function does not touch g->dirty: it is callable as a
 * post-contraction step from the analysis layer once
 * cumulative_deadline has been assigned. dag_recalculate runs it
 * automatically as the closing step of the splitter pass; external
 * callers (Phase 2's contracted DAG) invoke it directly.
 */
bool dag_compute_local_deadlines(dag_t *g);

/* Return true if the DAG currently contains a cycle. Walks the
 * graph independently of the indexed-nodes cache so the caller may
 * invoke it even before dag_recalculate. Defense-in-depth for the
 * topology-snapshot filter in module-deadline. */
bool dag_has_cycle(dag_t *g);

/* Apply a function to all real nodes with their current scheduling
 * parameters. The id is passed alongside the tid so the caller can
 * look up its own per-node state (e.g. the sched_setattr skip
 * cache) without rebuilding a tid index.
 *
 * The deadline is exposed in two forms:
 *   - cumulative_deadline: graph-relative milestone. Useful for the
 *     contracted-DAG analysis and for sound max-aggregation across
 *     a fused thread's members (a chain's externally observable
 *     completion bound).
 *   - local_deadline: kernel-relative. This is the value the
 *     caller must hand to sched_setattr() for the corresponding
 *     thread; it is `cumulative_deadline - max(pred.cumulative)`
 *     and equals the legacy per-node deadline slice produced by
 *     the splitter for a single non-fused node.
 */
/* Per-node iteration callback. `fusion_group_leader_id` is the
 * macro-node leader the node belongs to; on a singleton (no
 * fusion) the value equals `id`. Callers that do not care about
 * fusion can ignore the trailing parameter. */
typedef void (*dag_node_callback_t)(void *data, uint32_t id, pid_t tid,
		uint64_t wcet,
		uint64_t cumulative_deadline, uint64_t local_deadline,
		uint64_t period, uint32_t cpu,
		uint32_t fusion_group_leader_id);
int dag_foreach_node(dag_t *g, dag_node_callback_t cb, void *data);

void dag_print(dag_t *g);

#ifdef __cplusplus
}
#endif

#endif /* DAG_H */
