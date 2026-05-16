/*
 * contracted.c
 *
 * Implementation of the contracted-DAG types declared in
 * contracted.h. Pure data plumbing: no PipeWire runtime symbols,
 * no syscalls. Tests drive the builder with synthetic graphs and
 * verify shape; the analysis layer consumes the result.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "contracted.h"

contracted_dag_t *contracted_dag_create(uint64_t period_ns, uint64_t deadline_ns)
{
	contracted_dag_t *cg = calloc(1, sizeof(*cg));
	if (cg == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	spa_list_init(&cg->nodes);
	spa_list_init(&cg->edges);
	cg->period_ns = period_ns;
	cg->deadline_ns = deadline_ns;
	return cg;
}

void contracted_dag_destroy(contracted_dag_t *cg)
{
	contracted_node_t *cn, *tmp_n;
	contracted_edge_t *ce, *tmp_e;
	struct contracted_member *m, *tmp_m;

	if (cg == NULL)
		return;

	spa_list_for_each_safe(ce, tmp_e, &cg->edges, link) {
		spa_list_remove(&ce->link);
		/* src_link / dst_link are also being torn down here;
		 * removing them keeps spa_list invariants for the
		 * node-side iteration below, even though the nodes
		 * themselves are freed next. */
		spa_list_remove(&ce->src_link);
		spa_list_remove(&ce->dst_link);
		free(ce);
	}
	spa_list_for_each_safe(cn, tmp_n, &cg->nodes, link) {
		spa_list_for_each_safe(m, tmp_m, &cn->members, link) {
			spa_list_remove(&m->link);
			free(m);
		}
		spa_list_remove(&cn->link);
		free(cn);
	}
	free(cg);
}

contracted_node_t *contracted_dag_add_node(contracted_dag_t *cg)
{
	contracted_node_t *cn;

	if (cg == NULL) {
		errno = EINVAL;
		return NULL;
	}
	cn = calloc(1, sizeof(*cn));
	if (cn == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	cn->id = cg->n_nodes++;
	cn->cpu = -1;
	spa_list_init(&cn->members);
	spa_list_init(&cn->preds);
	spa_list_init(&cn->succs);
	spa_list_append(&cg->nodes, &cn->link);
	return cn;
}

int contracted_node_add_member(contracted_node_t *cn, uint32_t id,
		pid_t tid, uint64_t wcet_ns)
{
	struct contracted_member *m;

	if (cn == NULL)
		return -EINVAL;
	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return -ENOMEM;
	m->id = id;
	m->tid = tid;
	m->wcet_ns = wcet_ns;
	spa_list_append(&cn->members, &m->link);
	cn->n_members++;
	cn->is_fusion_group = cn->n_members > 1;
	return 0;
}

int contracted_dag_add_edge(contracted_dag_t *cg,
		contracted_node_t *src, contracted_node_t *dst)
{
	contracted_edge_t *ce;

	if (cg == NULL || src == NULL || dst == NULL)
		return -EINVAL;
	if (src == dst)
		return -EINVAL;

	/* Deduplicate parallel edges between the same two macro-nodes.
	 * Walk the source's successor list; a sub-millisecond cost
	 * for the graph sizes the audio path produces. */
	spa_list_for_each(ce, &src->succs, src_link) {
		if (ce->dst == dst)
			return 0;
	}

	ce = calloc(1, sizeof(*ce));
	if (ce == NULL)
		return -ENOMEM;
	ce->src = src;
	ce->dst = dst;
	spa_list_append(&cg->edges, &ce->link);
	spa_list_append(&src->succs, &ce->src_link);
	spa_list_append(&dst->preds, &ce->dst_link);
	cg->n_edges++;
	return 0;
}

/* DFS-based cycle detector. Two colour bits per node, recorded in
 * a side array since contracted_node has no scratch space; the
 * iteration is bounded by n_nodes so the bookkeeping cost is
 * O(V + E). */
enum dfs_colour {
	DFS_WHITE = 0,
	DFS_GREY  = 1,
	DFS_BLACK = 2,
};

static bool dfs_has_cycle(const contracted_dag_t *cg,
		const contracted_node_t *root,
		uint8_t *colour, contracted_node_t **stack,
		uint32_t stack_cap)
{
	uint32_t top = 0;
	(void)cg;

	stack[top++] = (contracted_node_t *)root;
	colour[root->id] = DFS_GREY;

	while (top > 0) {
		contracted_node_t *cn = stack[top - 1];
		bool advanced = false;
		contracted_edge_t *ce;

		spa_list_for_each(ce, &cn->succs, src_link) {
			uint8_t c = colour[ce->dst->id];
			if (c == DFS_GREY)
				return true;
			if (c == DFS_WHITE) {
				colour[ce->dst->id] = DFS_GREY;
				if (top >= stack_cap)
					return true;
				stack[top++] = ce->dst;
				advanced = true;
				break;
			}
		}
		if (!advanced) {
			colour[cn->id] = DFS_BLACK;
			top--;
		}
	}
	return false;
}

bool contracted_dag_has_cycle(const contracted_dag_t *cg)
{
	uint8_t *colour;
	contracted_node_t **stack;
	contracted_node_t *cn;
	bool result = false;

	if (cg == NULL || cg->n_nodes == 0)
		return false;

	colour = calloc(cg->n_nodes, sizeof(*colour));
	stack = calloc(cg->n_nodes, sizeof(*stack));
	if (colour == NULL || stack == NULL) {
		free(colour);
		free(stack);
		return true;
	}

	spa_list_for_each(cn, &cg->nodes, link) {
		if (colour[cn->id] != DFS_WHITE)
			continue;
		if (dfs_has_cycle(cg, cn, colour, stack, cg->n_nodes)) {
			result = true;
			break;
		}
	}

	free(colour);
	free(stack);
	return result;
}
