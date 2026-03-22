/*
 * dag.c
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <limits.h>
#include <errno.h>

#include "dag.h"
#include <time.h>

#define DAG_NODE_INDEX_INVALID UINT32_MAX
#define DAG_FICTITIOUS_SOURCE_ID UINT32_MAX
#define DAG_FICTITIOUS_SINK_ID (UINT32_MAX - 1)

typedef struct {
	dag_node_t **nodes;
	uint32_t count;
} node_array_t;

static inline uint32_t dag_list_len(struct spa_list *list)
{
	uint32_t len = 0;
	dag_node_t *pos;
	spa_list_for_each(pos, list, link) len++;
	return len;
}

static bool dag_is_reserved_fictitious_id(uint32_t id)
{
	return id == DAG_FICTITIOUS_SOURCE_ID || id == DAG_FICTITIOUS_SINK_ID;
}

static void dag_invalidate_schedule(dag_t *g)
{
	dag_node_t *n;

	if (!g)
		return;

	spa_list_for_each(n, &g->nodes, link) {
		n->deadline_assigned = false;
		n->deadline = 0;
		n->remaining_deadline = 0;
	}
}

static void dag_free_unrelated(dag_t *g)
{
	uint32_t i;

	if (!g || !g->unrelated)
		return;

	for (i = 0; i < g->unrelated_capacity; i++)
		free(g->unrelated[i]);

	free(g->unrelated);
	g->unrelated = NULL;
	g->unrelated_size = 0;
	g->unrelated_capacity = 0;
}

static void dag_free_indexed_nodes(dag_t *g)
{
	free(g->indexed_nodes);
	g->indexed_nodes = NULL;
	g->indexed_count = 0;
}

static void dag_invalidate_analysis(dag_t *g)
{
	dag_node_t *n;

	if (!g)
		return;

	spa_list_for_each(n, &g->nodes, link) {
		n->index = DAG_NODE_INDEX_INVALID;
		n->longest_len = 0;
		n->longest_next = -1;
	}

	dag_free_unrelated(g);
	dag_free_indexed_nodes(g);
}

dag_t *dag_create(uint64_t period, uint64_t deadline, float utilization, uint32_t num_cpus)
{
	if (period == 0 || deadline == 0 || utilization <= 0.0f || utilization > 1.0f || num_cpus == 0) {
		errno = EINVAL;
		return NULL;
	}

	dag_t *g = calloc(1, sizeof(*g));
	if (!g)
		return NULL;

	g->period = period;
	g->deadline = deadline;
	g->utilization = utilization;
	g->num_cpus = num_cpus;
	spa_list_init(&g->nodes);
	spa_list_init(&g->edges);

	return g;
}

void dag_destroy(dag_t *g)
{
	if (!g)
		return;

	dag_invalidate_analysis(g);

	/* Remove all edges */
	dag_edge_t *e, *etmp;
	spa_list_for_each_safe(e, etmp, &g->edges, link) {
		spa_list_remove(&e->link);
		if (e->src)
			spa_list_remove(&e->src_link);
		if (e->dst)
			spa_list_remove(&e->dst_link);
		free(e);
	}

	/* Remove all nodes */
	dag_node_t *n, *ntmp;
	spa_list_for_each_safe(n, ntmp, &g->nodes, link) {
		spa_list_remove(&n->link);
		free(n);
	}

	free(g);
}

int dag_set_global_period_deadline(dag_t *g, uint64_t period, uint64_t deadline)
{
	if (!g) {
		errno = EINVAL;
		return -1;
	}
	if (period == 0 || deadline == 0) {
		errno = EINVAL;
		return -1;
	}

	g->period = period;
	g->deadline = deadline;
	dag_invalidate_schedule(g);
	return 0;
}

static dag_node_t *find_node(dag_t *g, uint32_t id)
{
	dag_node_t *n;
	spa_list_for_each(n, &g->nodes, link) {
		if (n->id == id)
			return n;
	}
	return NULL;
}

static dag_node_t *dag_find_fictitious_source(dag_t *g)
{
	return find_node(g, DAG_FICTITIOUS_SOURCE_ID);
}

static dag_node_t *dag_find_fictitious_sink(dag_t *g)
{
	return find_node(g, DAG_FICTITIOUS_SINK_ID);
}

