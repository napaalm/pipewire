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
	uint64_t deadline;   /* assigned relative deadline */
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

/* Return true if the DAG currently contains a cycle. Walks the
 * graph independently of the indexed-nodes cache so the caller may
 * invoke it even before dag_recalculate. Defense-in-depth for the
 * topology-snapshot filter in module-deadline. */
bool dag_has_cycle(dag_t *g);

/* Apply a function to all real nodes with their current scheduling
 * parameters. The id is passed alongside the tid so the caller can
 * look up its own per-node state (e.g. the sched_setattr skip
 * cache) without rebuilding a tid index. */
typedef void (*dag_node_callback_t)(void *data, uint32_t id, pid_t tid,
		uint64_t wcet, uint64_t deadline, uint64_t period, uint32_t cpu);
int dag_foreach_node(dag_t *g, dag_node_callback_t cb, void *data);

void dag_print(dag_t *g);

#ifdef __cplusplus
}
#endif

#endif /* DAG_H */
