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

#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/log.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dag_node dag_node_t;
typedef struct dag_edge dag_edge_t;
typedef struct dag dag_t;

struct dag_node {
	struct spa_list link;        /* link in dag->nodes */
	struct spa_list outgoing;    /* list of outgoing edges (dag_edge_t) */
	struct spa_list incoming;    /* list of incoming edges (dag_edge_t) */

	uint32_t id;
	uint64_t wcet;       /* worst case execution time */
	uint64_t deadline;   /* assigned relative deadline */
	uint32_t cpu;        /* assigned CPU */
	pid_t tid;           /* associated thread id */

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
	float utilization;  /* max topology-aware per-CPU DAG load */
	uint32_t num_cpus;

	struct spa_list nodes; /* list of dag_node_t */
	struct spa_list edges; /* list of dag_edge_t */
};

/*
 * This library supports generic finite DAGs, including multiple sources,
 * multiple sinks, and disconnected components. Scheduling is computed over
 * each reachable source -> sink subproblem; unreachable source/sink
 * combinations are skipped.
 */

/* Create and destroy a DAG.
 * utilization is the maximum per-CPU load admitted for this DAG after
 * accounting for precedence: only pairwise unrelated tasks contribute
 * concurrently on the same CPU.
 */
dag_t *dag_create(uint64_t period, uint64_t deadline, float utilization, uint32_t num_cpus);
void dag_destroy(dag_t *g);

/* Set global period and deadline */
int dag_set_global_period_deadline(dag_t *g, uint64_t period, uint64_t deadline);

/* Add and remove nodes */
int dag_add_node(dag_t *g, uint32_t id, uint64_t wcet, pid_t tid);
int dag_remove_node(dag_t *g, uint32_t id);

/* Add and remove edges.
 * dag_add_edge() rejects self-loops and cycle-creating edges with ELOOP.
 */
int dag_add_edge(dag_t *g, uint32_t src_id, uint32_t dst_id);
int dag_remove_edge(dag_t *g, uint32_t src_id, uint32_t dst_id);

/* Update the WCET of a node */
int dag_set_node_wcet(dag_t *g, uint32_t id, uint64_t wcet);

/* Recalculate scheduling parameters after changes.
 * On failure, any previously assigned deadlines/CPUs are cleared.
 */
int dag_recalculate(dag_t *g);

/* Apply a function to all tids with current scheduling parameters */
typedef void (*dag_node_callback_t)(void *data, pid_t tid, uint64_t wcet, uint64_t deadline, uint64_t period, uint32_t cpu);
int dag_foreach_node(dag_t *g, dag_node_callback_t cb, void *data);

void dag_print(dag_t *g);

#ifdef __cplusplus
}
#endif

#endif /* DAG_H */