int dag_add_node(dag_t *g, uint32_t id, uint64_t wcet, pid_t tid, bool fictitious)
{
	if (!g) {
		errno = EINVAL;
		return -1;
	}
	if (!fictitious && dag_is_reserved_fictitious_id(id)) {
		errno = EINVAL;
		return -1;
	}
	if (wcet == 0 && !fictitious) {
		pw_log_error("Cannot add node %u with wcet=0", id);
		errno = EINVAL;
		return -1;
	}
	if (find_node(g, id)) {
		errno = EEXIST;
		return -1;
	}

	dag_node_t *n = calloc(1, sizeof(*n));
	if (!n)
		return -1;

	n->id = id;
	n->index = DAG_NODE_INDEX_INVALID;
	n->wcet = wcet;
	n->tid = tid;

	n->fictitious = fictitious;
	n->remaining_deadline = 0;
	n->longest_len = 0;
	n->longest_next = -1;
	n->deadline = 0;
	n->deadline_assigned = false;
	spa_list_init(&n->outgoing);
	spa_list_init(&n->incoming);
	spa_list_append(&g->nodes, &n->link);

	dag_invalidate_analysis(g);
	dag_invalidate_schedule(g);
	return 0;
}

static int dag_remove_node_ptr(dag_t *g, dag_node_t *n)
{
	if (!g || !n) {
		errno = EINVAL;
		return -1;
	}

	/* Remove all edges involving n */
	dag_edge_t *e, *etmp;
	spa_list_for_each_safe(e, etmp, &n->incoming, dst_link) {
		spa_list_remove(&e->link);
		spa_list_remove(&e->dst_link);
		spa_list_remove(&e->src_link);
		free(e);
	}
	spa_list_for_each_safe(e, etmp, &n->outgoing, src_link) {
		spa_list_remove(&e->link);
		spa_list_remove(&e->src_link);
		spa_list_remove(&e->dst_link);
		free(e);
	}

	spa_list_remove(&n->link);
	free(n->successors);
	free(n);
	return 0;
}

static int dag_remove_fictitious_nodes(dag_t *g)
{
	dag_node_t *n, *tmp;

	if (!g)
		return -1;

	spa_list_for_each_safe(n, tmp, &g->nodes, link) {
		if (!n->fictitious)
			continue;
		if (dag_remove_node_ptr(g, n) < 0)
			return -1;
	}

	return 0;
}

int dag_remove_node(dag_t *g, uint32_t id)
{
	if (!g) {
		errno = EINVAL;
		return -1;
	}

	dag_node_t *n = find_node(g, id);
	if (!n) {
		errno = ENOENT;
		return -1;
	}

	if (dag_remove_node_ptr(g, n) < 0)
		return -1;

	dag_invalidate_analysis(g);
	dag_invalidate_schedule(g);
	return 0;
}

int dag_add_edge(dag_t *g, uint32_t src_id, uint32_t dst_id)
{
	if (!g) {
		errno = EINVAL;
		return -1;
	}

	dag_node_t *src = find_node(g, src_id);
	dag_node_t *dst = find_node(g, dst_id);
	if (!src || !dst) {
		errno = ENOENT;
		return -1;
	}

	/* Check if edge already exists */
	dag_edge_t *e;
	spa_list_for_each(e, &g->edges, link) {
		if (e->src == src && e->dst == dst) {
			errno = EEXIST;
			return -1;
		}
	}

	e = calloc(1, sizeof(*e));
	if (!e)
		return -1;

	e->src = src;
	e->dst = dst;
	spa_list_append(&g->edges, &e->link);
	spa_list_append(&src->outgoing, &e->src_link);
	spa_list_append(&dst->incoming, &e->dst_link);

	dag_invalidate_analysis(g);
	dag_invalidate_schedule(g);
	return 0;
}

int dag_remove_edge(dag_t *g, uint32_t src_id, uint32_t dst_id)
{
	if (!g) {
		errno = EINVAL;
		return -1;
	}

	dag_node_t *src = find_node(g, src_id);
	dag_node_t *dst = find_node(g, dst_id);
	if (!src || !dst) {
		errno = ENOENT;
		return -1;
	}

	dag_edge_t *e, *etmp;
	spa_list_for_each_safe(e, etmp, &g->edges, link) {
		if (e->src == src && e->dst == dst) {
			spa_list_remove(&e->link);
			spa_list_remove(&e->src_link);
			spa_list_remove(&e->dst_link);
			free(e);
			dag_invalidate_analysis(g);
			dag_invalidate_schedule(g);
			return 0;
		}
	}

	errno = ENOENT;
	return -1;
}

int dag_set_node_wcet(dag_t *g, uint32_t id, uint64_t wcet)
{
	if (!g) {
		errno = EINVAL;
		return -1;
	}

	dag_node_t *n = find_node(g, id);
	if (!n) {
		errno = ENOENT;
		return -1;
	}

	n->wcet = wcet;
	dag_invalidate_schedule(g);
	return 0;
}

/***********************************************************************
 * Scheduling Parameter Computation
 ***********************************************************************/

/* Helper structures and functions for topological sort and longest path */

