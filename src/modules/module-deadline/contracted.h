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

	/* Aggregated timing. wcet_ns is the sum of member WCETs plus
	 * the measured overhead; the two are tracked separately so a
	 * reader can distinguish work from book-keeping. */
	uint64_t wcet_ns;
	uint64_t overhead_ns;

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

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEADLINE_CONTRACTED_H */
