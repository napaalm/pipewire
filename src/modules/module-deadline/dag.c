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

#define DAG_NODE_INDEX_INVALID UINT32_MAX

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

static void dag_invalidate_schedule(dag_t *g)
{
	dag_node_t *n;

	if (!g)
		return;

	spa_list_for_each(n, &g->nodes, link) {
		n->deadline_assigned = false;
		n->deadline = 0;
	}
}

static void dag_free_relatives(dag_t *g)
{
	if (!g || !g->relatives)
		return;

	free(g->relatives[0]);
	free(g->relatives);
	g->relatives = NULL;
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

	spa_list_for_each(n, &g->nodes, link)
		n->index = DAG_NODE_INDEX_INVALID;

	dag_free_relatives(g);
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

int dag_add_node(dag_t *g, uint32_t id, uint64_t wcet, pid_t tid,
		bool is_audio_source, bool is_audio_sink)
{
	if (!g) {
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
	n->is_audio_source = is_audio_source;
	n->is_audio_sink = is_audio_sink;
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
	free(n);
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
		bool has_in = !spa_list_is_empty(&n->incoming);
		bool has_out = !spa_list_is_empty(&n->outgoing);

		if (!has_in && n->is_audio_source)
			sarr[si++] = n;
		if (!has_out && n->is_audio_sink)
			tarr[ti++] = n;
	}

	*sources = sarr;
	*nsources = si;
	*sinks = tarr;
	*nsinks = ti;
}

static uint64_t compute_longest_path(dag_t *g, dag_node_t *src, dag_node_t *dst,
		dag_node_t ***path_out, int *path_len_out)
{
	*path_out = NULL;
	*path_len_out = 0;

	if (src == dst) {
		dag_node_t **p = malloc(sizeof(dag_node_t *));
		if (!p)
			return 0;

		p[0] = src;
		*path_len_out = 1;
		*path_out = p;
		return src->wcet;
	}

	int n = (int)dag_list_len(&g->nodes);
	if (n == 0) {
		return 0;
	}

	dag_node_t **topo = malloc((size_t)n * sizeof(dag_node_t *));
	if (!topo) {
		return 0;
	}

	int length = topological_sort(g, topo);
	if (length < 0) {
		free(topo);
		return 0;
	}

	int src_idx = -1, dst_idx = -1;
	for (int i = 0; i < length; i++) {
		if (topo[i] == src)
			src_idx = i;
		if (topo[i] == dst)
			dst_idx = i;
	}

	if (src_idx < 0 || dst_idx < 0 || src_idx > dst_idx) {
		free(topo);
		return 0;
	}

	uint64_t *dist = calloc((size_t)length, sizeof(uint64_t));
	int *parent = malloc((size_t)length * sizeof(int));
	if (!dist || !parent) {
		free(dist);
		free(parent);
		free(topo);
		return 0;
	}

	dist[src_idx] = topo[src_idx]->wcet;
	for (int i = 0; i < length; i++)
		parent[i] = -1;

	for (int i = src_idx; i <= dst_idx; i++) {
		dag_node_t *u = topo[i];
		dag_edge_t *e;

		spa_list_for_each(e, &u->outgoing, src_link) {
			int v = -1;

			for (int k = i + 1; k <= dst_idx; k++) {
				if (topo[k] == e->dst) {
					v = k;
					break;
				}
			}
			if (v >= 0 && dist[i] + e->dst->wcet > dist[v]) {
				dist[v] = dist[i] + e->dst->wcet;
				parent[v] = i;
			}
		}
	}

	uint64_t L = dist[dst_idx];
	int path_len = 0;
	for (int cur = dst_idx; cur != -1; cur = parent[cur])
		path_len++;

	dag_node_t **p = malloc((size_t)path_len * sizeof(dag_node_t *));
	if (!p) {
		free(dist);
		free(parent);
		free(topo);
		return 0;
	}

	int pos = path_len - 1;
	for (int cur = dst_idx; cur != -1; cur = parent[cur])
		p[pos--] = topo[cur];

	free(dist);
	free(parent);
	free(topo);

	*path_out = p;
	*path_len_out = path_len;
	return L;
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

static int dag_allocate_relatives(dag_t *g)
{
	size_t count = g->indexed_count;
	int *data;

	if (count == 0)
		return 0;

	g->relatives = calloc(count, sizeof(*g->relatives));
	if (!g->relatives)
		return -1;

	data = calloc(count * count, sizeof(*data));
	if (!data) {
		free(g->relatives);
		g->relatives = NULL;
		return -1;
	}

	for (size_t i = 0; i < count; i++)
		g->relatives[i] = data + (i * count);

	return 0;
}

static void dag_mark_descendants(dag_t *g, dag_node_t *root, dag_node_t *node, bool *visited)
{
	dag_edge_t *e;

	spa_list_for_each(e, &node->outgoing, src_link) {
		dag_node_t *dst = e->dst;

		if (visited[dst->index])
			continue;

		visited[dst->index] = true;
		g->relatives[root->index][dst->index] = 1;
		g->relatives[dst->index][root->index] = 1;
		dag_mark_descendants(g, root, dst, visited);
	}
}

static int dag_comp_relatives(dag_t *g)
{
	if (dag_allocate_relatives(g) < 0)
		return -1;

	for (uint32_t i = 0; i < g->indexed_count; i++) {
		bool *visited = calloc(g->indexed_count, sizeof(*visited));
		if (!visited)
			return -1;

		g->relatives[i][i] = 1;
		visited[i] = true;
		dag_mark_descendants(g, g->indexed_nodes[i], g->indexed_nodes[i], visited);
		free(visited);
	}

	return 0;
}

/* Removes all nodes that are not on a path from any source to any sink */
static int dag_remove_spurious_nodes(dag_t *g, dag_node_t **sources, uint32_t nsources, dag_node_t **sinks, uint32_t nsinks)
{
	int removed = 0;
	dag_node_t *n, *ntmp;
	spa_list_for_each_safe(n, ntmp, &g->nodes, link) {
		bool reachable_from_source = false;
		bool can_reach_sink = false;
		for (uint32_t i = 0; i < nsources; i++) {
			if (g->relatives[sources[i]->index][n->index]) {
				reachable_from_source = true;
				break;
			}
		}
		for (uint32_t i = 0; i < nsinks; i++) {
			if (g->relatives[n->index][sinks[i]->index]) {
				can_reach_sink = true;
				break;
			}
		}
		if (!reachable_from_source || !can_reach_sink) {
			if (dag_remove_node_ptr(g, n) < 0)
				return -1;
			removed++;
		}
	}
	return removed;
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

static bool dag_have_no_relatives(dag_t *g, bitset_t *set)
{
	for (uint32_t i = 0; i + 1 < g->indexed_count; i++) {
		if (!bitset_test(set, i))
			continue;

		for (uint32_t j = i + 1; j < g->indexed_count; j++) {
			if (bitset_test(set, j) && g->relatives[i][j])
				return false;
		}
	}

	return true;
}

static void dag_elemset_del_reachable(dag_t *g, bitset_t *set)
{
	bitset_decl_cpy(tmp, set, g->indexed_count);

	for (uint32_t i = 0; i + 1 < g->indexed_count; i++) {
		if (!bitset_test(set, i))
			continue;

		for (uint32_t j = i + 1; j < g->indexed_count; j++) {
			if (bitset_test(set, j) && g->relatives[i][j])
				bitset_clear(tmp, j);
		}
	}

	bitset_cpy(set, tmp, g->indexed_count);
}

static int dag_comp_unrelated_recur(dag_t *g, bitset_t *curr_cut)
{
	for (uint32_t i = 0; i < g->indexed_count; i++) {
		if (!bitset_test(curr_cut, i))
			continue;

		dag_node_t *node = g->indexed_nodes[i];
		if (spa_list_is_empty(&node->outgoing))
			continue;

		bitset_decl_cpy(cut, curr_cut, g->indexed_count);
		bitset_clear(cut, i);

		dag_edge_t *e;
		spa_list_for_each(e, &node->outgoing, src_link)
			bitset_set(cut, e->dst->index);

		dag_elemset_del_reachable(g, cut);

		if (!bitset_empty(cut, (int)g->indexed_count)) {
			if (!dag_have_no_relatives(g, cut)) {
				errno = EFAULT;
				return -1;
			}
			if (dag_add_unrelated(g, cut) < 0)
				return -1;
		}

		if (dag_comp_unrelated_recur(g, cut) < 0)
			return -1;
	}

	return 0;
}

static int dag_comp_unrelated(dag_t *g, dag_node_t **sources, uint32_t nsources)
{
	bitset_decl_zero(curr_cut, (int)g->indexed_count);

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
	int ret;
	dag_invalidate_analysis(g);

	if (dag_build_indexed_nodes(g) < 0)
		goto error;

	if (dag_comp_relatives(g) < 0)
		goto error;

	ret = dag_remove_spurious_nodes(g, sources, nsources, sinks, nsinks);
	if (ret < 0)
		goto error;
	if (ret > 0) {
		dag_invalidate_analysis(g);
		return ret;
	}

	if (dag_comp_unrelated(g, sources, nsources) < 0)
		goto error;

	return 0;

error:
	dag_invalidate_analysis(g);
	return -1;
}

static int assign_deadlines_recursive(dag_t *g, dag_node_t *src, dag_node_t *dst, uint64_t D)
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
	bool *excluded = calloc((size_t)path_len, sizeof(*excluded));
	if (!excluded) {
		free(P);
		return -1;
	}

	bool changed = true;
	while (changed) {
		changed = false;
		double D_d = (double)D;
		double L_d = (double)L;
		if (D_d <= 0.0 || L_d <= 0.0)
			break;

		for (int i = 0; i < path_len; i++) {
			if (excluded[i])
				continue;

			dag_node_t *ni = P[i];
			uint64_t C_i = ni->wcet;
			uint64_t d_prime = (uint64_t)floor(D_d * ((double)C_i / L_d));

			if (ni->deadline_assigned && ni->deadline < d_prime) {
				D = (D > ni->deadline) ? (D - ni->deadline) : 0;
				L = (L > C_i) ? (L - C_i) : 0;
				excluded[i] = true;
				changed = true;
				break;
			}
		}
	}

	double D_d = (double)D;
	double L_d = (double)L;

	if (D_d <= 0.0 || L_d <= 0.0) {
		free(excluded);
		free(P);
		return 0;
	}

	uint64_t assigned_src = 0;
	dag_node_t *n_src = P[0];
	uint64_t C_src = n_src->wcet;
	uint64_t d_prime_src = (uint64_t)floor(D_d * ((double)C_src / L_d));
	if (!n_src->deadline_assigned) {
		n_src->deadline = d_prime_src;
		n_src->deadline_assigned = true;
	} else if (n_src->deadline > d_prime_src) {
		n_src->deadline = d_prime_src;
	}
	assigned_src = n_src->deadline;

	dag_node_t *n_dst = P[path_len - 1];
	uint64_t C_dst = n_dst->wcet;
	uint64_t d_prime_dst = (uint64_t)floor(D_d * ((double)C_dst / L_d));

	if (!n_dst->deadline_assigned) {
		n_dst->deadline = d_prime_dst;
		n_dst->deadline_assigned = true;
	} else if (n_dst->deadline > d_prime_dst) {
		n_dst->deadline = d_prime_dst;
	}

	dag_edge_t *e;
	spa_list_for_each(e, &src->outgoing, src_link) {
		if (e->dst == dst)
			continue;
		if (!g->relatives[e->dst->index][dst->index])
			continue;

		uint64_t D_residual = (D_orig > assigned_src) ? (D_orig - assigned_src) : 0;
		int r = assign_deadlines_recursive(g, e->dst, dst, D_residual);
		if (r < 0) {
			free(excluded);
			free(P);
			return r;
		}
	}

	free(excluded);
	free(P);
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
	struct cpu_assignment_info *info;

	if (count == 0)
		return 0;

	for (uint32_t i = 0; i < count; i++) {
		if (!g->indexed_nodes[i]->deadline_assigned) {
			errno = EFAULT;
			return -1;
		}
	}

	info = calloc(count, sizeof(*info));
	if (!info)
		return -1;

	for (uint32_t i = 0; i < count; i++) {
		double denom = (double)((g->indexed_nodes[i]->deadline < g->period) ?
				g->indexed_nodes[i]->deadline : g->period);
		if (denom == 0.0) {
			free(info);
			errno = EFAULT;
			return -1;
		}

		info[i].node = g->indexed_nodes[i];
		info[i].index = g->indexed_nodes[i]->index;
		info[i].util = ((double)g->indexed_nodes[i]->wcet) / denom;
	}

	qsort(info, count, sizeof(*info), compare_cpu_assignment_info_desc);

	double *cpu_peak = calloc(num_cpus, sizeof(*cpu_peak));
	double *cpu_set_util = calloc((size_t)num_cpus * unrelated_size, sizeof(*cpu_set_util));
	if (!cpu_peak || (!cpu_set_util && unrelated_size > 0)) {
		free(cpu_set_util);
		free(cpu_peak);
		free(info);
		return -1;
	}

	for (uint32_t i = 0; i < count; i++) {
		double u = info[i].util;
		int chosen = -1;
		double chosen_projected = DBL_MAX;

		for (uint32_t c = 0; c < num_cpus; c++) {
			double projected = cpu_peak[c] > u ? cpu_peak[c] : u;

			for (uint32_t s = 0; s < unrelated_size; s++) {
				if (!bitset_test(g->unrelated[s], info[i].node->index))
					continue;

				double candidate = cpu_set_util[(size_t)c * unrelated_size + s] + u;
				if (candidate > projected)
					projected = candidate;
			}

			if (projected <= g->utilization &&
					prefer_cpu_choice(projected, c,
						chosen_projected, chosen)) {
				chosen_projected = projected;
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

		info[i].node->cpu = (uint32_t)chosen;
		double projected = cpu_peak[chosen] > u ? cpu_peak[chosen] : u;

		for (uint32_t s = 0; s < unrelated_size; s++) {
			size_t offset = (size_t)chosen * unrelated_size + s;
			if (!bitset_test(g->unrelated[s], info[i].node->index))
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

	for (uint32_t i = 0; i < nsources; i++) {
		for (uint32_t j = 0; j < nsinks; j++) {
			if (!g->relatives[sources[i]->index][sinks[j]->index])
				continue;

			if (assign_deadlines_recursive(g, sources[i], sinks[j], g->deadline) < 0) {
				free(sources);
				free(sinks);
				return -1;
			}
		}
	}

	free(sources);
	free(sinks);

	if (assign_cpus(g) < 0)
		return -1;

	//dag_print(g); // DEBUG

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
		if (!n->deadline_assigned) {
			if (dag_recalculate(g) < 0)
				return -1;
			break;
		}
	}

	spa_list_for_each(n, &g->nodes, link)
		cb(data, n->tid, n->wcet, n->deadline, g->period, n->cpu);

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
			if (g->relatives[n->index][i])
				pw_log_debug("    Node %u|%u|%u", g->indexed_nodes[i]->id, g->indexed_nodes[i]->index, g->indexed_nodes[i]->tid);
		}
	}
	pw_log_debug("Unrelated sets:");
	dag_node_dump_unrelated(g);
}