static node_array_t dag_nodes_to_array(dag_t *g)
{
	uint32_t count = dag_list_len(&g->nodes);
	node_array_t arr;
	uint32_t i = 0;
	dag_node_t *n;

	arr.count = count;
	arr.nodes = malloc((size_t)count * sizeof(dag_node_t *));
	if (!arr.nodes && count > 0)
		return arr;

	spa_list_for_each(n, &g->nodes, link)
		arr.nodes[i++] = n;

	return arr;
}

static int topological_sort(dag_t *g, dag_node_t **out)
{
	node_array_t arr = dag_nodes_to_array(g);
	uint32_t front = 0, back = 0, idx = 0;

	if (arr.count == 0) {
		free(arr.nodes);
		return 0;
	}
	if (!arr.nodes)
		return -1;

	int *indegree = calloc(arr.count, sizeof(int));
	if (!indegree) {
		free(arr.nodes);
		return -1;
	}

	for (uint32_t i = 0; i < arr.count; i++) {
		dag_edge_t *e;
		int in_deg = 0;

		spa_list_for_each(e, &arr.nodes[i]->incoming, dst_link)
			in_deg++;

		indegree[i] = in_deg;
	}

	int *queue = calloc(arr.count, sizeof(int));
	if (!queue) {
		free(indegree);
		free(arr.nodes);
		return -1;
	}

	for (uint32_t i = 0; i < arr.count; i++) {
		if (indegree[i] == 0)
			queue[back++] = (int)i;
	}

	while (front < back) {
		int u = queue[front++];
		dag_edge_t *edge;

		out[idx++] = arr.nodes[u];

		spa_list_for_each(edge, &arr.nodes[u]->outgoing, src_link) {
			int v = -1;

			for (uint32_t k = 0; k < arr.count; k++) {
				if (arr.nodes[k] == edge->dst) {
					v = (int)k;
					break;
				}
			}
			if (v >= 0) {
				indegree[v]--;
				if (indegree[v] == 0)
					queue[back++] = v;
			}
		}
	}

	free(queue);
	free(indegree);
	free(arr.nodes);

	if (idx != arr.count) {
		errno = EINVAL;
		return -1;
	}

	return (int)idx;
}

static void find_sources_and_sinks(dag_t *g, dag_node_t ***sources, uint32_t *nsources,
		dag_node_t ***sinks, uint32_t *nsinks)
{
	uint32_t count = dag_list_len(&g->nodes);
	dag_node_t **sarr = calloc(count, sizeof(*sarr));
	dag_node_t **tarr = calloc(count, sizeof(*tarr));
	uint32_t si = 0, ti = 0;
	dag_node_t *n;

	spa_list_for_each(n, &g->nodes, link) {
		dag_edge_t *e;
		bool has_in = false;
		bool has_out = false;

		if (n->fictitious)
			continue;

		spa_list_for_each(e, &n->incoming, dst_link) {
			if (!e->src->fictitious) {
				has_in = true;
				break;
			}
		}

		spa_list_for_each(e, &n->outgoing, src_link) {
			if (!e->dst->fictitious) {
				has_out = true;
				break;
			}
		}

		if (!has_in)
			sarr[si++] = n;
		if (!has_out)
			tarr[ti++] = n;
	}

	*sources = sarr;
	*nsources = si;
	*sinks = tarr;
	*nsinks = ti;
}

static int dag_add_fictitious_endpoints(dag_t *g, dag_node_t **sources, uint32_t nsources,
		dag_node_t **sinks, uint32_t nsinks)
{
	if (dag_add_node(g, DAG_FICTITIOUS_SOURCE_ID, 0, -1, true) < 0)
		return -1;

	if (dag_add_node(g, DAG_FICTITIOUS_SINK_ID, 0, -1, true) < 0)
		return -1;

	for (uint32_t i = 0; i < nsources; i++) {
		if (dag_add_edge(g, DAG_FICTITIOUS_SOURCE_ID, sources[i]->id) < 0)
			return -1;
	}

	for (uint32_t i = 0; i < nsinks; i++) {
		if (dag_add_edge(g, sinks[i]->id, DAG_FICTITIOUS_SINK_ID) < 0)
			return -1;
	}

	return 0;
}

static void dag_populate_longest_paths(dag_t *g)
{
	for (uint32_t i = g->indexed_count; i > 0; i--) {
		dag_node_t *node = g->indexed_nodes[i - 1];
		dag_edge_t *e;
		uint64_t best_len = 0;
		int best_next = -1;

		node->longest_len = node->wcet;
		node->longest_next = -1;

		spa_list_for_each(e, &node->outgoing, src_link) {
			dag_node_t *child = e->dst;
			if (best_next < 0 || child->longest_len > best_len) {
				best_len = child->longest_len;
				best_next = (int)child->index;
			}
		}

		if (best_next >= 0) {
			node->longest_len = node->wcet + best_len;
			node->longest_next = best_next;
		}
	}
}

static uint64_t compute_longest_path(dag_t *g, dag_node_t *src, dag_node_t *dst,
		dag_node_t ***path_out, int *path_len_out)
{
	uint64_t total = 0;
	dag_node_t *node;
	dag_node_t **path;
	uint32_t count = 0;

	*path_out = NULL;
	*path_len_out = 0;

	if (!g || !src || !dst)
		return 0;

	if (src == dst) {
		path = malloc(sizeof(*path));
		if (!path)
			return 0;

		path[0] = src;
		*path_len_out = 1;
		*path_out = path;
		return src->wcet;
	}

	node = src;
	while (node != NULL) {
		count++;
		if (count > g->indexed_count)
			return 0;
		if (node == dst)
			break;
		if (node->longest_next < 0 || (uint32_t)node->longest_next >= g->indexed_count)
			return 0;
		node = g->indexed_nodes[node->longest_next];
	}

	if (node != dst)
		return 0;

	path = malloc((size_t)count * sizeof(*path));
	if (!path)
		return 0;

	node = src;
	for (uint32_t i = 0; i < count; i++) {
		path[i] = node;
		total += node->wcet;
		if (node == dst)
			break;
		node = g->indexed_nodes[node->longest_next];
	}

	*path_out = path;
	*path_len_out = (int)count;
	return total;
}

static int dag_build_indexed_nodes(dag_t *g)
{
	uint32_t count = dag_list_len(&g->nodes);
	int length;

	if (count == 0)
		return 0;

	g->indexed_nodes = calloc(count, sizeof(*g->indexed_nodes));
	if (!g->indexed_nodes)
		return -1;

	length = topological_sort(g, g->indexed_nodes);
	if (length < 0 || (uint32_t)length != count) {
		dag_free_indexed_nodes(g);
		errno = EINVAL;
		return -1;
	}

	g->indexed_count = count;
	for (uint32_t i = 0; i < count; i++)
		g->indexed_nodes[i]->index = i;

	return 0;
}

static int dag_build_successors(dag_t *g)
{
	uint32_t count = g->indexed_count;

	if (count == 0)
		return 0;

	for (uint32_t i = 0; i < count; i++) {
		g->indexed_nodes[i]->successors = bitset_alloc((int)count);
		if (!g->indexed_nodes[i]->successors)
			return -1;
	}

	for (uint32_t i = count; i > 0; i--) {
		dag_node_t *node = g->indexed_nodes[i - 1];
		dag_edge_t *e;

		bitset_zero(node->successors, (int)count);
		bitset_set(node->successors, node->index);

		spa_list_for_each(e, &node->outgoing, src_link)
			bitset_or(node->successors, e->dst->successors, (int)count);
	}

	return 0;
}

static int dag_ensure_unrelated_capacity(dag_t *g, uint32_t needed)
{
	if (g->unrelated_capacity >= needed)
		return 0;

	uint32_t new_capacity = g->unrelated_capacity ? g->unrelated_capacity : 16;
	while (new_capacity < needed) {
		if (new_capacity > UINT32_MAX / 2) {
			errno = ENOMEM;
			return -1;
		}
		new_capacity *= 2;
	}

	bitset_t **unrelated = realloc(g->unrelated, (size_t)new_capacity * sizeof(*unrelated));
	if (!unrelated)
		return -1;

	for (uint32_t i = g->unrelated_capacity; i < new_capacity; i++)
		unrelated[i] = NULL;

	g->unrelated = unrelated;
	g->unrelated_capacity = new_capacity;
	return 0;
}

static int dag_add_unrelated(dag_t *g, bitset_t *set)
{
	for (uint32_t i = 0; i < g->unrelated_size; i++) {
		if (bitset_includes(g->unrelated[i], set, (int)g->indexed_count)) {
			return 0;
		} else if (bitset_includes(set, g->unrelated[i], (int)g->indexed_count)) {
			bitset_cpy(g->unrelated[i], set, (int)g->indexed_count);
			return 0;
		}
	}

	if (dag_ensure_unrelated_capacity(g, g->unrelated_size + 1) < 0)
		return -1;

	if (!g->unrelated[g->unrelated_size]) {
		g->unrelated[g->unrelated_size] = bitset_alloc((int)g->indexed_count);
		if (!g->unrelated[g->unrelated_size])
			return -1;
	}

	bitset_cpy(g->unrelated[g->unrelated_size], set, (int)g->indexed_count);
	g->unrelated_size++;
	return 1;
}

static void dag_unrelated_squash(dag_t *g)
{
	for (uint32_t s1 = 0; s1 < g->unrelated_size; s1++) {
		for (uint32_t s2 = s1 + 1; s2 < g->unrelated_size; s2++) {
			if (bitset_includes(g->unrelated[s1], g->unrelated[s2], (int)g->indexed_count)) {
				bitset_t *tmp = g->unrelated[s2];
				g->unrelated[s2] = g->unrelated[g->unrelated_size - 1];
				g->unrelated[g->unrelated_size - 1] = tmp;
				g->unrelated_size--;
				s2--;
			} else if (bitset_includes(g->unrelated[s2], g->unrelated[s1], (int)g->indexed_count)) {
				bitset_t *tmp = g->unrelated[s1];
				g->unrelated[s1] = g->unrelated[g->unrelated_size - 1];
				g->unrelated[g->unrelated_size - 1] = tmp;
				g->unrelated_size--;
				s1--;
				break;
			}
		}
	}
}

static void dag_build_antichain_initial_stack(dag_t *g, bitset_t *stack)
{
	bitset_zero(stack, (int)g->indexed_count);

	for (uint32_t i = 0; i < g->indexed_count; i++) {
		if (!g->indexed_nodes[i]->fictitious)
			bitset_set(stack, i);
	}
}

		dag_node_t *node = g->indexed_nodes[i];
		if (node->fictitious || spa_list_is_empty(&node->outgoing))
			continue;

	bitset_cpy(dst, src, (int)g->indexed_count);
	bitset_nclear(dst, 0, (int)chosen_index);
	bitset_andnot(dst, chosen->successors, (int)g->indexed_count);

		dag_edge_t *e;
		spa_list_for_each(e, &node->outgoing, src_link) {
			if (e->dst->fictitious)
				continue;
			bitset_set(cut, e->dst->index);
		}

		if (node->fictitious || bitset_test(node->successors, chosen_index))
			bitset_clear(dst, candidate);
	}
}

static bool dag_antichain_is_redundant(dag_t *g, bitset_t *stack,
		int last_added, uint32_t chosen_index)
{
	dag_node_t *chosen = g->indexed_nodes[chosen_index];
	int candidate = bitset_next_set(stack, (int)g->indexed_count, last_added);

	while (candidate >= 0 && (uint32_t)candidate < chosen_index) {
		if (!dag_nodes_are_related(g->indexed_nodes[candidate], chosen))
			return true;
		candidate = bitset_next_set(stack, (int)g->indexed_count, candidate);
	}

	return false;
}

static int dag_comp_unrelated_recur(dag_t *g, bitset_t *curr_cut,
		bitset_t *stack, int last_added)
{
	int candidate = bitset_next_set(stack, (int)g->indexed_count, last_added);

	while (candidate >= 0) {
		bitset_decl_cpy(new_cut, curr_cut, g->indexed_count);
		bitset_decl_zero(new_stack, (int)g->indexed_count);

		bitset_set(new_cut, candidate);
		dag_build_antichain_next_stack(g, new_stack, stack, (uint32_t)candidate);

		if (bitset_empty(new_stack, (int)g->indexed_count)) {
			if (!dag_antichain_is_redundant(g, stack, last_added, (uint32_t)candidate) &&
					dag_add_unrelated(g, new_cut) < 0)
				return -1;
		} else if (dag_comp_unrelated_recur(g, new_cut, new_stack, candidate) < 0) {
			return -1;
		}

		candidate = bitset_next_set(stack, (int)g->indexed_count, candidate);
	}

	return 0;
}

static int dag_comp_unrelated(dag_t *g)
{
	bitset_decl_zero(curr_cut, (int)g->indexed_count);
	bitset_decl_zero(stack, (int)g->indexed_count);

	for (uint32_t i = 0; i < nsources; i++)
		bitset_set(curr_cut, sources[i]->index);

	if (!bitset_empty(curr_cut, (int)g->indexed_count)) {
		if (dag_add_unrelated(g, curr_cut) < 0)
			return -1;
		if (dag_comp_unrelated_recur(g, curr_cut) < 0)
			return -1;
	}

	dag_unrelated_squash(g);
	return 0;
}

static int dag_build_analysis(dag_t *g, dag_node_t **sources, uint32_t nsources, dag_node_t **sinks, uint32_t nsinks)
{
	if (dag_remove_fictitious_nodes(g) < 0)
		goto error;

	dag_invalidate_analysis(g);

	if (dag_add_fictitious_endpoints(g, sources, nsources, sinks, nsinks) < 0)
		goto error;

	if (dag_build_indexed_nodes(g) < 0)
		goto error;

	if (dag_comp_relatives(g) < 0)
		goto error;

	dag_populate_longest_paths(g);

	if (dag_comp_unrelated(g, sources, nsources) < 0)
		goto error;

	dag_populate_longest_paths(g);

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	if (dag_comp_unrelated(g) < 0)
		goto error;
	clock_gettime(CLOCK_MONOTONIC, &end);
	double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
	pw_log_error("Unrelated set computation took %.6f seconds", elapsed);

	return 0;

error:
	dag_remove_fictitious_nodes(g);
	dag_invalidate_analysis(g);
	return -1;
}

static int assign_path_head_deadline(dag_node_t **path, int path_len, uint64_t D, uint64_t L,
		uint64_t *assigned_head)
{
	dag_node_t *n_src;
	bool *excluded;
	bool changed;

	if (!path || path_len <= 0) {
		errno = EINVAL;
		return -1;
	}

	n_src = path[0];

	if (L == 0) {
		if (!n_src->deadline_assigned) {
			n_src->deadline = 0;
			n_src->deadline_assigned = true;
		} else if (n_src->deadline > 0) {
			n_src->deadline = 0;
		}
		if (assigned_head)
			*assigned_head = n_src->deadline;
		return 0;
	}

	excluded = calloc((size_t)path_len, sizeof(*excluded));
	if (!excluded)
		return -1;

	changed = true;
	while (changed) {
		changed = false;

		if (D == 0 || L == 0)
			break;

		for (int i = 0; i < path_len; i++) {
			dag_node_t *ni;
			uint64_t d_prime;

			if (excluded[i])
				continue;

			ni = path[i];
			d_prime = ni->wcet == 0 ? 0 :
				(uint64_t)floor((double)D * ((double)ni->wcet / (double)L));

			if (ni->deadline_assigned && ni->deadline < d_prime) {
				D = D > ni->deadline ? D - ni->deadline : 0;
				L = L > ni->wcet ? L - ni->wcet : 0;
				excluded[i] = true;
				changed = true;
				break;
			}
		}
	}

	if (D == 0 || L == 0) {
		if (!n_src->deadline_assigned) {
			n_src->deadline = 0;
			n_src->deadline_assigned = true;
		} else if (n_src->deadline > 0) {
			n_src->deadline = 0;
		}
		if (assigned_head)
			*assigned_head = n_src->deadline;
		free(excluded);
		return 0;
	}

	{
		uint64_t d_prime_src = n_src->wcet == 0 ? 0 :
			(uint64_t)floor((double)D * ((double)n_src->wcet / (double)L));

		if (!n_src->deadline_assigned) {
			n_src->deadline = d_prime_src;
			n_src->deadline_assigned = true;
		} else if (n_src->deadline > d_prime_src) {
			n_src->deadline = d_prime_src;
		}
	}

	if (assigned_head)
		*assigned_head = n_src->deadline;

	free(excluded);
	return 0;
}

static SPA_UNUSED int assign_deadlines_recursive(dag_t *g, dag_node_t *src, dag_node_t *dst, uint64_t D)
{
	if (src == dst) {
		uint64_t d_i = D;
		if (src->deadline_assigned) {
			if (src->deadline > d_i)
				src->deadline = d_i;
		} else {
			src->deadline = d_i;
			src->deadline_assigned = true;
		}
		return 0;
	}

	dag_node_t **P;
	int path_len;
	uint64_t L = compute_longest_path(g, src, dst, &P, &path_len);
	if (path_len == 0 || L == 0) {
		free(P);
		return -1;
	}

	uint64_t D_orig = D;
	uint64_t assigned_src = 0;

	if (assign_path_head_deadline(P, path_len, D, L, &assigned_src) < 0) {
		free(P);
		return -1;
	}

	dag_edge_t *e;
	spa_list_for_each(e, &src->outgoing, src_link) {
		if (e->dst == dst)
			continue;
		if (!bitset_test(e->dst->successors, dst->index))
			continue;

		uint64_t D_residual = (D_orig > assigned_src) ? (D_orig - assigned_src) : 0;
		int r = assign_deadlines_recursive(g, e->dst, dst, D_residual);
		if (r < 0) {
			free(P);
			return r;
		}
	}

	free(P);
	return 0;
}

static int assign_deadlines_iterative(dag_t *g)
{
	dag_node_t *fictitious_src = dag_find_fictitious_source(g);
	dag_node_t *fictitious_sink = dag_find_fictitious_sink(g);

	if (!fictitious_src || !fictitious_sink) {
		pw_log_error("missing fictitious endpoints for iterative deadline assignment");
		errno = EFAULT;
		return -1;
	}

	fictitious_src->deadline = 0;
	fictitious_src->deadline_assigned = true;
	fictitious_src->remaining_deadline = g->deadline;

	fictitious_sink->deadline = 0;
	fictitious_sink->deadline_assigned = true;
	fictitious_sink->remaining_deadline = 0;

	for (uint32_t i = 0; i < g->indexed_count; i++) {
		dag_node_t *node = g->indexed_nodes[i];
		dag_node_t **path = NULL;
		uint64_t available_deadline = 0;
		uint64_t assigned_deadline = 0;
		uint64_t path_len;
		bool has_predecessor = false;
		dag_edge_t *e;
		int path_nodes = 0;

		if (node == fictitious_src) {
			available_deadline = g->deadline;
		} else {
			spa_list_for_each(e, &node->incoming, dst_link) {
				uint64_t candidate = e->src->remaining_deadline;

				if (!has_predecessor || candidate < available_deadline)
					available_deadline = candidate;
				has_predecessor = true;
			}

			if (!has_predecessor) {
				pw_log_error("node %u has no predecessor while assigning iterative deadlines",
						node->id);
				errno = EFAULT;
				return -1;
			}
		}

		node->remaining_deadline = available_deadline;
		path_len = compute_longest_path(g, node, fictitious_sink, &path, &path_nodes);
		if (path_nodes == 0 || (node != fictitious_sink && path_len == 0)) {
			pw_log_error("failed to compute longest path from node %u to fictitious sink",
					node->id);
			free(path);
			errno = EFAULT;
			return -1;
		}

		if (assign_path_head_deadline(path, path_nodes, available_deadline, path_len,
					&assigned_deadline) < 0) {
			free(path);
			return -1;
		}

		node->remaining_deadline = available_deadline > assigned_deadline ?
			available_deadline - assigned_deadline : 0;
		free(path);
	}

	return 0;
}

struct cpu_assignment_info {
	dag_node_t *node;
	uint32_t index;
	double util;
};

static int compare_density_desc(double a_density, uint32_t a_key,
		double b_density, uint32_t b_key)
{
	if (a_density < b_density)
		return 1;
	if (a_density > b_density)
		return -1;
	if (a_key > b_key)
		return 1;
	if (a_key < b_key)
		return -1;
	return 0;
}

static int compare_cpu_assignment_info_desc(const void *a, const void *b)
{
	const struct cpu_assignment_info *info_a = a;
	const struct cpu_assignment_info *info_b = b;

	/* Place denser tasks first. Equal-density nodes keep topological order. */
	return compare_density_desc(info_a->util, info_a->index,
			info_b->util, info_b->index);
}

static bool prefer_cpu_choice(double projected, uint32_t cpu,
		double best_projected, int best_cpu)
{
	if (best_cpu < 0)
		return true;
	if (projected < best_projected)
		return true;

	/* When two CPUs yield the same projected load, prefer the lowest index. */
	return projected == best_projected && (int) cpu < best_cpu;
}

static int assign_cpus(dag_t *g)
{
	uint32_t count = g->indexed_count;
	uint32_t unrelated_size = g->unrelated_size;
	uint32_t num_cpus = g->num_cpus;

	if (count == 0)
		return 0;

	for (uint32_t i = 0; i < count; i++) {
		if (g->indexed_nodes[i]->fictitious)
			continue;
		if (!g->indexed_nodes[i]->deadline_assigned) {
			pw_log_error("Node %u has no assigned deadline", g->indexed_nodes[i]->id);
			errno = EFAULT;
			return -1;
		}
	}

	struct {
		dag_node_t *n;
		double util;
	} *info = calloc(count, sizeof(*info));
	if (!info)
		return -1;

	for (uint32_t i = 0; i < count; i++) {
		if (g->indexed_nodes[i]->fictitious) {
			info[i].node = g->indexed_nodes[i];
			info[i].index = g->indexed_nodes[i]->index;
			info[i].util = 0.0;
			continue;
		}

		double denom = (double)((g->indexed_nodes[i]->deadline < g->period) ?
				g->indexed_nodes[i]->deadline : g->period);
		if (denom == 0.0) {
			pw_log_error("Node %u has zero deadline denominator", g->indexed_nodes[i]->id);
			free(info);
			errno = EFAULT;
			return -1;
		}

		info[i].n = g->indexed_nodes[i];
		info[i].util = ((double)g->indexed_nodes[i]->wcet) / denom;
	}

	for (uint32_t x = 0; x + 1 < count; x++) {
		for (uint32_t y = x + 1; y < count; y++) {
			if (info[x].util < info[y].util) {
				double tmpu = info[x].util;
				dag_node_t *tmpn = info[x].n;

				info[x].util = info[y].util;
				info[x].n = info[y].n;
				info[y].util = tmpu;
				info[y].n = tmpn;
			}
		}
	}

	double *cpu_peak = calloc(num_cpus, sizeof(*cpu_peak));
	double *cpu_set_util = calloc((size_t)num_cpus * unrelated_size, sizeof(*cpu_set_util));
	if (!cpu_peak || (!cpu_set_util && unrelated_size > 0)) {
		free(cpu_set_util);
		free(cpu_peak);
		free(info);
		return -1;
	}

	for (uint32_t i = 0; i < count; i++) {
		if (info[i].node->fictitious)
			continue;

		double u = info[i].util;
		int chosen = -1;
		double max_margin = -DBL_MAX;

		for (uint32_t c = 0; c < num_cpus; c++) {
			double projected = cpu_peak[c] > u ? cpu_peak[c] : u;

			for (uint32_t s = 0; s < unrelated_size; s++) {
				if (!bitset_test(g->unrelated[s], info[i].n->index))
					continue;

				double candidate = cpu_set_util[(size_t)c * unrelated_size + s] + u;
				if (candidate > projected)
					projected = candidate;
			}

			double margin = g->utilization - projected;
			if (margin >= 0.0 && margin > max_margin) {
				max_margin = margin;
				chosen = (int)c;
			}
		}

		if (chosen < 0) {
			free(cpu_set_util);
			free(cpu_peak);
			free(info);
			errno = EAGAIN;
			return -1;
		}

		info[i].n->cpu = (uint32_t)chosen;
		double projected = cpu_peak[chosen] > u ? cpu_peak[chosen] : u;

		for (uint32_t s = 0; s < unrelated_size; s++) {
			size_t offset = (size_t)chosen * unrelated_size + s;
			if (!bitset_test(g->unrelated[s], info[i].n->index))
				continue;

			cpu_set_util[offset] += u;
			if (cpu_set_util[offset] > projected)
				projected = cpu_set_util[offset];
		}
		cpu_peak[chosen] = projected;
	}

	free(cpu_set_util);
	free(cpu_peak);
	free(info);
	return 0;
}

int dag_recalculate(dag_t *g)
{
	// register initial time
	struct timespec start_time;
	clock_gettime(CLOCK_MONOTONIC, &start_time);

	int ret;

	if (!g) {
		errno = EINVAL;
		return -1;
	}

	if (spa_list_is_empty(&g->nodes)) {
		dag_invalidate_analysis(g);
		return 0;
	}

	dag_invalidate_schedule(g);

	dag_node_t **sources, **sinks;
	uint32_t nsources, nsinks;
	find_sources_and_sinks(g, &sources, &nsources, &sinks, &nsinks);

	if (nsources == 0 || nsinks == 0) {
		free(sources);
		free(sinks);
		errno = EINVAL;
		return -1;
	}

	ret = dag_build_analysis(g, sources, nsources, sinks, nsinks);
	if (ret != 0) {
		free(sources);
		free(sinks);
		return ret > 0 ? dag_recalculate(g) : -1;
	}

	if (assign_deadlines_iterative(g) < 0) {
		free(sources);
		free(sinks);
		return -1;
	}

	free(sources);
	free(sinks);

	if (assign_cpus(g) < 0)
		return -1;

	//dag_print(g); // DEBUG
	// register end time and log duration
	struct timespec end_time;
	clock_gettime(CLOCK_MONOTONIC, &end_time);
	double duration = (end_time.tv_sec - start_time.tv_sec) +
		(end_time.tv_nsec - start_time.tv_nsec) / 1e9;
	pw_log_info("DAG recalculation completed in %.6f seconds", duration);
	return 0;
}

int dag_foreach_node(dag_t *g, dag_node_callback_t cb, void *data)
{
	if (!g || !cb) {
		errno = EINVAL;
		return -1;
	}

	dag_node_t *n;
	spa_list_for_each(n, &g->nodes, link) {
		if (!n->fictitious && !n->deadline_assigned) {
			if (dag_recalculate(g) < 0)
				return -1;
			break;
		}
	}

	spa_list_for_each(n, &g->nodes, link) {
		if (n->fictitious)
			continue;
		cb(data, n->tid, n->wcet, n->deadline, g->period, n->cpu);
	}

	return 0;
}

void dag_node_dump_unrelated(dag_t *g) {
  for (uint32_t i = 0; i < g->unrelated_size; i++) {
    printf("{ ");
    int k = 0;
    for (uint32_t j = 0; j < g->indexed_count; j++)
      if (bitset_test(g->unrelated[i], j))
        printf("%s%d", k++ > 0 ? ", " : "", j);
    printf(" }%s", i < g->unrelated_size - 1 ? ", " : "");
  }
  printf("\n");
}

void dag_print(dag_t *g)
{
	dag_node_t *n;
	spa_list_for_each(n, &g->nodes, link) {
		pw_log_debug("Node %u|%u|%u with wcet %lu and deadline %lu on cpu %u", n->id, n->index, n->tid, n->wcet, n->deadline, n->cpu);
		dag_edge_t *e;
		spa_list_for_each(e, &n->outgoing, src_link)
			pw_log_debug("  Edge to node %u|%u", e->dst->id, e->dst->tid);
		pw_log_debug("  Relatives:");
		for (uint32_t i = 0; i < g->indexed_count; i++) {
			if (dag_nodes_are_related(n, g->indexed_nodes[i]))
				pw_log_debug("    Node %u|%u|%u", g->indexed_nodes[i]->id, g->indexed_nodes[i]->index, g->indexed_nodes[i]->tid);
		}
	}
	pw_log_debug("Unrelated sets:");
	dag_node_dump_unrelated(g);
}
